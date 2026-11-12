# HFT Systems

How a tier-1 firm lays out machines, cores and processes. General industry practice from public
sources (conference talks, vendor docs, job posts) — specific firms' stacks aren't public, so read
this as "a firm of that class."

### What's parallel, and what can't be
**The parallelism axis is SYMBOLS, never messages.**

Cannot be parallelised:
- **Decoding one packet.** Every `E`/`C`/`X`/`D`/`U` depends on an earlier `A` via the order map.
  Strictly sequential.
- **Applying messages to one symbol's book.** Must be in sequence order.

Can be:
```
one feed handler per venue/channel     each pinned to its own core
sharding by symbol group               core N owns symbols N*k .. N*k+k
A/B arbitration                        two sockets feeding one arbitrator
strategy                               often its own core
order entry (TX)                       often its own core
logging / telemetry                    always its own core
```
Connected by **SPSC (single-producer single-consumer) lock-free ring buffers** — no locks anywhere
on the hot path.

### Two architectures; market makers usually pick the second
```
pipelined:       core0 decode -> ring -> core1 book -> ring -> core2 strategy
                 each stage specialised, but EVERY handoff is a cross-core transfer

shard-per-core:  core0 does decode + book + strategy for symbols A-F
                 core1 does the same for G-M
                 no handoffs at all -- the whole pipeline is core-local
```
**Shard-per-core wins for market making because nothing crosses cores on the hot path.**

**The cost of crossing:** a line moving from core A's L1 to core B's L1 goes via L3 or a
cache-to-cache transfer — **~50-100 cycles**, per line, per handoff.

### Which caches are private
| | private or shared |
|---|---|
| **L1i / L1d** | **private per core**, always |
| **L2** | private per core on most Xeons; shared per cluster on Apple Silicon |
| **L3** | **shared** across all cores on a socket |

**This is the real reason sharding helps cache — it's privacy, not just capacity.** Each core gets
its own L1/L2, so a shard's working set is genuinely private and nothing else evicts it.

### Sharding exists to make the working set fit
```
8000 symbols, ~4M live orders x 16 B = 64 MB    -> fits in NO cache
```
- **filter to subscribed symbols** — trading 50 names means ignoring 7,950. ~20k live orders = 320 KB.
- **shard by symbol across cores** — each core's map is a fraction of the total.

Not a throughput decision. See [cache.md](cache.md) — working set budget.

### Standard operational setup (all Linux)
```
isolcpus / cset      nothing else is ever scheduled on the trading core
IRQ affinity         the NIC interrupt lands on the SAME core as the receive thread, so the
                     kernel's copy warms THAT core's L1 rather than a neighbour's
NUMA placement       the NIC hangs off ONE socket's PCIe lanes. Wrong socket = every packet
                     crosses UPI / Infinity Fabric. Pin to a core on the NIC's socket.
                     MATTERS MORE THAN ANY CACHE TUNING.
hugepages            2 MB pages -> far fewer TLB entries for a large structure
mlock / mlockall     pin pages in physical RAM; a page fault on the hot path is MILLISECONDS
busy-poll            never sleep. Burn a core so the thread is awake when a packet lands.
Intel CAT            reserve L3 ways so a noisy neighbour can't evict you
cache warming        background loop touching hot structures during quiet periods
PGO / BOLT           some firms; measured branch frequencies beat hand annotation
```

### The extreme end: the CPU leaves the critical path
Firms at that tier publicly use **FPGA NICs**, where the decode — and sometimes the trading
decision — happens in hardware. The packet never reaches a CPU core for the fastest strategies: the
FPGA recognises a condition and emits an order directly.

Software then handles everything that *isn't* the single hottest path: book building for strategies
that tolerate microseconds, risk, position, logging.

### What's engineered vs what's measured
```
engineered:  structure sizes, memory layout, sharding, core/IRQ affinity, NUMA, hugepages
measured:    L1/L2/L3 miss rates, p50/p99/p99.9/max per stage, tracked PER BUILD
NOT mapped:  actual cache contents. Nobody knows or controls which line holds what.
```
Much of it is **empirical, not derived** — change one thing, measure p99.9, keep or revert.

