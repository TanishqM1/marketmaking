# Inbound / Outbound Data

Inbound market data = **ITCH**. Outbound orders = **SBE**. Real venues are asymmetric like this
(Nasdaq is ITCH in, OUCH out).

| | ITCH (in) | SBE (out) |
|---|---|---|
| endianness | **big**, byteswap every field | little |
| packing | packed, no padding | we control it, can align |
| message type | byte 0, a char | `templateId` in the header |
| length | fixed per type (lookup table) | `blockLength` in the header |

### Message layouts
Every ITCH message starts with the same 11 bytes:
```
0   msg type        1 (char)
1   stock locate    2       <- symbol id, on EVERY message
3   tracking num    2       <- Nasdaq-internal, never read it
5   timestamp       6       <- ns since MIDNIGHT (not epoch)
```
The `"AAPL"` string appears only on Add Order; everything else references the order ref.

```
A - Add Order (36)
11  order_ref        8
19  buy_sell         1 (B/S)
20  shares           4
24  stock            8 (char)
32  price            4       <- 4 implied decimals: 175.2500 = 1752500

F - Add Order with MPID (40) = A + 4-byte MPID at 36. All of A's offsets apply.
    Skipping the MPID, but it IS an add — not handling it loses orders.

E - Order Executed (31)
11  order_ref        8
19  executed_shares  4
23  match_number     8      <- trade reporting only
    No price field: the trade was at the resting order's price, which the map has.

C - Order Executed With Price (36)
11/19/23 same as E
31  printable        1      <- "count this in trade stats?"  tape concern
32  execution_price  4      <- trade reporting. Still decrement the level per the map.
    Same fields we read as E, same offsets. ONLY the length differs (36 vs 31).
    Reuse the decoder, never the length.

X - Order Cancel (23)
11  order_ref        8
19  cancelled_shares 4

D - Order Delete (19)
11  order_ref        8
    No qty field at all — which is why the order map is mandatory.

U - Order Replace (35)
11  original order_ref  8
19  new order_ref       8
27  shares              4
31  price               4
    Its own message, not a variant of X/D. A delete + add, and all future
    messages use the NEW ref.
```

⚠ Verify every offset against the official Nasdaq TotalView-ITCH 5.0 spec.

### Why not cast a struct onto the buffer
ITCH is packed and big-endian, so no struct matches the wire, and casting is misaligned → UB.
Use `constexpr` offsets, `memcpy` into a local, then byteswap. A fixed-size `memcpy` is one load,
so it's the same speed. Offsets live in `codec/`, not `types/` — they're wire knowledge.

Two annoying parts:
- **Byteswap every multi-byte field** (`std::byteswap` in `<bit>`). Swap once at decode; everything
  internal is little-endian after that. **Never byteswap text** — `"AAPL    "` becomes `"    LPAA"`.
- **The 6-byte timestamp.** No 6-byte int exists, and reading 8 overruns the message. Assemble with
  explicit shifts (big-endian, MSB first) — no endianness assumption, no UB:
  ```cpp
  (uint64_t(p[0])<<40)|(uint64_t(p[1])<<32)|(uint64_t(p[2])<<24)
  |(uint64_t(p[3])<<16)|(uint64_t(p[4])<<8)|uint64_t(p[5]);
  ```
  The `uint64_t(...)` casts are load-bearing — shifting a 32-bit `int` by 40 is UB.
  6 bytes because 2⁴⁸ ≈ 2.8e14 fits a day in ns (8.64e13); 5 bytes covers only ~18 min.

### The parse loop
One datagram = **many messages** packed back to back, no padding. The venue batches (~42 bytes of
eth/ip/udp overhead per packet, and syscalls dominate a receive loop), so ~1400 bytes and ~40
messages per `recvfrom`.
```
offset:   0        36       67       90      109      144
         'A'      'E'      'X'      'D'      'U'
          |--36----|--31----|--23----|--19----|--35----|    len = 144
```
Each message is self-identifying: type is at `buf[offset]`, not just `buf[0]`. Nothing says how
many are in there — you discover it by walking.

Read `buf[offset]` → switch → memcpy+byteswap from known offsets → advance by that type's length →
repeat. Lengths A=36 F=40 E=31 C=36 X=23 D=19 U=35, fixed per type, so no length prefix is needed
to parse.

Two different numbers, don't mix them:
```
len - offset        bytes left in the buffer  -> bounds check: does this msg fit?
length_for(type)    this message's size       -> how far to advance
```

**Bounds check before every decode:** `if (msglen > len - offset) bail`. Written that way rather
than `offset + msglen > len` it can't overflow. Skip it and a truncated datagram reads past the
end. Also check the **header** (11 bytes) before reading the type's neighbours.

**Loop should end at `offset == len` exactly.** Anything else means truncation or a wrong length
constant — and a bad constant lands you mid-message, you read a payload byte as a type, and
everything after decodes to garbage. One wrong number desyncs the rest of the datagram.

⚠ Unknown type → unknown length → **you cannot find the next message.** `default:` must `return`,
or you re-read the same byte forever. A real feed opens the day with System Event (`'S'`), so this
hangs on message one. Needs a length table over all ~20 types to skip properly.

### Our buffer
One fixed 64 KB array, allocated at startup, overwritten every read. UDP is message-oriented so one
`recvfrom` = exactly one complete datagram — no ring buffer, no accumulation. Undersizing silently
truncates; 64 KB costs nothing (max payload is 65507). Anything pointing into it dies on the next
`recvfrom`, so decode fully, apply to the book, then read again.

### SBE (outbound, later)
8-byte header, always first:
```
0-1  blockLength   length of the PAYLOAD (not the total)
2-3  templateId    which message this is  <- SBE's name for "message type"
4-5  schemaId      sanity check (from the schema's id= attribute)
6-7  version       schema revision
8+   payload
```
Next message = `current + 8 + blockLength`.

`blockLength` exists so you can skip a message you don't understand and stay in sync, and so fields
can be appended without breaking old readers.

- **templateId is read, never picked**, on the decode side — it's already in the bytes at a fixed
  offset. The encoder writes it; the decoder reads it.
- Keep templateIds **dense and small** (1,2,3…) so the compiler emits a jump table.
- ⚠ Don't shrink the header to 6 bytes: the payload would start at offset 6, misaligning an 8-byte
  first field, and you'd pad back to 8 anyway.

### Folders
```
types/        Price, Qty, OrderRef, Side — no wire knowledge, compiles alone
codec/itch.*  offsets, encoder, decoder — all the wire knowledge
codec/schema/ SBE schema, later
state/        order map + price levels — the actual market state
transport/    sockets, MoldUDP64, later
session/      seq numbers, gap detection, later
test/codec/   round trip + hand-written-hex tests
```
`types/` is vocabulary, `state/` is the book, `codec/` is anything that parses or serializes.
