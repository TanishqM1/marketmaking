# Market Data

What arrives on the wire, how we parse it, and how feeds are delivered and recovered.

Inbound market data = **ITCH**. Outbound orders = **SBE**. Real venues are asymmetric like this
(Nasdaq is ITCH in, OUCH out).

| | ITCH (in) | SBE (out) |
|---|---|---|
| endianness | **big**, byteswap every field | little |
| packing | packed, no padding | we control it, can align |
| message type | byte 0, a char | `templateId` in the header |
| length | fixed per type | `blockLength` in the header |

---

## ITCH

### Message layouts
Every ITCH message starts with the same 11 bytes:
```
0   msg type        1 (char)
1   stock locate    2       <- symbol id, on EVERY message
3   tracking num    2       <- Nasdaq-internal, never read it
5   timestamp       6       <- ns since MIDNIGHT (not epoch)
```
The `"AAPL"` string appears only on Add Order; everything else references the order ref. An offset
is just the byte where a field starts: a 2-byte field at 1 occupies 1–2, so the next starts at 3.

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

X - Order Cancel (23)          partial: remove cancelled_shares
11  order_ref        8
19  cancelled_shares 4

D - Order Delete (19)          full: remove whatever remains
11  order_ref        8
    No qty field at all — which is why the order map is mandatory.

U - Order Replace (35)
11  original order_ref  8
19  new order_ref       8
27  shares              4
31  price               4
    Its own message, not a variant of X/D. A delete + add (side carried over from the
    original order), and all future messages use the NEW ref.
```
Book mapping: A/F → `add`, E/C → `execute`, X → `cancel`, D → `remove`, U → `replace`.

⚠ Verify every offset against the official Nasdaq TotalView-ITCH 5.0 spec.

**What the messages mean:**
- The exchange narrates already-resolved facts. An aggressive order that fills immediately never
  gets an `A` — you see the `E` against the resting order, then an `A` for any remainder.
- Only the resting side decrements on a fill. A venue's book never crosses, so `best_bid >=
  best_ask` is always our bug or a dropped packet.
- `find` returning null is normal (start of day, after a gap, already-erased order), not an error.

### Why not cast a struct onto the buffer
ITCH is packed and big-endian, so no struct matches the wire, and casting is misaligned → UB.
Use `constexpr` offsets, `memcpy` into a local, then byteswap. A fixed-size `memcpy` is one load,
so it's the same speed. Offsets live in `codec/`, not `types/` — they're wire knowledge.

- **Byteswap every multi-byte field** (`std::byteswap` in `<bit>`) once at decode; everything
  internal is little-endian after that. **Never byteswap text** — `"AAPL    "` becomes `"    LPAA"`.
- **The 6-byte timestamp.** No 6-byte int exists, and reading 8 overruns the message. Assemble with
  explicit shifts (big-endian, MSB first):
  ```cpp
  (uint64_t(p[0])<<40)|(uint64_t(p[1])<<32)|(uint64_t(p[2])<<24)
  |(uint64_t(p[3])<<16)|(uint64_t(p[4])<<8)|uint64_t(p[5]);
  ```
  The `uint64_t(...)` casts are load-bearing — shifting a 32-bit `int` by 40 is UB.
  6 bytes because 2⁴⁸ ≈ 2.8e14 fits a day in ns (8.64e13); 5 bytes covers only ~18 min.
- **Wire width ≠ in-memory width.** ITCH traded CPU convenience for bandwidth (2-byte locate, 4-byte
  price, no padding); the codec is the only layer that knows both.

### The parse loop
One datagram = **many messages** packed back to back, no padding. The venue batches (~42 bytes of
eth/ip/udp overhead per packet, and syscalls dominate a receive loop), so ~1400 bytes and ~40
messages per `recvfrom`.
```
offset:   0        36       67       90      109      144
         'A'      'E'      'X'      'D'      'U'
          |--36----|--31----|--23----|--19----|--35----|    len = 144