**Tradeoffs they actually make:**
| | |
|---|---|
| bigger table | shorter probes, worse cache residency — past a point every lookup is DRAM anyway |
| predictability over average speed | plain linear probing is common *because* it's predictable; p99.9 is the product |
| burn a core busy-polling | a core is cheap, microseconds of jitter are not |
| memory for alignment padding | 11% more memory beats a doubled cache miss |

⚠ **Firms vary enormously in rigour**, and a lot of circulating HFT advice is cargo-culted rather
than measured. Also: research/backtesting (Python) runs on **separate hosts entirely**, so it can't
compete for cache, cores or PCIe bandwidth with the trading path.

### Q: Why not one machine per stock?
Cost, correlation, and no benefit for most names. Firms **shard by core, not by box**: correlated
symbols share a machine, the biggest names get a dedicated core, the long tail packs many per core.

| reason | |
|---|---|
| cost | ~10,000 US symbols × colo rack space/power = millions/month |
| correlation | MSFT's quote depends on AAPL/NVDA/QQQ. Cross-box = network hop **~1–2 µs**; cross-core on one socket = **tens of ns** |
| ingest | feeds are multicast per *channel* (many symbols), so every box still parses the whole channel |
| volume | most symbols are quiet; one core handles hundreds sequentially, no queue |

> ⚠️ verify: "some firms give SPY/AAPL their own server" — plausible, not confirmed.

See also: Sharding exists to make the working set fit (above).

### Q: TLDR of this project's e2e structure, and what this machine limits
A closed loop for **one symbol**: data in → book → quote → order out → fills back → position →
next quote. The exchange is a stub (ITCH file replay + a gateway that acks/fills by a dumb rule).
```
UDP ITCH → ring → MoldUDP64 session (seq/gaps/A-B) → parse → dispatch (stock_locate) → book
book → strategy (stub) → OMS → risk (2-3 real checks) → SBE encode → TCP order entry
TCP exec reports (ack/fill/reject) → parse → OMS → position → strategy
```

| limitation here (M4 MacBook, macOS) | industry (x86 Xeon/EPYC, Linux) |
|---|---|
| no kernel bypass, sockets only | NIC DMAs into user memory (DPDK/ef_vi/Onload) |
| no core pinning/isolation, CAT, DDIO, hugepage control | `isolcpus`, IRQ affinity, CAT, DDIO, hugepages |
| one book, no sharding | shard symbols across cores, correlated names together |
| 128 B lines, 16 MB L2 shared per cluster | 64 B lines, ~1-2 MB L2 **per core** → our 1 MB map is borderline L2 / L3 there |
| latency numbers are macOS-socket numbers | tuned bypass, tick-to-trade ~1-5 µs (FPGA: ~10s-100s ns) |
| P&L meaningless: replay doesn't react, fake fills, no queue position | real matching, queue position modelled |

**What it does prove:** book correctness vs a real ITCH day, the full order lifecycle (incl. fill-
after-cancel, rejects), and a per-stage latency breakdown — quote these as *relative*, not absolute.

**Gotcha:** keep OS code behind `transport/` and no globals in the book, so Linux/bypass/sharding
later is a swap, not a redesign.

### Q: Avoid dynamic memory on the hot path; use it where throughput / dev speed matter?
Mostly right. The line is **which path**, not latency vs throughput: the tick-to-trade thread never
allocates after startup; everything else may.

| where | allocate? | why |
|---|---|---|
| startup (even on the hot thread) | yes | size once, warm, `mlock` |
| tick-to-trade thread, after start | **never** | judged on **worst case** (p99.9) |
| pricing model / params | yes, **off** the hot path | publish results into a preallocated table the hot path reads |
| research / backtests | yes | dev speed wins |

Why malloc hurts the tail: fast path ~tens of ns, but `mmap` syscall / first-touch page fault ~µs,
lock contention unbounded.

