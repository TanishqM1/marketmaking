# Cache

Ordered mechanism → application: line sizes and hierarchy first, then layout rules, then what
firms do about it.

**Framing:** every entry should land on *what a tier-1 HFT firm actually does and why* — not just
the textbook mechanism. For each concept: what they engineer, what they measure, what they accept,
and what they'd never ship. The mechanism is only there to explain the choice.

### L1 is split; L2/L3 are unified
The CPU fetches code and data in the same cycle, so one shared L1 would contend with itself.
- **L1i** — the code being executed
- **L1d** — the data it operates on

This machine (M4, 10-core):
| | perf cores | efficiency cores |
|---|---|---|
| L1i | **192 KB** | 128 KB |
| L1d | 128 KB | 64 KB |
| L2 | 16 MB | 4 MB |

**Cache line is 128 bytes**, not the usual 64 — use 128 for `alignas` and false-sharing padding.

⚠ 192 KB of L1i is huge; typical x86 server cores have **~32 KB**. Code that fits here could
thrash on a Xeon. "Measure in Release" eventually becomes "measure on the target."

### Line, way, set
| | |
|---|---|
| **line** | 128 bytes — the unit of transfer |
| **way** | one line-slot inside a set. "8-way" = 8 ways per set. |
| **set** | a group of ways. An address maps to **exactly one** set — no choice. |

```
L1d = 128 KB, 128 B/line, 8-way
  131,072 / 128      = 1,024 lines
  1,024 / 8 ways     = 128 sets
  128 B x 8 x 128    = 131,072 B   ok
```

The address is chopped into three fields:
```
[63 ........ 14] [13 ... 7] [6 ... 0]
      tag        set index    offset
                 7 bits =    7 bits =
                 128 sets    128 B
```
A lookup computes the set index, then compares the 8 tags in that set.

⚠ **"Is L1 full?" is never the question — it's "is THIS address's set full?"** 9 addresses mapping
to the same set will thrash while 1,015 lines sit unused. That's a *set conflict*. Not a worry for
a large contiguous array (it spreads evenly across all sets), but it's how two hot structures at
unlucky addresses evict each other with a tiny working set.

> The 8-way figure is illustrative — Apple doesn't publish M4 associativity. 128 KB and the 128-byte
> line are confirmed by `sysctl`.

### Caches are a lookaside over RAM, not separate storage
**RAM is the real storage; L1/L2/L3 hold copies of recently-used 128-byte chunks of it.** Every
cache line is tagged with *which RAM address it's a copy of* — that's what the `tag` field is.
A lookup asks "do I have a copy of address X?" Caches have no addresses of their own.

**Copies = actual bytes, not pointers.** Each line also stores a **tag** identifying which RAM
address those bytes are a copy of — that's how the hardware answers "do I have address X?" The
`tag` is the only pointer-like part. If a line held a pointer you'd still have to follow it to RAM
— the ~250 cycles you were avoiding. The duplication *is* the speed, and it's also why `dirty` and
coherence exist: three copies of one address can disagree.

```
your code:  load from address 0x7fff5313
hardware:   in L1?    yes -> ~4 cycles
            in L2?    yes -> ~15, and install into L1
            in L3?         -> ~50
            DRAM           -> ~250, install on the way back
```
Same instruction, same result, four different latencies. Cache optimisation is *entirely* about
which branch you take.

Three consequences:
- **You never address a cache.** There is no "read from L2" instruction. You can't tell a hit from
  a miss in code — only from timing or `perf` counters.
- **Invisible to correctness.** A hit and a miss return the same value. Cache behaviour is purely a
  performance concern, never a correctness one.
- **One address, several copies** — the same line can be in L1, L2 and L3 at once. Keeping them
  consistent is the **cache coherence protocol**, the machinery behind the dirty bit and false
  sharing.

### The cache hierarchy is not a conveyor belt
On a DRAM miss the line is installed in **multiple levels at once** — not L1, then L2, then L3 in
sequence. Which levels depends on the chip's **inclusion policy**:
| | |
|---|---|
| inclusive | L3 holds a copy of everything in L2/L1. Intel, historically. |
| exclusive | a line lives in exactly one level. AMD, for some levels. |
| non-inclusive | neither guaranteed. Common now. |

**Every level evicts, including L3.** There is no "it stays in L3 forever" — L3 is finite and
spills back to DRAM like the others. "Never leaves cache" is true only if the **working set fits**.