```
Type is at `buf[offset]`, not just `buf[0]`. Nothing says how many messages there are — you
discover it by walking: read the type → switch → memcpy + byteswap from known offsets → advance by
that type's length → repeat. Lengths A=36 F=40 E=31 C=36 X=23 D=19 U=35.

Two different numbers, don't mix them:
```
packet.size() - offset   bytes left in the buffer   -> bounds check: does this message fit?
X::length                this message's size        -> how far to advance
```
- **Check the header (11 bytes) first**, then the message: `if (X::length > packet.size() - offset)
  bail`. Written that way it can't overflow.
- **The loop should end at `offset == len` exactly.** A wrong length constant lands you mid-message,
  you read a payload byte as a type, and the rest of the datagram decodes to garbage.
- ⚠ **Unknown type → unknown length → you can't find the next message**; today the loop would spin
  on it. Accepted as an assumption (the feed only sends our 7 types) — a real feed opens with System
  Event `'S'`. Each case keeps its own length check on purpose — see [decisions.md](decisions.md).

### Our buffer
One fixed 64 KB array, allocated at startup, overwritten every read. Undersizing silently
truncates; 64 KB costs nothing (max UDP payload is 65,507). Anything pointing into it dies on the
next `recvfrom`, so decode fully, apply to the book, then read again. (Why no ring: UDP is
message-oriented — [hardware.md](hardware.md).)

---

## SBE (outbound, later)
8-byte header, always first:
```
0-1  blockLength   length of the PAYLOAD (not the total)
2-3  templateId    which message this is  <- SBE's name for "message type"
4-5  schemaId      sanity check (from the schema's id= attribute)
6-7  version       schema revision
8+   payload
```
Next message = `current + 8 + blockLength`. `blockLength` lets you skip a message you don't
understand and stay in sync, and lets fields be appended without breaking old readers.

- **templateId is read, never picked**, on the decode side — the schema defines it, the encoder
  writes it, the decoder reads it from a fixed offset.
- Keep templateIds **dense and small** (1, 2, 3…) so the compiler emits a jump table.
- ⚠ Don't shrink the header to 6 bytes: an 8-byte first field would be misaligned, and you'd pad
  back to 8 anyway.

---

## Feeds and recovery

### Why multicast, and why A/B feeds
**Multicast is forced by fanout, not by packet loss.** The exchange sends each packet once and
hundreds of subscribers pick it off the wire; unicast would mean N copies of every message.
```
fanout -> multicast -> UDP (multicast only exists for UDP; TCP is 1:1)
       -> no reliability -> need seq numbers + A/B + recovery
```
**A/B is redundancy on top.** The exchange publishes the *same* data twice, on two groups, usually
over separate physical paths. Take whichever **sequence number** arrives first, drop the duplicate.
- Latency is the **minimum** of two paths — A/B is faster, not slower. Cost is 2× packets.
- A drop on A is covered by B, so you skip recovery (the slow thing).
- ⚠ A/B doesn't guarantee delivery: both can drop, and **if we're too slow both sockets drop** in
  our own kernel buffer. It protects against network loss, not against us.
- ⚠ Compare sequence numbers, never payloads. The UDP checksum already covers corruption.

Ours is unicast only, no A/B yet (`udp_receiver.cpp` TODO).

### Gap recovery
Detect the gap from the sequence number (1038 → 1040), **immediately stop quoting that
instrument**, fetch the missing messages, apply them, resume.
```
1. A/B arbitration     the other feed already has it      free, usual case
2. retransmit request  "send me 1039"                     ~100 µs - several ms
3. snapshot resync     catastrophic loss — replay would flood us
```
- ⚠ Retransmission transport varies — Nasdaq's MoldUDP64 uses a UDP rewind server; CME and others
  use TCP.
- ⚠ "Stop quoting" is the point. A gap means the book is wrong and **silently stays wrong forever**.
  "Missed data" is recoverable; "my logic is broken" isn't — an invariant violation should fail
  loudly, because a crash stops but a wrong book keeps quoting.

Not built yet (`session/`) — the first error check a firm would build.
