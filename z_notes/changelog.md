# Learning Changelog

Dated session log. Newest first. Topic sections mirror the other files in `z_notes/`.
Written by `/changelog <date>`.

---

## 2026-11-01 — Sun Nov 1, 2026

**Built:** `transport/udp_receiver.{hpp,cpp}` (socket, bind, `SO_RCVBUF`, `receive()` returning a
span) and `app/main.cpp`'s loop — the full ingestion path now runs end to end. Added `mm_transport`
+ `trading_system` CMake targets, LTO in Release, and `-DNDEBUG`. Started `state/order_map.hpp`;
wrote `test/state/test_order_map.cpp` (12 runtime tests + 6 layout assertions). Trimmed all notes
1221 → ~975 lines.

### C++
- **`noexcept` removes a landing pad, not just documentation.** If a function might throw the
  caller emits stack-unwinding machinery and the optimiser can't reorder across the call — and
  across a TU boundary the compiler must *assume* it might. See [cpp.md](cpp.md).
- **`explicit` blocks implicit conversion, which matters most when the constructor has side
  effects.** `UDP_Receiver r = 8080;` compiles without it, silently opening a socket.
- **`alignas` goes on the ARRAY, not the struct.** On the struct, `sizeof(OrderEntry)` becomes 128
  — 4× the memory and 1 entry per cache line instead of 4.
- **`static constexpr` members occupy zero bytes.** They're baked into the instruction stream, so
  `CAPACITY`/`MASK`/`EMPTY` cost nothing. A real member instead would waste 8 bytes per object.
- **Header-only vs `.cpp` is a size decision, not a style one.** `OrderMap::find` is tiny and
  per-message → header. `Book::add` is bigger → `.cpp`, and LTO can still inline it.

### Machine & OS
- **`&` is bitwise AND, and `ref & (CAPACITY-1)` *is* modulo for powers of two.** Keeps the low N
  bits. One instruction vs ~20-40 cycles for division — the whole reason CAPACITY must be 2ⁿ.
- **Cache line is 128 bytes here, 64 on x86.** `sysctl hw.cachelinesize` confirms. Most advice you
  read says 64.
- **Two separate requirements for cache efficiency: `sizeof` must divide 128, AND the array must be
  128-aligned.** One without the other still straddles, and reading one straddling entry costs two
  misses. See [opsys_machine_arch.md](opsys_machine_arch.md).
- **The latency cliff is the cache line, not the probe count.** 4 entries/line, so probes 1-4 are
  free and probe 5 doubles the cost. That's why load factor ≤ 0.5, not because 1.5 probes is "fast
  enough."
- **A TU boundary costs two opaque calls per packet, plus a duplicated test.** `main` re-checks
  `packet.empty()` on a value `receive()` already decided, because it can't see inside. LTO fixes
  it; verified `-flto=thin` on the Release link line.
- **The three HFT tricks are one idea.** Kernel bypass, ring buffers, and flat maps all trade
  generality for knowing something specific about the data. The question is "what do I know that
  `std::unordered_map` isn't allowed to assume?"

### Wire format
- **`recvfrom` returns only an `ssize_t`** — the byte count. Data arrives as a side effect. `n == 0`
  is a valid empty datagram, not EOF.
- **`flags` takes `MSG_*`, not `IPPROTO_*`.** `IPPROTO_UDP` is 17, so passing it silently sets two
  arbitrary MSG bits. Compiles fine — both params are plain `int`.
- **`MSG_DONTWAIT` is 128 on macOS, 64 on Linux.** Never hardcode the number.
- **Blocking vs busy-poll is one constant, because `receive()` returns a possibly-empty span.**
  Blocking sleeps the thread (2 context switches, jittery); busy-poll never sleeps.

### Data structures
- **An open-addressing hash map *is* a flat array.** "Not a hashmap" meant "not
  `std::unordered_map`" — the array is storage, the hash is just how you compute the index.
- **Linear probing wins despite worse theory.** Quadratic/double hashing scatter, so every probe is
  a fresh DRAM miss; linear's cluster is in the line you already paid for.
- **Order refs are roughly sequential, so their low bits are well distributed** — `ref & MASK` needs
  no hash function at all.
- **The key must be stored in each slot.** Without it you can't tell "mine" from "someone else's
  who collided here." 8 of the entry's bytes go to the key.
- **A tombstone is writable but not terminating.** Erasing to `EMPTY` breaks the probe chain for
  entries further along — they become unfindable. See
  [data_structures.md](data_structures.md).
