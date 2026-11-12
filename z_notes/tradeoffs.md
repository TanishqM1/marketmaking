# Tradeoffs

One entry per design decision: the problem, the options, what we picked and why.
Entries are separated by `---`.

---

### Q: Order map — store entries inline, or an index of `{ref, ptr}` into a separate pool?

**Background.** Every E/X/D/U message names an order only by its `ref`. The book has to turn that
ref into the order's `remaining`, `tick` and `side` — **on every message**. So the order map is
the hottest lookup in the system, and its cost is mostly **how many cache lines it touches**.

**Option 1 — inline (ours, `state/order_map.hpp`).** The table *is* the storage: each 32 B slot
holds the whole order, 4 slots per 128 B line.
```
slots_[]  65,536 × 32 B = 2 MB
┌──────────┬──────────┬───────┬───────┐┌──────────┬──
│ ref 1001 │ ref 5537 │ EMPTY │ EMPTY ││ ref 1004 │ ..
│ rem,tick │ rem,tick │       │       ││ rem,tick │
└──────────┴──────────┴───────┴───────┘└──────────┴──
  find(1001): load 1 line -> compare ref -> data is right there      = 1 memory access
```

**Option 2 — index + pool.** Table holds only `{ref, ptr}` (16 B, 8 per line); orders live in a
pre-allocated pool with a free list.
```
index[] {ref, ptr}                     pool[] OrderEntry
┌────────┬────────┬───────┐            ┌──────────────┐
│1001│ ●─┼──────────────────────────▶  │ ref 1001     │
│5537│ ●─┼──▶ (elsewhere)             │ rem,tick,side│
└────────┴────────┴───────┘            └──────────────┘
  find(1001): load index line -> follow ptr -> load pool line        = 2 DEPENDENT accesses
```
"Dependent" = the 2nd address comes from the 1st load, so the CPU can't start it early.

**Why option 2 doesn't win here:**
- Probing costs **cache lines, not slots**. Extra slots inside an already-loaded line are ~free.
- Our load factor is <5% (hundreds–thousands live / 65,536) → chains are ~1 slot. Nothing to save.
- So option 2 trades ~zero probe savings for a **second cache miss** (the pointer chase).

| data lives in | inline (1 access) | index + pool (2 accesses) |
|---|---|---|
| L1 | ~1 ns | ~2 ns |
| L2 | ~5 ns | ~10 ns |
| **DRAM** (likely: 500 books ≈ 1 GB) | **~100 ns** | **~200 ns** |
> ⚠️ estimates, not measured. Settle with a microbenchmark: both layouts, books out of cache, p50/p99.

**When option 2 IS right:**
| benefit | matters for us? |
|---|---|
| pointer stability — erase shifts only the 16 B index slot, entries never move | barely; book never holds a ptr across erase |
| large entries (e.g. 200 B) — shifting pointers is cheaper than shifting entries | no, ours are 32 B |
| dense iteration over all live orders | no, the book never iterates |

**Industry:** the big C++ hash maps ship both and document this exact split —
Abseil `flat_hash_map` (inline) vs `node_hash_map` (pointer per entry; "use only for pointer
stability"); Facebook F14 `F14ValueMap` (inline) vs `F14VectorMap` (≈ option 2: index into a dense
value array; for big values / fast iteration). For a hot lookup with small values, inline is the
default — expected at an HFT firm too.
> ⚠️ verify: specific firm internals aren't public.

**Decision:** keep inline. The bigger latency lever is **cache footprint** — shrink per-book
`CAPACITY` (e.g. 4,096 slots = 128 KB/book, ~64 MB for 500) so more books stay hot. That beats any
layout change.

**Gotcha:** what we built is *linear probing + backward-shift deletion*, **not** Robin Hood.
Robin Hood is an *insert* rule (a far-from-home entry steals the slot of a near-home one to even out
probe lengths); it's often paired with backward shift, hence the mix-up. At our load it changes nothing.

See also: [data_structures.md](data_structures.md) — backward-shift deletion, direct-array layout;
[hft_systems.md](hft_systems.md) — where the order map lives.