**Typical sizes** (you don't choose these — the chip does; you choose your working set):
| | |
|---|---|
| L1d | 32–128 KB per core (this machine: 128 KB) |
| L2 | 256 KB – 2 MB per core on x86; **16 MB shared** on M4 |
| L3 | 8–256 MB |

**Example** — why "make the table huge so the load factor is tiny" backfires:
```
2 MB table,   16 MB L2   -> fits. nothing evicts. effectively always cached.
256 MB table, 32 MB L3   -> thrashes. most lookups miss to DRAM anyway.
```
You'd trade short probe chains for a guaranteed DRAM miss on every lookup. Size the table to
**actual peak live entries at ~50% load**, not as big as memory allows.

⚠ Firms do influence cache from the hardware side — buying CPUs for large L3, sometimes disabling
cores or hyperthreading so each active core gets a bigger share. That's a purchasing decision, not
a code one.

### Inclusive vs exclusive caches
("Outer" = further from the core, toward DRAM.)
| | guarantee |
|---|---|
| **inclusive** | outer levels **always** hold a copy. In L1 ⟹ also in L2 and L3. |
| **exclusive** | a line is in **exactly one** level. In L1 ⟹ **not** in L2 or L3. |
| **non-inclusive** | **no guarantee either way** — may or may not be there. NOT the same as exclusive, which guarantees it is *not*. |

Possible states differ, which is why "in L1 and L3 but not L2" isn't always legal:
```
inclusive:  L3 only | L2+L3 | L1+L2+L3        (L1 without L2 is IMPOSSIBLE)
exclusive:  exactly one level
non-incl:   any combination
```

**The search order is always L1 → L2 → L3 → DRAM, under every policy.** Policy changes *where
copies live*, never the walk.

**MESI** = the coherence protocol's per-line states: **M**odified (= dirty, we hold the only valid
copy), **E**xclusive (clean, only copy), **S**hared (clean, others may have it), **I**nvalid (not
present). "Dirty" *is* the M state — so the hardware never needs a lookup to know dirty-ness, the
bits are already there.

**Eviction of a dirty line:**
| | |
|---|---|
| **inclusive** | the outer level has a **stale** copy → the writeback **updates** it. That line then becomes dirty itself. |
| **exclusive** | the outer level doesn't have it → **allocate** a new line there, which may evict something *else*. |
| **non-inclusive** | may or may not be present → update or allocate; the controller looks up its own tags (which it does for every request anyway). |

⚠ **Eviction cascades.** Dirty L1 line evicted → writes to L2 → L2's set is full → evicts a dirty
L2 line → writes to L3 → ... → DRAM. One eviction can trigger a chain of writebacks, which is a
real and *variable* latency source. Another reason write-heavy structures hurt tail latency more
than read-mostly ones.

**The tradeoff:**
```
inclusive:  usable capacity = L3 only (L1/L2 contents are duplicated inside it)
            but coherence is CHEAP -- L3 is a complete directory, so "is this line
            anywhere on the chip?" is one lookup
exclusive:  usable capacity = L1 + L2 + L3 (no duplication) -- more cache for the same silicon
            but needs a separate snoop filter / directory for coherence
```

**What firms use — they don't choose; the chip does:**
| | L3 policy |
|---|---|
| Intel, through ~Skylake-client | inclusive |
| **Intel Skylake-SP (2017) and later Xeons** | **non-inclusive** |
| **AMD Zen / EPYC** | **victim cache** (exclusive-ish: L3 holds lines evicted from L2) |

So firms run **non-inclusive or victim L3**. CPUs get picked for core count, cache size, clock and
PCIe lanes; the policy comes along for the ride.

⚠ **None of this changes code you write.** It's invisible to correctness and shifts miss-path
latency by single-digit cycles. The levers you control are **working-set size** and **access
pattern**.

### Clean vs dirty cache lines
**One bit per line meaning "this hasn't been written outward yet."**
| | |
|---|---|
| **clean** | the cache copy **matches** the next level out. Read-only, or already written back. |
| **dirty** | we **wrote** to the cache copy and that write hasn't propagated. The cache holds the only current value. |

"Next level **out**" = away from the core, toward DRAM: L1 → L2 → L3 → DRAM.

