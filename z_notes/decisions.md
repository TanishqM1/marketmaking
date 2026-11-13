# Decisions

Every design call in one place: what we picked, why, and where it lives. Read this first when
coming back to the project. ⚠️ = reasoned or estimated, not measured.

---

## Standing rules

- **No `new` / `delete` / `std::make_unique` on the hot path.** Pre-allocate everything at startup.
  The tick-to-trade thread never allocates after start; everything else may — see
  [hft_systems.md](hft_systems.md), dynamic memory.
- **No `throw` / `try-catch` on the hot path.** Exceptions bloat code and force stack unwinding.
  Use error codes, status enums, or `std::expected`. Startup is the exception — a failed bind
  *should* throw.
- **No `std::list` / `std::map` / `std::unordered_map`.** Scattered heap nodes = a pointer chase per
  access. Flat arrays, contiguous.
- **No rarely-taken code inlined into the hot path** (error handling, logging, gap recovery). It
  occupies L1i forever. Separate function, `[[gnu::cold]]` + `[[gnu::noinline]]`, `[[unlikely]]`
  on the branch. Write error paths as separate functions from the start.
- **Never format a log string on the hot path.** `snprintf` is microseconds. Bump a counter or push
  a fixed-size binary record to a ring; format elsewhere.
- **No float prices, ever.** Scaled integers or ticks. Floats break equality, and equality is the
  whole job of a book.
- **No bare typedefs for domain types.** `using Price = int64_t` is the same type as `Qty`, so
  transposing them compiles. A one-field struct is distinct with identical codegen.
- **Never claim an unmeasured speedup.** Check the asm, then benchmark in Release.

---

## Assumptions

Things the code relies on but doesn't check. If one stops being true, the listed thing breaks.

| assumption | relied on by | breaks how |
|---|---|---|
| After startup, the feed only sends the 7 types we decode: A, F, E, C, X, D, U | `decode_packet` (`codec/itch.cpp`) — an unknown type never advances `offset` | any other type (S, R, P, Q…) → the hot loop spins forever on that byte |
| R messages arrive as **one burst at the start**, never later | `run_startup` ends at the first non-R; the hot switch has no R case | an intraday R (e.g. a new listing ⚠️) → the hot loop spins on it |
| `trading_system` is running before the feed starts | `run_startup` | UDP drops anything sent before the socket binds → the R burst is lost, zero books, startup waits forever |
| Add orders always have shares > 0 | the bitmap in `state/book.hpp` `add` | a qty-0 add sets a level's bit with nothing behind it → can become best with qty 0 |
| The side byte is always `'B'` or `'S'` | `decode_add_order` — side starts as Buy, `'S'` → Sell | any other byte silently becomes a Buy |
| ITCH order refs only increase (no strides of 65,536) | identity hash `ref & MASK` (`state/order_map.hpp`) | strided refs share low bits → one huge probe chain. Don't let the mock exchange issue refs like `symbol_id × 65536 + n` |

---

## Decision log