- **ONE global order map, not one per symbol.** When an `E` arrives you have only the ref, so you
  couldn't pick which per-symbol map to search. The map is what *tells* you the symbol.
- **Backward-shift deletion beats tombstones for high churn.** Tombstones force a mid-session
  rehash, which contradicts "never rehash mid-session."

### Market structure
- **Position comes from the map; quantity comes from the message — except for `D` and `U`.**
  `E`/`C`/`X` carry a quantity; `D` carries nothing but a ref, so the amount comes from
  `remaining`. (Correcting an earlier over-generalisation of mine.)
- **`find` returning null is normal, not an error.** Start of day, after a gap, or an
  already-erased order — hundreds of times each morning.

### Bugs I hit
- **`slots_` declared without `{}` — silent, and would destroy everything.** Indeterminate bytes
  mean garbage refs, so `find()` matches at random or probes forever. Nobody catches it.
- **`sizeof(OrderEntry)` is 24, not 32 — caught by `static_assert`.** 128/24 isn't an integer so
  entries straddle lines. The assert turned it into a build failure instead of a mystery.
- **`class UDP_Receiver {}` written in *both* the header and the `.cpp` — loud at link.** That
  creates two unrelated classes sharing a name; the header's declarations had no definitions.
  The `.cpp` needs `UDP_Receiver::` prefixes and no `class` keyword.
- **Public data members in `OrderMap` — silent.** Any caller could corrupt the table and break
  every probe chain. The invariants only hold if `insert`/`find`/`erase` are the sole mutators.
- **I dropped `-DNDEBUG` when overriding `CMAKE_CXX_FLAGS_RELEASE`** — so every assert would have
  stayed live in the build used to measure latency. Found by code review, not by me.
- **Pasted LLM code with confidently wrong comments.** "recvfrom extracts bytes directly out of the
  NIC ring buffer", "`__builtin_expect` tells the CPU", "zero-copy handoff" — all wrong, all
  plausible-sounding. Also contained two real bugs: `continue` on error (spins at 100% CPU) and a
  hoisted `addrLen` that `recvfrom` writes to.

### Still open
- **`sizeof(OrderEntry)` padding to 32** — the `static_assert` is failing; one `pad[14]` away from
  the tests compiling. Then undefined symbols for `insert`/`find`/`erase` become the todo list.
- **`OrderMap` implementation** — and the erase strategy decision (tombstones vs backward-shift).
- **Issues filed:** #10 kernel-bypass research, #11 busy-polling + benchmark, #12 latency profiling,
  #13 error contract, #14 `length_for()` + the `default:` hang, #15 multicast + A/B feeds, #16
  inlining/alignment/dead code.
- **⚠ `default:` in `decode_packet` still doesn't advance `offset`** — a real ITCH feed opens with
  System Event (`'S'`), so it hangs on message one. Accepted for now since the mock exchange only
  emits our 7 types.

## 2026-10-29 — Thu Oct 29, 2026

**Built:** `codec/itch.hpp` + `codec/itch.cpp` — all six ITCH offset structs, `MsgType` enum,
`read_be16/32/64` helpers, `decode_add_order` returning `AddOrderFields`, and the `decode_packet`
dispatch loop with the Add Order path bounds-checked and advancing. Also `types/types.hpp` with
`mm::Side`, a `.clang-format`, and the `/savenotes` + `/changelog` skills.

### C++

- **`std::span` is `{pointer, size}`, non-owning.** It carries the length a raw pointer can't, so
  bounds checks are possible and ptr/len can't drift apart. Gives you bounds, not lifetime.
  See [cpp.md](cpp.md).
- **`using Price = int64_t` gives zero type safety** — it's an alias, same type. A one-field
  struct is a distinct type with identical codegen, provided you don't write a copy/move ctor or
  destructor (that flips the ABI to passing in memory instead of a register).
- **Locals live in registers, not memory.** Taking an address doesn't force memory if it doesn't
  escape; `memcpy` into a local compiles to one `ldr`. See [cpp.md](cpp.md).
- **Structs mostly don't exist in memory.** Inline → SROA → registers. They only materialise if
  they outlive the function, their address escapes, they're in an array, or they're >16 bytes and
  not inlined.
- **That's the as-if rule:** the compiler preserves observable behaviour, not the operations you
  wrote. Why `memcpy`, strong-typedef wrappers, and struct returns are all free.