**It is not written out on a schedule — only on eviction**, when that line's slot is needed for
something else:
```
write to slots_[idx]  -> line marked dirty
write again           -> still dirty, no cost
write again           -> still dirty, no cost
eviction              -> NOW it's written to L2, bit clears
```
**That deferral is the whole point: 100 writes, one writeback.** This is *write-back* caching.
Write-*through* would pay the slower level on every single write. Dirty isn't a choice — it's the
cache deferring work it hopes to avoid entirely.

**What eviction costs:**
```
clean  ->  FREE. just drop it; the next level already has the correct data.
dirty  ->  must be written out first, then the slot is reusable.
```

⚠ **Eviction from L1 is not a "move" to L2.** L2 already has the line (inclusive) or receives it
(exclusive). **Nothing is lost** — the data is still reachable at the next level out. Caches hold
*copies* of DRAM lines, never the only copy.

**The flow: a write ALWAYS makes the line dirty, lands only in L1, and propagates outward only on
eviction.** The new value *is* in the cache — dirty means the cache holds the authoritative value
and memory doesn't.
```
write        -> lands in L1 only, marked dirty
write again  -> L1 again, still dirty, FREE
write again  -> L1 again, FREE
eviction     -> NOW it propagates outward
```
The point is to *avoid* propagating — 100 writes, one writeback. (Write-*through* pays the slower
level on every write. Nothing modern does that for general memory.)

**On eviction it's ONE write outward either way** — only the slot handling differs:
```
exclusive:  L2 doesn't have it  -> ALLOCATE a new line there
            needs a free way -> may evict something ELSE from L2, which cascades if dirty
inclusive:  L2 has a stale copy -> OVERWRITE its data
            the way is already ours -> no new eviction from this step, but L2's line is
            now dirty and propagates when IT gets evicted
```

**Example** — in our code:
```cpp
find(ref)                   // READ  -> line stays clean -> eviction is free
entry->remaining -= qty;    // WRITE -> line goes dirty  -> eviction costs a writeback to L2
bid_qty[tick]    -= qty;    // WRITE -> same
```
Nearly free after the lookup: `find` already pulled the line into L1, so the write just lands
there. You pay the ~200-cycle miss once to *get* the line; everything you do to it afterwards is
L1-speed until eviction.

⚠ **Writes can miss too — write-allocate.** Writing to an address that isn't cached *loads the line
first*, then applies the write. So touching a cold price level costs a read before the update,
which is why `top_n` near the touch is cheap (lines already hot) and jumping to a distant price
isn't.
Not worth engineering around (~12 cycles, amortised), but it's why a read-mostly structure is
cheaper cache-wise than a write-heavy one.

⚠ **The dirty bit is also part of cache coherence.** If another core wants to read that line, the
protocol must notice our copy is dirty and fetch the current value from us rather than from stale
DRAM. That machinery is what makes **false sharing** expensive — two cores writing to the same line
keep invalidating each other's copies.

⚠ **DRAM always keeps the allocation, but not always the current value.** A dirty line means the
cache copy is newer. Separately the OS can **swap** pages to disk under memory pressure — prevent
it with `mlock`/`mlockall`, standard HFT setup, because a page fault on the hot path is
milliseconds.

### Cache lines: how they work and how to use them efficiently
A cache is an array of **lines**. One line holds 128 contiguous bytes from memory (on this
machine), aligned to a 128-byte boundary. **The CPU cannot fetch less than a line** — touch one
byte and the whole aligned block comes in.

Stored in SRAM on the CPU die, not in RAM:
```
L1d  128 KB  = 1,024 lines     ~4 cycles
L2    16 MB  = 131,072 lines   ~12
DRAM                           ~200-300   <- not cache; the thing cache exists to avoid
```
**Cache line size is hardware-dependent** — `sysctl hw.cachelinesize` → 128 here.
| | line |
|---|---|
| Apple Silicon (this machine) | **128** |
| Intel x86-64 / AMD x86-64 | **64** |
| most ARM servers | 64 |

Firms run x86-64 Linux, so **the real target is 64**. Most advice you read says 64 for that reason.

What that changes:
| | M4 (128B) | Xeon (64B) |
|---|---|---|
| `OrderEntry` @ 32B | 4 per line | **2** per line |
| price levels @ 4B | 32 per line | **16** per line |
| `alignas` value | 128 | **64** |
| probes before crossing a line | 4 | **2** |

So probe-chain math gets *tighter* on the real target — probe 3 crosses a line instead of probe 5,
which makes the ≤0.5 load factor matter more, not less.

