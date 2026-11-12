# Data Structures

How the order map (`state/order_map.hpp`) works: an open-addressing hash table. *Which* layout we
picked and why is in [decisions.md](decisions.md).

---

### Why a flat array, never `std::unordered_map`
`std::unordered_map` is a bucket array of linked lists: a pointer chase per lookup and a heap
allocation per insert. Firms use a **flat array + open addressing** — hand-rolled, or abseil
`flat_hash_map`, boost `unordered_flat_map`, folly `F14`. An open-addressing hash map *is* a flat
array: the array is the storage, the hash just computes the index. Each slot stores the key (`ref`)
too — otherwise you can't tell "mine" from "someone else's who collided here."

**Never rehash mid-session.** Preallocate for the day's peak live orders; a rehash reinserts
everything — a multi-millisecond stall mid-trading-day.

### The hash: `ref & MASK`
With `CAPACITY` a power of two, `ref & (CAPACITY - 1)` keeps the low 16 bits — it *is* modulo, in
one AND instead of a ~20–40-cycle divide. That's why `CAPACITY` must be 2ⁿ.

It's an **identity hash** (the number itself, unscrambled), and it works because ITCH refs come from
one increasing counter: nearby refs land in neighbouring slots with almost no collisions.
```
works:   ref 1001 -> slot 1001, ref 1002 -> slot 1002 ...
breaks:  ref 0, 65536, 131072 ...  -> all slot 0 -> one huge probe chain
```
The fix for strided keys is a **Fibonacci hash** `(ref * 0x9E3779B97F4A7C15) >> 48` — the multiply
mixes every bit into the top 16. Skipped: ~3 cycles on every lookup for a pattern the feed doesn't
produce (see the assumption in [decisions.md](decisions.md)).

### Probing: linear vs quadratic vs double hashing
Two keys hashing to one slot is a **collision**; probing is the deterministic rule for where to look
next, so lookup retraces the path insert took.

| | probe sequence | fixes | cache |
|---|---|---|---|
| **linear** (ours) | `h+1, h+2, h+3` | — | **same line → free** |
| quadratic | `h+1, h+4, h+9` | primary clustering | new line per probe |
| double hashing | `h + i·h2(key)` | primary **and** secondary | new line per probe, + a 2nd hash |

- **Primary clustering:** occupied runs merge, and a key hashing *anywhere* into a run walks it.
- **Secondary clustering:** keys with the same start follow the same sequence forever.

**Linear wins anyway** — theory loses to cache locality. Its cluster sits in the **line you already
paid for**: one ~200-cycle miss, then probing is free. The others scatter, so every probe is a new
miss.
```
16 B slots, 128 B line = 8 per line
linear:     h=2 -> 3, 4, 5      all in line A, free
quadratic:  h=2 -> 3, 6, 11     11 is in line B, a new miss
```
⚠ Quadratic on a power-of-two table doesn't reach every slot (`i²` mod 2ⁿ) — insert can fail on a
table that isn't full.

### Load factor — why ≤ 0.5
A **probe** = one slot examined (1 probe = the home slot held our ref). Fuller table → longer
probes for both `insert` (walk to an empty slot) and `find` (walk to the ref or an empty slot), and
it degrades non-linearly as runs merge:
```
0.50 -> 1.5 probes     0.90 -> 5.5    <- 1.3x the fill, 2.5x the probes
0.70 -> 2.2            0.95 -> 10.5
```
**The real cliff is the cache line, not the probe count.** 8 entries per line → probes 1–8 are free
if they stay in the line; the next line is another miss (~200 cycles each). Keep load ≤ 0.5 to stay
in one line — not because 1.5 probes is "fast enough."

**Where your home slot lands in the line:** `ref & MASK` picks any slot uniformly, so a home slot
near the end of a line crosses into the next one after a probe or two — roughly **6% of lookups at
0.5 load** with 8 per line (12% with the old 4 per line).

**Bucketizing** is the guaranteed fix: round the home index down to the line start and probe within
that group (one line, always), with an overflow path for a full bucket.

⚠ **A full table:** `insert`/`find` are bounded loops (at most `CAPACITY` iterations), so they don't
hang — but a miss on a full table walks all 65,536 slots (1 MB). Today `CAPACITY` is fixed with no
load cap → issue **#25**.

### What industry libraries do: groups + SIMD metadata
| | |
|---|---|
| abseil `flat_hash_map` ("Swiss tables") | groups of 16 slots |
| folly `F14` | 14-slot chunks |
| boost `unordered_flat_map` | group-based |

A **separate array of 1-byte control values** (7 bits of hash + empty/deleted), one per slot.
Probing loads 16 control bytes into a SIMD register and compares all 16 in **one instruction**; only
matches get a full key compare.
> For an HFT order map, plain linear probing with a well-sized table is also common —
> **predictability beats average speed when you care about p99.9.** Build the simple one first;
> it's what you'd validate a SIMD version against.

### Deletion: tombstones vs backward-shift
You can't just empty a slot: other keys may have probed *past* it, and an empty slot ends their
search early, making them unfindable.

| | how | cost |
|---|---|---|
| **tombstone** (lazy) | mark "deleted" — probing continues through it, insert may reuse it | never cleaned. Needs a mid-session rehash, which breaks "never rehash" |
| **backward-shift** (eager, ours) | empty the slot, then slide later entries in the cluster back into the gap — never before their own home slot | pay a little on every erase (usually 0–1 entries, same line) |

**Measured** (old tombstones vs backward-shift, 4 sweeps + 20k misses): **10.8 s → 0.21 s**. After
one sweep of the table every slot was a tombstone, so every miss scanned all 65,536 slots. Firms
take a small steady cost over a tail that grows all day. Code:
[state/order_map.hpp](../state/order_map.hpp) `shift_delete`.

The move rule: the entry at `j` (home slot `home`) may fill the hole only if
`((j - home) & MASK) >= ((j - hole) & MASK)` — i.e. the hole is between its home and where it sits.

With no tombstones left, a slot is just occupied or not — a 1-bit `occupied` field. The array is
allocated **once** and never grows, shrinks, or frees.

**Gotcha:** erase *moves* other entries → never hold an `OrderEntry*` across an erase. (`reduce`
reads `side` / `abs_tick` before erasing for this reason.)

### Prefetching
Read `order_ref` first, issue the lookup (or a prefetch), then decode the other fields while the
miss is in flight — hides part of the one unavoidable miss.

See also: [decisions.md](decisions.md) — inline vs index+pool, per-book vs shared maps, the
best-bid/ask bitmap; [hardware.md](hardware.md) — cache lines, TLB.