| area | decision | why | where |
|---|---|---|---|
| prices | ITCH integer prices (4 implied decimals: $175.23 = 1,752,300) at the boundaries; the book works in **ticks** = price / 100 | integers only; a tick is a grid index | `book.hpp` `TICK_SIZE` |
| ladder | fixed 4,096-tick window centred on yesterday's close (clamped at 0). Out-of-window orders stay in the order map but never touch the ladder | `idx = tick - low_` as unsigned → **one** compare `idx >= ARRAY_SIZE` catches below *and* above | `book.hpp` |
| ladder | `[ARRAY_SIZE]` is a guard slot (always 0); `NO_LEVEL = 4096` | an empty side's cursor reads 0 qty with no branch | `book.hpp` |
| ladder | `alignas(128)` on `bids_` / `asks_` | the 4,097-entry `bids_` left `asks_` starting mid-line | `book.hpp` |
| book API | handlers take plain values (ref, side, price, qty), not ITCH structs | the book stays independent of the feed format | `book.hpp` |
| book API | handler bodies live in the header | the compiler only inlines bodies it can see — [cpp.md](cpp.md), inlining | `book.hpp`, `order_map.hpp` |
| best bid/ask | **1-level bitmap** + `clz`/`ctz`, no summary word | write-up below | `book.hpp` |
| order map | per-book flat open-addressing table, linear probing | write-up below | `order_map.hpp` |
| order map | identity hash `ref & MASK`; **skipped** Fibonacci hashing | refs increase, so low bits already spread; a multiply is ~3 cycles on *every* lookup for a pattern the feed doesn't produce | `order_map.hpp` |
| order map | **backward-shift deletion**, not tombstones | tombstones filled the table; measured 10.8 s → 0.21 s — [data_structures.md](data_structures.md) | `order_map.hpp` `shift_delete` |
| order map | entries stored **inline**, not an index of `{ref, ptr}` into a pool | 1 memory access instead of 2 dependent ones — write-up below | `order_map.hpp` |
| order map | `OrderEntry` = **16 B**: `ref` 8 + `remaining` 4 + bit-fields `abs_tick:26`, `side:1`, `occupied:1` | 8 entries per line (was 4), table 2 MB → 1 MB. Max tick 42,949,672 < 2²⁶ (`static_assert`). Separate `occupied` bit because ITCH ref **can be 0**. Bit-fields compile to the same asm as hand-written masks (checked) | `order_map.hpp` |
| order map | CAPACITY 65,536 for every book, no load-factor cap — **deferred** | needs real per-symbol peaks first | issue **#25** |
| order map | hugepages — **deferred** | Linux-only; can't build or measure on macOS | issue **#24** |
| receive | busy-poll with `MSG_DONTWAIT` | removes sleep/wake jitter (~5–50 µs ⚠️). Still one syscall per poll; kernel bypass impossible on macOS | `udp_receiver.cpp` |
| decode | **trust exchange field values, validate framing.** Side: start as Buy, `'S'` → Sell; no rejection, no `std::optional` | firm practice — per-field checks cost latency for errors that don't happen. The real bug was `side` uninitialized (UB), which the default fixes | `itch.cpp` `decode_add_order` |
| decode | no `default:` on the `MsgType` switch | `-Wswitch` then warns if a new enumerator has no `case` | `itch.cpp` |
| decode | **keep** the 7 per-case length checks; don't hoist one `length_for(type)` check + one advance | asm: hoisting adds a 2nd indirect jump on the type byte → ⚠️ likely slower. Cleaner isn't worth slower | `itch.cpp` |
| memory | no dynamic memory on the tick-to-trade thread after startup | judged on p99.9 — [hft_systems.md](hft_systems.md) | everywhere |
| startup | **separate startup loop** for the R burst; R kept **out of `MsgType`** | the hot switch compiles identical (127 instructions, asm checked). An R case in the hot switch reshuffles its compare tree (A: 2 → 3 compares) | `app/startup.cpp`, `itch.cpp` `next_r_message` |
| startup | config = `TICKER,dollars` CSV, path from `argv[1]`; dollars → ITCH integer at load | locates change daily, so the config can't hold them — R supplies today's. Integer parse, never a float | `startup.cpp` `load_config` |
| startup | config format is **strict**: no spaces, LF line endings, no trimming; any whitespace → throw with the line number | the file is generated, not hand-typed. A stray space in a ticker would silently never match the wire's R; strict makes it loud | `load_config` |
| startup | before the first R, non-R datagrams are dropped; after it, the first non-R ends startup and the rest of that datagram is `leftover` | before any R there are no books, so nothing droppable matters; the leftover keeps the first orders | `handle_startup_packet` |
| startup | config ticker with no R → warn and continue; duplicate R ticker or locate → **throw, refuse to start**; `main` catches, prints, exits 1 | startup checks run once, so they're free. A duplicate means a broken feed (e.g. two days replayed together); trading on miswired books is worse than not starting | `bind_r_message`, `report_missing_tickers` |
| startup | `locate_to_book` = flat `uint16_t[65536]`; books = `std::vector<Book>` + `reserve` | direct index: no hash, no probe, no bounds check; 128 KB vs 512 KB for pointers. Book count is runtime-only; `reserve` → never reallocates | `startup.hpp` `Market` |
| planned | `decode_packet<Handler>`; the handler gets only `books` (as a span) + `locate_to_book`, never `config` | `codec/` never includes `state/`; the hot path can't reach the map or grow the vector | `app/` |