**Make it a named constant** (`constexpr std::size_t CACHE_LINE = 128;`) so porting is one line
instead of hunting literals. Keep 128 while developing: over-aligning on a 64B machine just wastes
padding, but under-aligning here causes real straddling.

⚠ Some AMD EPYC generations prefetch in 128-byte pairs even though the line is 64, so
false-sharing padding is sometimes done at 128 on AMD anyway. Doesn't change layout decisions,
just means the 64/128 split is fuzzier than it looks once you're measuring.

**The goal: every byte you fetch should be a byte you use.** A line that's half padding or half
cold code wasted half the fetch.

**Two separate requirements, both needed:**
```cpp
struct OrderEntry { ... };                     // sizeof must DIVIDE 128 (8/16/32/64/128)
alignas(128) OrderEntry slots_[CAPACITY];      // array must START on a line boundary
```
Alignment fixes where the array starts; size stops individual entries from straddling. One without
the other doesn't work:
```
start aligned, sizeof 24:   128/24 isn't an integer -> some entries span 2 lines
sizeof 32, start unaligned: array begins mid-line   -> some entries span 2 lines
both:                       4 entries per line, never straddles          ok
```
Reading **one** straddling entry costs **two** misses — worse than any probing concern.

⚠ **`alignas` goes on the ARRAY, not the struct.**
```cpp
alignas(128) struct OrderEntry { ... };    // WRONG: sizeof becomes 128, 1 entry per line
alignas(128) OrderEntry slots_[CAPACITY];  // right: entry stays 32, array is aligned
```
On the struct it's 4x the memory and 4x the misses.

**Space wasters to avoid:**
| | |
|---|---|
| bad member order | a `uint64_t` after a `uint8_t` inserts 7 bytes of padding. **Largest first.** |
| real members that could be `static constexpr` | `std::size_t capacity_ = 65536;` costs 8 bytes **per object** for a value that never changes |
| fields you don't read | `match_number`, `tracking_num` |
| oversized types | `uint64_t` for something that fits in `uint16_t` |

⚠ **`static constexpr` is the GOOD practice, not a waster** — it occupies **zero bytes**. The value
is baked into the instruction stream, so `CAPACITY`/`MASK`/`EMPTY` cost nothing in memory or cache.

**The one case where you WANT padding: false sharing.** Two variables written by *different
threads* that share a line will invalidate each other's cache line on every write, even though
neither touches the other's data. There you deliberately pad each to its own line with
`alignas(128)`. Opposite problem, opposite fix.

See also: [cpp.md](cpp.md) — struct layout, `alignof`/`sizeof` rules.

### Hot path in the cache line
**The bytes you fetch should be bytes you execute.** A line that's 60% hot path and 40% error
handling wasted 40% of that fetch.

Cold code isn't "outside the cache" — it's at a **far-away address**, so it never lands in the same
line. If the error fires you pay one miss, for something that never happens.

| control | what it does |
|---|---|
| `[[unlikely]]` / `[[likely]]` | move a cold **branch body** out of line; make the hot path the fall-through. The *test* still runs every time. |
| `[[gnu::cold]]` | whole **function** → `.text.unlikely`, optimised for size, AND marks every branch reaching it unlikely — it propagates, so no need to annotate call sites |
| `[[gnu::hot]]` | → `.text.hot`, linker groups hot functions contiguously |
| `[[gnu::noinline]]` | keep a cold function out of the hot path's footprint |
| `-ffunction-sections` + linker order file | explicit symbol ordering from `perf` data (`-Wl,-order_file` on ld64, `--symbol-ordering-file` on lld) |
| PGO | automatic; also splits cold branches *within* a hot function |
| BOLT (`llvm-bolt`) | post-link relayout from `perf` data. Linux/ELF only. |

**Applies to data too.** The order-ref map entry is the big one — the fields a lookup touches
should fit in one line, not span two. Same principle, L1d.

It's about **density, not size**: a 50 KB hot path packed contiguously beats a scattered 10 KB one,
because the scattered version wastes most of every line and touches more cache sets.

**Also at larger code sizes: iTLB.** 300 KB of code spans ~75 pages at 4 KB, and the instruction
TLB is small — you start missing on *address translation*, not the code. Fix is hugepages for the
text segment (2 MB pages → 1-2 TLB entries for the whole binary). Standard HFT technique, often a
bigger win than I-cache tuning.