- **`alignof` = largest member's alignment; `sizeof` rounds up to a multiple of it** so arrays
  stay aligned. Declare largest-first to avoid internal padding.
- **No 5- or 6-byte integer type exists** — CPUs have 8/16/32/64-bit load instructions and the
  language offers nothing else.
- **Wire width and in-memory width are independent decisions.** 6-byte timestamp → `uint64_t`,
  4-byte price → `int64_t`. The codec is the only layer that knows both.
- **`size_t` for sizes/indices/offsets, fixed-width for wire fields.** On this Mac they're the
  same width but *different types* (`unsigned long` vs `unsigned long long`).
- **`auto` when the RHS makes the type obvious; spell it out for numeric widths and strong
  types.** `auto x = 0` is an `int`, which is wrong for an index.
- **Anonymous namespace = internal linkage.** Compiler sees all call sites (better inlining),
  nothing external can call it, no cross-TU collisions. `.cpp` files only — in a header every TU
  gets its own private copy.
- **`using namespace X;` is not `namespace X { }`.** The first only affects lookup; definitions
  stay at global scope.
- **Enums are named integers, and a cast relabels rather than translates.** No validation, no
  default, no warning. See [cpp.md](cpp.md).
- **Enum names label *some* values, not the only legal ones.** With a fixed underlying type all
  256 values are valid.
- **Scoped enums default to `int`** — specify `: std::uint8_t` when size matters.
- **`[[likely]]`/`[[unlikely]]` affect static code layout, not the branch predictor.** The
  hardware predictor learns at runtime and is better at it; these decide which path is the
  fall-through and where cold code gets placed.

### Machine & OS

- **Kernel and userspace are separated by hardware** — privilege rings plus page permissions. A
  packet DMAs into kernel memory, queues on your socket, and `recvfrom` copies it out: **two
  copies**. See [opsys_machine_arch.md](opsys_machine_arch.md).
- **A syscall is a privilege transition, ~100ns–1µs**, usually the dominant cost in a receive
  loop. Hence batching, `recvmmsg`, and kernel bypass.
- **UDP is message-oriented**: one `recvfrom` = exactly one complete datagram, never partial. No
  accumulation logic needed. TCP is a byte stream and will need it.
- **L1 splits into L1i (code) and L1d (data).** This machine: 192KB/128KB on perf cores, 128-byte
  lines. x86 servers have ~32KB L1i, so the inlining budget here is ~6× the real target's.
- **Inlining is a cache budget, not a free win.** Large bodies inlined into the hot loop evict the
  loop itself.
- **Cache keys on address locality in the binary, not execution order.** Two functions called
  back-to-back but 200KB apart help each other not at all.
- **You can't load code into I-cache** — it's demand-filled. The goal is stopping cold code from
  evicting hot code: `[[gnu::hot/cold]]`, linker order files, PGO, BOLT.
- **Stack spills aren't cache misses** (~4 cycles, L1). The expensive access is the order-ref map:
  random, millions of entries, ~200 cycles.
- **ASCII:** `'A'`=65, `'B'`=66, `'S'`=83. A character literal *is* its code.

### Wire format

- **One datagram holds many messages**, packed with no padding. Venues batch to amortise ~42 bytes
  of packet overhead and the syscall. See [inbound_outbound_data.md](inbound_outbound_data.md).
- **ITCH messages don't carry their own length** — it's fixed per type, from a lookup table. You
  need lengths for all ~20 types, not just the six you decode, or an unknown message desyncs the
  rest of the packet.
- **Two different numbers:** `len - offset` is bytes remaining (bounds check), `length_for(type)`
  is this message's size (how far to advance).
- **Never byteswap text.** `read_be64` on `"AAPL    "` returns `"    LPAA"`.
- **ITCH's 6-byte timestamp is the smallest size fitting a day in nanoseconds** — 2⁴⁸ ≈ 2.8×10¹⁴
  vs 8.64×10¹³ needed; 5 bytes only covers ~18 minutes. Since-midnight, not since-epoch.
- **ITCH traded CPU convenience for bandwidth** — which is why it's unaligned and awkward: 2-byte
  `stock_locate` instead of the symbol string, 4-byte price, zero padding.
- **`stock_locate` is the symbol id, present on every message** — so the 8-char `stock` field can
  be skipped entirely.
- **SBE's header is `blockLength/templateId/schemaId/version`**, and `templateId` is just SBE's
  name for message type. Shrinking it to 6 bytes gains nothing — you'd pad back to 8 for
  alignment.