---

## Write-ups

### Order map — where it lives, and how it's laid out
**Background.** Every E/X/D/U message names an order only by its `ref`. The book must turn that ref
into `remaining`, `tick`, `side` — **on every message**. So the order map is the hottest lookup in
the system, and its cost is mostly **how many cache lines it touches**.

| layout | how | wins | loses |
|---|---|---|---|
| **per-book open-addressing (ours)** | each `Book` owns a 65,536-slot table; `ref & MASK` + linear probe, inline entries | small, private to the core that owns the symbol; L2-hot | each book sized for *its own* peak → total = sum of peaks; hot names (AAPL) can overflow, cold names waste 1 MB (#25) |
| one shared hash map per feed thread | same table, sized for every book the thread owns, `book_id` in the entry | sized for the **total** peak, which is far below the sum of peaks (names don't all peak together). ⚠️ the common firm design | `book_id` needs 9 bits for 500 books; only 4 spare in our 16 B entry |
| one direct array `orders[ref]` | refs are day-unique and increasing → one load, no hashing (Charles Cooper's open-source ITCH book does this) | no probing at all | sized for the **whole day's** refs (100M+ → GBs, TLB pressure); refs are increasing but not sequential → holes; `book_id` comes back |
| index `{ref, ptr}` + pool | table holds pointers into a separate entry pool | pointer stability | 2 dependent accesses — below |

**Decision:** per-book for now. A single symbol's refs are sparse (one counter across all symbols),
so a direct array per book would be mostly holes, and a small hash map stays hot.
> ⚠️ firm internals aren't public. Open question in `z_questions/questions.md`: measure per-book
> vs one shared direct array on p99 for a real feed day.

### Order map — store entries inline, or an index of `{ref, ptr}` into a pool?
**Option 1 — inline (ours).** The table *is* the storage: each 16 B slot holds the whole order,
8 slots per 128 B line.
```
slots_[]  65,536 × 16 B = 1 MB
┌──────────┬──────────┬───────┬───────┐┌──────────┬──
│ ref 1001 │ ref 5537 │ EMPTY │ EMPTY ││ ref 1004 │ ..
│ rem,tick │ rem,tick │       │       ││ rem,tick │
└──────────┴──────────┴───────┴───────┘└──────────┴──
  find(1001): load 1 line -> compare ref -> data is right there      = 1 memory access
```

**Option 2 — index + pool.** Table holds only `{ref, ptr}`; orders live in a pre-allocated pool.
```
index[] {ref, ptr}                     pool[] OrderEntry
┌────────┬────────┬───────┐            ┌──────────────┐
│1001│ ●─┼──────────────────────────▶  │ ref 1001     │
│5537│ ●─┼──▶ (elsewhere)             │ rem,tick,side│
└────────┴────────┴───────┘            └──────────────┘
  find(1001): load index line -> follow ptr -> load pool line        = 2 DEPENDENT accesses
```
"Dependent" = the 2nd address comes from the 1st load, so the CPU can't start it early (no
memory-level parallelism) — you pay ~100 + 100 ns back to back.

**Why option 2 doesn't win here:**
- Probing costs **cache lines, not slots**. Extra slots inside an already-loaded line are ~free
  (L1 hit ~1 ns vs DRAM ~100 ns).
- Our load factor is low → chains are ~1 slot. Nothing to save.
- A contiguous pool doesn't help: contiguity helps *iteration*, but lookups arrive in random ref
  order, so lookup N+1 lands nowhere near lookup N. The only way to get the data into the index's
  line is to put the data *in* the slot — option 1.

| data lives in | inline (1 access) | index + pool (2 accesses) |
|---|---|---|
| L1 | ~1 ns | ~2 ns |
| L2 | ~5 ns | ~10 ns |
| **DRAM** (likely: 500 books ≈ 500 MB) | **~100 ns** | **~200 ns** |
> ⚠️ estimates. Settle with a microbenchmark: both layouts, books out of cache, p50/p99.

**When option 2 IS right:** pointer stability (erase shifts only the index slot — we never hold a
pointer across an erase), large entries (shifting pointers beats shifting 200 B entries — ours are
16 B), dense iteration over all live orders (the book never iterates).

**Industry:** the big C++ maps ship both — Abseil `flat_hash_map` (inline) vs `node_hash_map`
("use only for pointer stability"); Facebook `F14ValueMap` (inline) vs `F14VectorMap` (≈ option 2,
for big values / fast iteration). For a hot lookup with small values, inline is the default.

**Decision:** keep inline. The bigger lever is **cache footprint** — size `CAPACITY` per book (#25).

**Gotcha:** ours is *linear probing + backward-shift deletion*, **not** Robin Hood. Robin Hood is an
*insert* rule (a far-from-home entry steals a near-home one's slot to even out probe lengths); it's
often paired with backward shift, hence the mix-up.

### Best bid/ask — 1-level bitmap, or 2-level with a summary word?
**Background.** When the best level empties, the book must find the next non-empty one. The old
scan read `bids_[]` one `uint32` at a time: up to 4,096 loads / 16 KB / 128 lines on a thin book
(best at 3000, next at 10). A p99.9 spike on one message.

**Both options:** a bitmap per side, 1 bit per level (`1` = has qty). 4,096 / 64 = **64 words**
(`WORDS`), 512 B. One `word == 0` compare checks 64 levels; `clz` (count leading zeros → highest 1)
/ `ctz` (count trailing zeros → lowest 1) finds the level inside a word in one instruction. The
search starts in the old best's word — nothing better than best is ever set, so no masking needed.

```
level i  ->  word i / 64, bit i % 64
bids at 10 and 3000; 3000 empties:
  word 46 == 0 -> word 45 ... -> word 0 != 0          (47 compares, 1-level)
  63 - clz(word 0) = 10  -> best_bid_ = 10
```

| | 1-level (ours) | 2-level (+ summary) |
|---|---|---|
| extra state | none | 1 `uint64`: bit w = "word w non-empty" |
| find the word | **loop**, 64 levels per step | one `clz` on the summary |
| common case (next level ≤ 64 ticks) | 1 load + `clz` | 2 **dependent** loads + 2 `clz` |
| worst case | 64 compares, 4 lines | 2 loads, always |
| per add / per emptying | 1 OR / 1 AND | 2 ORs / AND + cmp + shift + AND |

Neither is "always faster": 2-level is constant-time (predictable tail); 1-level is cheaper in the
common case and on every message's maintenance. The summary word sits next to the cursors (already
hot), so its cost is the dependent chain and extra instructions, not cache.

**Decision:** **1-level.** Liquid top-500 names almost always have the next level within 64 ticks →
the loop stops on the first word. The big win was 16 KB → 512 B; 64 → 2 compares is small.
> ⚠️ unmeasured. Replay real ITCH and histogram words visited per scan. If the tail is fat
> (pre-market, the open, halts, cheap stocks where 64¢ is a big move), bring the summary back.

**Gotcha:** `clz(0)` / `ctz(0)` = 64 → `63 - 64` wraps to a garbage index. Only call them on a word
already known non-zero.

**Optional:** a guard word `ask_bits_[WORDS]` with bit 0 set lets the ask loop drop its
`word < WORDS` check (lands on 4096 = `NO_LEVEL`). Bids can't — there's no index −1.