> Acting on any of this needs `perf record -e L1-icache-load-misses`, which doesn't exist on
> macOS. Linux-only — see issues #12, #16.

### Cache depends on address locality, not execution order
Two functions called back-to-back but 200 KB apart in the binary don't help each other. "Hot code
beside each other" means **adjacent addresses**.

**You can't *put* code in L1i** — it's demand-filled. The goal isn't "load my hot 50 KB", it's
"stop the cold 250 KB from evicting it."

Two mechanisms:
- **Line granularity.** 128 bytes per line, and the CPU can't fetch half a line. A hot function
  sandwiched between cold ones means every line is part-hot/part-cold.
- **Set conflicts.** L1i is set-associative — an address maps to one set, and only N lines fit per
  set. Two hot functions whose addresses collide evict each other **even if total hot footprint is
  tiny.** So "50 KB hot, 192 KB cache" guarantees nothing; distribution matters, not just size.

⚠ Source file order is only a weak hint. Within one TU functions often land in source order by
accident, but the linker can reorder TUs, and `-ffunction-sections` lets it reorder everything.

### Inlining is a cache budget
Inlining copies the body into every call site, growing the hot loop's **instruction** footprint.
If the loop stops fitting in L1i you pay misses on the exact path you were speeding up.

| | |
|---|---|
| **always inline** | leaf functions smaller than their own call sequence. `read_be16` is one load + one `rev` — not inlining it is strictly worse. |
| **verify, don't assume** | whole message decoders. Six inlined bodies in one `switch` makes `decode_packet` big. |

```sh
clang++ -O2 -Rpass=inline -Rpass-missed=inline -c codec/itch.cpp
```
Shows what was inlined and what was **declined**, with reasons. Release only — `-O0` inlines
nothing. `[[gnu::always_inline]]` forces, `[[gnu::noinline]]` prevents.

### Reserving cache: Intel CAT is L3-only
**CAT (Cache Allocation Technology)** partitions the **L3 only**, at **way** granularity. You define
classes of service, assign L3 ways to each, and assign *cores* to a class.
```
you CAN say:     "this core gets 4 of L3's 16 ways, nothing else may use them"
you CANNOT say:  "pin this specific array"
```
**Firms use it so their hot structures are guaranteed at least L3 (~50 cycles) rather than DRAM
(~250).** The threat isn't time passing — it's a **noisy neighbour** process streaming through
memory and evicting you. CAT walls them off.

**L1/L2 need no API** — they're per-core (or per-cluster). Isolate the core (`isolcpus` + IRQ
affinity) and nothing else is scheduled there, so you own its L1/L2 outright. "Never evict the ref
map from L2" is achieved indirectly: own the core, keep the working set under L2, nothing competes.

Closest thing to true pinning is Intel **cache pseudo-locking** — load a region into reserved L3
ways, then configure so nothing else can use them. Exposed via Linux `resctrl`. Real but niche.

⚠ **None of this exists on macOS / Apple Silicon.** Linux + Intel/AMD only. ARM's equivalent
(**MPAM**) exists but is far less mature. Same bucket as kernel bypass and core pinning.

### Working set budget — ours vs a real firm's
```
ours                                        a firm's raw problem
order map       ~2 MB                       8000 symbols, ~4M live orders x 32 B = 128 MB
ladders         ~128 KB                     (fits in NO cache)
recv buffer     ~1.4 KB
hot code        a few KB
                ───────
                ~2.2 MB  vs 16 MB L2
```
Two things close that gap, and both are design decisions:
- **Filter to subscribed symbols.** Trading 50 names means ignoring 7,950. ~20k live orders =
  **640 KB**. The `bool[65536]` subscribed filter is a cache-residency tool as much as a CPU one.
- **Shard by symbol across cores.** Each core owns a symbol group, so each map is a fraction of the
  total. **This is the real reason sharding matters** — not throughput, but making each shard's
  working set fit in cache.

A realistic per-core budget for a 50-symbol market maker lands at **a few MB** — same order of
magnitude as ours. The shape is similar; they arrive there by filtering and sharding.

### How this actually works at a quant firm
Moved — see [hft_systems.md](hft_systems.md): core layout, sharding, operational setup, what's
engineered vs measured.