**Gotcha:** throughput code also avoids malloc in inner loops (per-event malloc makes a backtest
several times slower). It just tolerates the occasional slow allocation, because only the average matters.

Firms: fixed arrays, object pools, ring buffers; some count `malloc` calls during a replay test
and fail the run if the count isn't 0.
> ⚠️ verify: the malloc-count CI check is a common pattern, not a confirmed specific firm practice.

See also: [bad_habits.md](bad_habits.md) — no `new`/`delete` on the hot path

### Q: our architecture — where does the order map live?
**One small order map per `Book`**, owned by the core that runs that symbol. Order state follows
the sharding: whoever owns the book owns its orders.
```
locate_to_book[stock_locate] -> Book (one per traded symbol, static storage)
                                 ├─ orders_  open-addressing map, ref & MASK, 65,536 × 16 B = 1 MB,
                                 │           backward-shift erase (state/order_map.hpp)
                                 └─ bids_/asks_ tick ladders + best cursors  (state/book.hpp)
```

| | per-book map (ours) | one shared array `orders[ref]` |
|---|---|---|
| fits | few symbols, sharded by core | one process maintaining **every** book on the feed |
| memory | ∝ live orders (1 MB, L2-hot) | ∝ day's total refs (GBs, TLB/cache misses) |
| lookup | hash + 1–2 probes | 1 load, but likely a cold one |
| cores | private per core, no sharing | shared → cache-line bouncing or locks |

Why ours: ITCH refs come from **one counter across all symbols**, so a single symbol's refs are
sparse — a direct array per book would be mostly holes. A hash map sized to live orders stays hot.
> ⚠️ verify: firm internals aren't public; "hot hash map beats cold direct array" is reasoned, not
> measured — open question in `z_questions/technical/questons.md`.

See also: [data_structures.md](data_structures.md) — backward-shift deletion, direct-array layout.

### Q: do quant firms use hugepages
Yes — standard on every latency-critical Linux box. Big pages = fewer address translations = no
TLB misses on big tables, and they never get swapped out.

**TLB** (translation lookaside buffer) = the CPU's small cache of "virtual page → physical page".
Miss it and the CPU walks the page table in RAM *before* it can even start the real load.
| page size | pages to cover 500 MB (our 500 books × 1 MB order maps) | fits in TLB (~1–2k entries)? |
|---|---|---|
| 4 KB (Linux default) | ~128,000 | no → page walks on random lookups |
| 2 MB | 250 | yes |
| 1 GB | 1 | yes |

**Why firms use them:**
- **No TLB misses** on order maps, books, ring buffers — random probes stay ns, not page-walk + miss.
- **No swapping** — explicit hugepages (HugeTLB) stay in RAM, so the OS can't page them out mid-open.
- **Kernel bypass needs them** — DPDK requires them for the NIC↔app zero-copy ring buffers;
  Onload/ExaNIC use them for packet buffers too. > ⚠️ verify: "require" is true for DPDK, optional for the others

**How firms set them up:**
- **Explicit HugeTLB, THP off.** THP (transparent huge pages) lets the kernel promote pages on its
  own; its background thread (`khugepaged`) compacts memory at random times → latency spikes.
  Firms turn it off (or `madvise`-only) and allocate hugepages deliberately.
- **Reserve at boot** — `hugepagesz=1G hugepages=N` on the kernel command line, before RAM fragments
  (1 GB pages need contiguous physical memory, hard to find later).
- **Allocate once at startup** — `mmap(MAP_HUGETLB)`, never on the hot path. Same rule as all memory.
- **NUMA-local** — reserve on the socket where the trading thread and NIC live; the other socket's
  RAM is slower.

**Our project:** flagged by review for `OrderMap::slots_` (1 MB/book). Can't do it on macOS Apple
Silicon (no user-controllable huge pages ⚠️ verify) → a Linux-only, production step.

See also: [data_structures.md](data_structures.md) — hugepages for the order table;
[cache.md](cache.md) — iTLB and hugepages for code.
