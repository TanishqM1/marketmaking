Inbound market data = ITCH. Outbound orders = SBE. Real venues are asymmetric like this
(Nasdaq is ITCH in, OUCH out).

ITCH is big-endian, packed with no padding, message type is byte 0, and lengths are fixed per
type. SBE is little-endian, type is the templateId in the header, length is in the header.


Phase 1)
1. primitive types `types/` (Price, Qty, OrderRef, Side, Timestamp)
2. offset constants `codec/itch.hpp`
3. encoder + decoder for Add Order `codec/itch.cpp`
4. round trip test, encode -> decode -> compare `test/codec/`
5. repeat for the other 5
6. dispatch loop, read byte 0 -> switch -> decoder

We need an encoder even though ITCH is inbound, because the mock exchange has to publish data,
and it's how we make test fixtures.

Not doing yet: MoldUDP64, sockets, any SBE.


Don't cast a struct onto the buffer. ITCH is packed and big-endian so no struct matches the
wire, and casting is misaligned + UB. Use constexpr offsets, memcpy into a local, then byteswap.
A fixed size memcpy is one load so it's the same speed. Offsets go in codec/ not types/, since
they're wire knowledge.


These are the messages. Every one starts with the same 11 bytes:

```
0   msg type        1 (char)
1   stock locate    2       <- symbol id
3   tracking num    2
5   timestamp       6       <- ns since midnight
```

The actual "AAPL" string only shows up on Add Order. Everything else references the order ref.

```
A - Add Order (36 bytes)
11  order_ref       8
19  buy_sell        1 (B/S)
20  shares          4
24  stock           8 (char)
32  price           4       <- 4 implied decimals, 175.2500 = 1752500

E - Order Executed (31 bytes)
11  order_ref       8
19  executed_shares 4
23  match_number    8
        no price field. the trade was at the resting order's price, which we already have.

C - Order Executed With Price (36 bytes)
11  order_ref       8
19  executed_shares 4
23  match_number    8
31  printable       1
32  execution_price 4
        execution_price is just trade reporting. still decrement the level from our map.

X - Order Cancel (23 bytes)
11  order_ref       8
19  cancelled_shares 4

D - Order Delete (19 bytes)
11  order_ref       8
        no qty field at all, which is why we need the order map.

U - Order Replace (35 bytes)
11  original order_ref  8
19  new order_ref       8
27  shares              4
31  price               4
        its own message, not a version of X or D. it's a delete + add, and all future
        messages use the new ref.
```

F is Add Order + a 4 byte MPID at offset 36 (40 total). Skipping it, we don't care about MPID.

Verify all these offsets against the Nasdaq TotalView-ITCH 5.0 spec before coding.


Two annoying parts:
- byteswap every multi-byte field on read (std::byteswap in <bit>, or __builtin_bswap64). Swap
  once at decode, then everything internal is little-endian.
- the 6 byte timestamp. There's no 6 byte int, and reading 8 bytes overruns the message. memcpy
  6 bytes into a zeroed uint64 then swap. Get this one working by itself first.


What our book actually needs: shares at each price, bids descending and asks ascending. No
fills, no time priority, no per-order queues, no MPID.

But ITCH is order-based, so price levels don't exist on the wire, we build them. E C X D U only
give us an order ref (no price/side/symbol), so state/ needs order_ref -> {side, price,
remaining}. Every message except A starts by looking in it. We don't match orders, but we do
track every live one.

```
A       insert ref, level[price] += shares
E/C     level[price] -= executed, decrement remaining, erase if 0
X       level[price] -= cancelled, decrement remaining, erase if 0
D       level[price] -= ref.remaining, erase ref
U       subtract old remaining from old level, erase, insert new ref at new price
```

Partial fills are normal so decrement, don't delete.

The book never crosses. The exchange matches internally and just tells us what already happened.
An order that executes instantly never gets an A because it never rested, we just see the E
against the resting order, then an A for whatever's left over.

So if best bid >= best ask, that's our bug or a dropped packet. Never real.


Parse loop is: read byte 0, switch to the right decoder, memcpy + byteswap from the known
offsets, advance by that type's length, repeat. Lengths are A=36 E=31 C=36 X=23 D=19 U=35, fixed
per type, so we don't need a length prefix to parse.


Folders:
    types/          Price, Qty, OrderRef, Side. no wire knowledge, compiles on its own
    codec/itch.*    offsets, encoder, decoder. all the wire knowledge
    codec/schema/   SBE schema, later
    state/          order map + price levels, the actual market state
    transport/      sockets, MoldUDP64, later
    session/        seq numbers, gap detection, later
    test/codec/     round trip tests

types/ is just the vocabulary, state/ is the actual book, codec/ is anything that parses or
serializes.
