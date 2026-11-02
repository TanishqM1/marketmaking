# Data Structures

### Probing: linear vs quadratic vs double hashing
**Why probing exists:** open addressing stores entries *in the array itself*, with no linked lists.
Two keys can hash to the same slot — a **collision**. Probing is the rule for where to look next,
and it must be deterministic so lookup retraces the same path insert took.

| | probe sequence | fixes | cache |
|---|---|---|---|
| **linear** | `h+1, h+2, h+3` | — | **same line → free** |
| **quadratic** | `h+1, h+4, h+9` (`h + i²`) | primary clustering | new line per probe |
| **double hashing** | `h + i·h2(key)` | primary **and** secondary | new line per probe |

**Two kinds of clustering**, which is what the fancier schemes exist to fix:
- **primary** — runs of occupied slots merge and grow. Any key hashing *anywhere* into a run must
  traverse the whole run. Linear probing suffers from this.
- **secondary** — two keys with the same initial slot follow the identical probe sequence forever.
  Quadratic still suffers from this; double hashing doesn't, because each key gets its own stride.

**Costs:** quadratic needs no extra hash but jumps 1, 3, 5, 7 slots apart. Double hashing needs a
**second hash computation** per lookup, and `h2` must be nonzero and coprime with the table size
(for a power-of-two table, `h2` must be odd).

### Why linear wins anyway
> Theoretical clustering behaviour loses to cache locality on real hardware.

Linear probing's "weakness" — the cluster — is exactly what makes it fast: the cluster is in the
**cache line you already paid for**. One ~200-cycle DRAM miss, then probing is free.

Quadratic and double hashing scatter, so every probe is a fresh miss. Better textbook properties,
worse real performance.

**Example:** 16-byte slots in a 128-byte cache line = 8 slots per line.
```
line A                                        │ line B
┌────┬────┬────┬────┬────┬────┬────┬────┐    ┌────┬───
│ 0  │ 1  │ 2  │ 3  │ 4  │ 5  │ 6  │ 7  │    │ 8  │
└────┴────┴────┴────┴────┴────┴────┴────┘    └────┴───
linear:     h=2 → 3, 4, 5      all in line A, free
quadratic:  h=2 → 3, 6, 11     11 is in line B, a new miss
```

**Worst case for linear:** hash lands on the last slot in a line, so `idx+1` crosses into the next
line. 1 slot in 8, and only if there's a collision — at 50% load factor (≈1.5 probes average)
that's ~6% of lookups paying a second miss.

**Mitigations, in order:** keep load factor ≤ 0.5 (chains stay 1-2 probes) → bucketize (group 8
slots per cache line and probe *within* the bucket, guaranteeing one line per lookup — what
hopscotch and most modern flat tables do).

⚠ **`sizeof(slot)` must divide the cache line**, and `alignas(128)` the table. Otherwise a *single*
slot straddles a boundary and reading **one** slot costs two misses — worse than any probing
concern. 16 into 128 = exactly 8, never straddles.

⚠ **Quadratic with a power-of-two table doesn't reach every slot.** `i²` mod 2ⁿ only covers a
fraction, so insertion can fail on a table that isn't full. Fix: a prime table size, or triangular
numbers `h + i(i+1)/2`, which do cover all slots for power-of-two sizes.

### For the order-ref map
Flat array + open addressing + linear probing is what quant firms use — either hand-rolled or via
abseil `flat_hash_map`, boost `unordered_flat_map`, or folly `F14`. **Never `std::unordered_map`**
(bucket array of linked lists: a pointer chase per lookup, a heap allocation per insert).

Order refs are assigned roughly sequentially by the exchange, so their low bits are well
distributed — `ref & (capacity-1)` is a good enough hash with **no hash function at all.** One AND
instruction.

**What matters more than the probe strategy:**
- **Never rehash mid-session.** Preallocate for the day's peak live-order count. A rehash means
  reinserting millions of entries — a multi-millisecond stall mid-trading-day.
- **Hugepages for the table.** 160MB spans ~40,000 4KB pages; the TLB holds 1-2k entries, so you
  miss on *address translation* before touching data. 2MB pages → 80 pages. Often a bigger win
  than anything about probing.
- **Prefetch early** — read `order_ref` first, issue the lookup, then decode the other fields while
  the miss is in flight.
- **Shrink the entry** — 16 bytes gives 8 slots per line vs 5 at 24 bytes.

⚠ **Erase is the fiddly part.** You can't just zero a slot — other keys may have probed *past* it
to reach their own, and zeroing breaks their chain. Use tombstones (a "deleted" marker that probing
continues through, needing an occasional rehash) or backward-shift deletion.

> Worth exploring: refs are broadly sequential, so `ref - session_base` might be dense enough to
> index an array directly with **no hashing**. The catch is range — a venue may issue 100M+ refs a
> day, trading hashing for TLB pressure. Measure if you get there.

### Load factor — why ≤ 0.5
`used_ / CAPACITY` high → longer probes, for **both** operations:
```
insert  probe until an EMPTY or TOMBSTONE slot to write into
find    probe until the ref matches, or EMPTY
```

Degrades non-linearly, because occupied runs merge and a ref hashing *anywhere* into a run must
traverse all of it:
```
0.50 -> 1.5 probes     0.90 -> 5.5    <- 1.3x the fill, 2.5x the probes
0.70 -> 2.2            0.95 -> 10.5
```

**The real cliff is the cache line, not the probe count.** At 32-byte entries, 4 fit per 128-byte
line — probes 1-4 are free, probe 5 is a new miss:
```
1.5 probes  ~ one line     ~200 cycles
5.5 probes  ~ two lines    ~400
10.5        ~ three        ~600
```
So keep load ≤ 0.5 to stay inside one line. Not because 1.5 probes is "fast enough."

⚠ **If `used_` ever reaches CAPACITY there is no EMPTY slot to stop at, and `find()` on an absent
ref loops forever** — an infinite loop in the hot path. Assert `used_ < CAPACITY` on insert, and
bound the probe count in release with a counter on the give-up path.