### Spatial vs temporal locality
**Spatial locality tells you how to LAY OUT data; temporal locality tells you how to SEQUENCE
work.** Temporal = if you touch an address once you'll likely touch it again soon, so finish with
data while it's still resident instead of coming back to it later. Ignoring it means you can have a
perfect contiguous layout and still thrash, because the lines get evicted before you're done with
them. The classic fix is **loop tiling**: instead of sweeping a huge grid row by row (by the time
you wrap around, the start is evicted), process small blocks that fit in cache and exhaust each
block's reuse before moving on. Replacement policies like LRU exist precisely to exploit this —
recently-used lines are the ones kept.

**In our code, temporal locality explains which structure is expensive:**
| | temporal reuse | result |
|---|---|---|
| receive buffer | **excellent** — same ~11 lines, millions of times | permanently hot, never evicted |
| ladder near the touch | good — same few levels hit repeatedly | stays hot |
| **order map** | **poor** — random refs, each touched a few times then gone | **the bottleneck** |

So the map isn't slow just because access is *random* — it's slow because there's **no reuse to
exploit.** That's also why prefetching (hiding the miss) is the right tool there, rather than
trying to improve its locality.

### Memory inventory — what lives where, and why
**Built now:**
| | size | why |
|---|---|---|
| incoming receive buffer | 64 KB alloc, **~1.4 KB hot** | 64 KB because max UDP payload is 65,507 — sized so truncation is impossible. Only ever holds **one** datagram, so the hot part is ~11 cache lines. Permanently dirty and permanently resident. |
| decode locals (parsing) | **0** | Never in memory at all. SROA splits the `Fields` structs into registers; nothing is stored. |
| **ref → order map** | **2 MB** | 65,536 slots × 32 B. Sized at ~2× peak live orders so load stays ≤0.5. **The only random-access structure → the only real miss source.** |
| ladders (bid + ask) | **16 KB per symbol** | 2 sides × ~2,000 ticks × 4 B. Sequential scan near the touch, so it stays hot. |

⚠ The ladders are **much** smaller than the map per symbol — 16 KB vs 2 MB, not "a bit less." But
they scale differently:
```
order map  scales with LIVE ORDERS     -> 2 MB regardless of symbol count
ladders    scale with SYMBOLS x WINDOW -> 50 symbols x 16 KB = 800 KB
```
So the tick *window* matters once multiplied: 2,000 × 50 = 800 KB, but 16,384 × 50 = 6.4 MB.

&nbsp;

**Needed soon:**
| | size | why |
|---|---|---|
| symbol metadata table | small | `locate → tick size, base price`. **Can't write `price_to_tick()` without it** — symbols have different tick sizes and reference prices. |
| subscribed-symbol filter | 8–64 KB | `bool[65536]` or a bitset. A **cache-residency** tool as much as a CPU one: filtering before decode keeps unwanted symbols out of the map entirely. |
| the code itself | tens of KB | Competes for **L1i**, not L1d — and past L1 it competes with data in the unified L2. |

&nbsp;

**Later phases:**
| | size | why |
|---|---|---|
| our own open-orders map | small — tens of live quotes | **Separate from the market's order map.** Needed to cancel our own orders and to recognise our own liquidity in the feed (the exchange doesn't mark it). |
| log ring buffer | **could be MBs** | The "push fixed-size binary records, format elsewhere" structure. The other potentially large allocation — size it deliberately. |
| sequence / session state | tiny, per feed | gap detection, A/B arbitration |
| position / PnL | tiny | updated per fill |
| strategy state | tiny | fair value, EMAs |
| risk limits | tiny, read-only | pre-trade checks |
| TX: outbound buffer + SBE scratch | small | order encoding |
| TCP accumulation buffer | small | order entry is a byte **stream**, so partial messages must be buffered |

**The takeaway:** everything except the order map is either tiny, sequential, or heavily reused —
so it stays cached for free. **The map is the only structure that generates real misses**, which is
why it gets all the layout attention (entry size, alignment, prefetch) and nothing else does.

### DDIO: NIC DMAs straight into L3, and DMA coherence
**Intel DDIO (Data Direct I/O)** lets the NIC write incoming packets into **L3 instead of DRAM**.
Xeon, since Sandy Bridge-EP.
```
without DDIO:  NIC -> DRAM.  first read misses all the way out.   ~250 cycles
with DDIO:     NIC -> L3.    first read hits L3.                   ~50 cycles
```
The win is specifically the **first touch of every packet** — the one miss you can't avoid paying.