### Market structure

- **A market data book tracks state and never matches** — but it still does per-order bookkeeping,
  because ITCH is order-based and the aggregate price levels don't exist on the wire.
- **`E`/`C`/`X`/`D`/`U` carry only an order ref** — no price, side, or symbol. `D` has no quantity
  field at all, which is the proof the order-ref map is mandatory.
- **The exchange narrates already-resolved facts.** An aggressive order that fills immediately
  never gets an `A` (it never rested); you see the `E` against the resting order, then an `A` for
  the remainder.
- **Only the resting side decrements on a fill**, and `E` has no price field because the price *is*
  the resting order's price, which the map already holds.
- **A venue's book never crosses**, so `best_bid >= best_ask` is always our bug or a dropped
  packet.
- **Error detection is nearly free; the response is what costs.** The map lookup happens anyway,
  so checking it succeeded is one branch. Never format a log string on the hot path.
- **"Missed data" is recoverable, "my logic is broken" isn't.** Gap → mark stale, pull quotes,
  resync. Invariant violation → fail loudly, because a crash stops but a wrong book keeps quoting.

### Bugs I hit

- **Missing `break` in the side switch — silent.** `case 'B'` fell through into `case 'S'`, so
  every buy became a sell. No warning by default; `-Wimplicit-fallthrough` catches it.
- **`static_cast<Side>(byte)` — silent, and wrong on every message.** Gave `Side(66)`: legal,
  nameless, never equal to `Buy`. A cast relabels the bits; it doesn't translate. Downstream
  `if (side == Buy)` would be false always, so every order lands on the ask side and the book has
  **no bids at all**. Nobody catches this.
- **`std::byteswap(temp);` with the result discarded — silent.** It's a pure function. Every field
  came out byte-reversed.
- **`packet.subspan(offset);` with the result discarded — silent.** Same shape; `subspan` returns
  a new span rather than modifying in place, so the line was a no-op.
- **`uint16_t temp` with an 8-byte `memcpy` — stack buffer overflow, UB.** Writing 8 bytes into a
  2-byte object. Fixed structurally by `sizeof temp`, which ties the count to the type so they
  can't disagree.
- **`int32_t` for price — caught by `-Wsign-conversion`.** `int32_t` max with 4 implied decimals is
  $214,748.36, so any price above that would come out negative.
- **`side` uninitialized on the `default` path — caught by `-Wsometimes-uninitialized`,** which
  traced the exact route from `default:` to the struct construction.
- **`using namespace itch;` instead of `namespace itch { }` — loud.** Definitions stayed at global
  scope while the header declared them in `itch`, giving "call to 'read_be16' is ambiguous."
- **`size_t order_ref` — semantically wrong, no live bug here.** An order ref is an 8-byte wire
  field, not a size. Works only because `size_t` happens to be 64-bit on arm64; would read half
  the field on a 32-bit target.
- **Mixing offset-cursor and span-shrink — would have double-advanced** and skipped a message per
  iteration. They're alternatives, not partners.

**The pattern:** silent bugs outnumbered loud ones roughly 2:1, and the warning flags caught two
things that would otherwise have been debugged at the wire level.

### Still open

- **How a decoder reports a malformed message** — `std::expected<Fields, DecodeError>` is the
  likely answer. Blocking: `default:` has nowhere to go, `side` stays uninitialized, and all six
  decoders should share one shape. Decide before writing decoder #2.
- **`length_for(type)` over all ~20 ITCH types** — without it, `default:` can't advance and an
  unknown message is an infinite loop.
- **Header read can overrun** on a trailing 1–2 byte fragment: `read_be16` at `offset + 1` needs 3
  bytes, the loop only guarantees 1. Needs a `remaining < MessageHeader::size` check above the
  switch.
- **`mm::Price` / `mm::Qty` strong types don't exist yet**, so `AddOrderFields` hands out raw
  `uint32_t` and the transposition protection stops at the codec boundary.
- **Timestamp deferred** — see [../todo.md](../todo.md). Can't retrofit data never read, and
  tick-to-trade measurement needs it.
- **Verify inlining** — see [../todo.md](../todo.md).
- **⚠️ ITCH offsets are from memory, not the spec.** Arithmetic checks out per message, but verify
  against the official Nasdaq TotalView-ITCH 5.0 document before trusting them. A transposed
  offset is silent and corrupts the book.