**What the DMA actually does:**
```
1. allocates the line in L3, marked DIRTY
2. INVALIDATES any copies of that address in L1 **or** L2 (either, both, or neither
   under non-inclusive) -- coherence
3. DRAM still holds the OLD value
```
Step 2 is essential: the buffer's lines may already be cached from decoding a previous packet at
that address, and without invalidation you'd read stale bytes.

⚠ **Invalidating them is free, though** — we only ever *read* the buffer (the NIC does the writing),
so our copies are clean and there's no writeback to pay. And a real bypass setup uses a **ring of
hundreds of buffers**, so consecutive packets land on *different* addresses; by the time it wraps,
those lines are long evicted anyway.

**The real reason for a ring isn't cache — it's overlap and correctness:**
```
single buffer:  the NIC must wait for us to finish decoding, or it OVERWRITES the
                packet we're still reading  <- a correctness bug
ring:           NIC fills buffer N+1 while we decode buffer N  <- pipelined
```
Firms size the ring for burst absorption — enough buffers that a microburst doesn't outrun the
consumer. Same reasoning as `SO_RCVBUF`, one layer down.

Your read then: L1 miss → L2 miss → **L3 hit (~50)**, installing into L1/L2 on the way in. ~11
lines per packet, then the decode runs at L1 speed.

⚠ **This changes the "buffer is permanently hot" picture.** In the socket path the *CPU* does the
copy, so the lines stay valid and dirty in L1. With DDIO the *NIC* invalidates them every packet,
so you take a fresh L3 hit each time — still far better, since you traded a syscall + kernel copy
for 11 L3 hits.

**Is coherence slow? No — that's why it's in hardware.** The invalidation is an interconnect
message overlapped with the DMA write that's happening anyway. Single-digit cycles, off your
critical path. The alternative (non-coherent DMA, some embedded ARM) makes *you* execute explicit
cache-invalidate instructions per line before reading — `dma_sync_single_for_cpu()` in Linux
drivers. **That** would be slow.

**Where coherence genuinely costs:**
```
NIC -> your core         one-way. invalidate, done. cheap.
core A <-> core B write  PING-PONG. the line bounces, each write killing the other's copy.
```
Only the second is expensive, because it's repeated and bidirectional — that's false sharing. The
practical consequence: **pin the receive thread to one core.** One writer, one reader, no sharing.
Two cores touching the buffer turns a cheap one-way invalidation into a ping-pong.

**Also worth knowing:**
- **DDIO ≠ CAT.** DDIO = *where* DMA lands. CAT = *partitioning* L3 against other cores. Separate,
  but they interact via one tunable (**IIO LLC Ways**, how many L3 ways DDIO may use): too many and
  packets evict your app data, too few and packets spill to DRAM. Firms set it deliberately.
- **Nothing is unevictable.** DDIO lines in L3 evict like anything else; CAT reserves ways *from
  other cores*, not from eviction. Residency comes from a small working set, not from pinning.
- **NUMA matters more than any of this.** The NIC hangs off **one socket's** PCIe lanes. Run the
  thread on the other socket and every packet crosses the inter-socket link (UPI / Infinity
  Fabric). Pin to a core on the NIC's socket.

### Q: Can a ring buffer be evicted, and how is it kept in cache in industry?
Yes. **Eviction is pressure-driven, not time-driven**: a line goes only when something else needs
its slot (LRU-ish). Granularity is one line (64 B x86, 128 B M4), so half a ring can be evicted
while the other half stays. A busy-polled ring is touched constantly, so its own consumer keeps it
hot — the danger is *competition*.

Core isolation alone is **not** immunity — it only protects the **private** caches. Firms layer four
defences:

| threat | defence |
|---|---|
| other threads / IRQs on your core (L1/L2) | `isolcpus` + `nohz_full` + IRQ affinity |
| other cores (L3 is **shared**) | **CAT** — reserve L3 ways |
| your own book/map/code | keep the total working set under L2 |
| the NIC overrunning its own ring | size the ring to fit the DDIO ways ("leaky DMA" otherwise) |

**Gotcha:** "isolate the core and it's never evicted" is the common overstatement. L3 is shared and
DDIO writes into it regardless of which core you isolated. Linux/x86 only.

See also: Reserving cache (CAT), DDIO above; [hft_systems.md](hft_systems.md) — ops setup.
