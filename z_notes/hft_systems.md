# HFT Systems

How a tier-1 firm lays out machines, cores and processes — then how this project maps onto that.
General practice from public sources (talks, vendor docs, job posts); specific firms' stacks aren't
public, so read this as "a firm of that class."

---

## How firms build it

### What's parallel, and what can't be
**The parallelism axis is SYMBOLS, never messages.** Decoding one packet is strictly sequential
(every E/C/X/D/U depends on an earlier A via the order map), and one symbol's book must apply
messages in sequence order.

What *can* run in parallel, each usually pinned to its own core:
```
one feed handler per venue/channel     sharding by symbol group (core N owns symbols N*k..N*k+k)
A/B arbitration                        strategy, order entry (TX), logging / telemetry
```
Connected by **SPSC (single-producer single-consumer) lock-free rings** — no locks on the hot path.

### Two architectures; market makers usually pick shard-per-core
```
pipelined:       core0 decode -> ring -> core1 book -> ring -> core2 strategy
                 every handoff is a cross-core transfer (~50-100 cycles per line)
shard-per-core:  core0 does decode + book + strategy for symbols A-F; core1 for G-M
                 no handoffs at all -- the whole pipeline is core-local
```
**It also wins on cache privacy:** L1 and (on most Xeons) L2 are private per core, L3 is shared. A
shard's working set lives in its own core's L1/L2 and nothing else evicts it.

### Sharding exists to make the working set fit
```
8000 symbols, ~4M live orders x 16 B = 64 MB    -> fits in NO cache
```
- **Filter to subscribed symbols** — trading 50 names means ignoring 7,950; ~20k live orders =
  320 KB. A `locate -> book` table with a "not traded" sentinel is a cache-residency tool as much as
  a CPU one: unwanted symbols never touch the map.
- **Shard by symbol across cores** — each core's map is a fraction of the total.

Not a throughput decision. A realistic per-core budget for a 50-symbol market maker is **a few MB**.

### Why not one machine per stock?
Cost, correlation, and no benefit for most names. Firms **shard by core, not by box**: correlated
symbols share a machine, the biggest names get a dedicated core, the long tail packs many per core.
| reason | |
|---|---|
| cost | ~10,000 US symbols × colo rack space/power = millions/month |
| correlation | MSFT's quote depends on AAPL/NVDA/QQQ. Cross-box hop **~1–2 µs**; cross-core on one socket **tens of ns** |
| ingest | feeds are multicast per *channel* (many symbols), so every box still parses the whole channel |
| volume | most symbols are quiet; one core handles hundreds sequentially |
> ⚠️ "some firms give SPY/AAPL their own server" — plausible, not confirmed.

### Standard operational setup (all Linux)
```
isolcpus / cset      nothing else is ever scheduled on the trading core
IRQ affinity         the NIC interrupt lands on the receive thread's core
NUMA placement       the NIC hangs off ONE socket; wrong socket = every packet crosses
                     UPI / Infinity Fabric. MATTERS MORE THAN ANY CACHE TUNING.
hugepages            far fewer TLB entries for big tables (hardware.md)
mlock / mlockall     pin pages in RAM; a page fault on the hot path is MILLISECONDS
busy-poll            never sleep; burn a core so the thread is awake when a packet lands
Intel CAT            reserve L3 ways so a noisy neighbour can't evict you
cache warming        background loop touching hot structures during quiet periods
PGO / BOLT           some firms; measured branch frequencies beat hand annotation
```
At the extreme end, **FPGA NICs** do the decode — sometimes the trading decision — in hardware and
emit the order without touching a CPU core. Software handles everything that tolerates
microseconds: book building for slower strategies, risk, position, logging.

### Dynamic memory: only off the hot path
The line is **which path**, not latency vs throughput: the tick-to-trade thread never allocates
after startup; everything else may.
| where | allocate? | why |
|---|---|---|
| startup (even on the hot thread) | yes | size once, warm, `mlock` |
| tick-to-trade thread, after start | **never** | judged on **worst case** (p99.9) |
| pricing model / params | yes, **off** the hot path | publish into a preallocated table the hot path reads |
| research / backtests | yes | dev speed wins (still avoid it in inner loops) |

malloc's fast path is ~tens of ns, but an `mmap` / first-touch page fault is ~µs and lock contention
is unbounded. Firms use fixed arrays, object pools and rings; some count `malloc` calls during a
replay test and fail the run if it isn't 0 ⚠️.

### What's engineered vs what's measured
```
engineered:  structure sizes, memory layout, sharding, core/IRQ affinity, NUMA, hugepages
measured:    L1/L2/L3 miss rates, p50/p99/p99.9/max per stage, tracked PER BUILD
NOT mapped:  actual cache contents. Nobody knows or controls which line holds what.
```
Much of it is **empirical** — change one thing, measure p99.9, keep or revert.
| tradeoff firms make | |
|---|---|
| bigger table | shorter probes, worse cache residency — past a point every lookup is DRAM anyway |
| predictability over average speed | plain linear probing is common *because* it's predictable |
| burn a core busy-polling | a core is cheap, microseconds of jitter are not |
| memory for alignment padding | a little more memory beats a doubled cache miss |
| trust exchange fields, validate framing | per-field checks cost latency on every message for errors that don't happen; gaps and framing are what actually go wrong |

⚠ Firms vary enormously in rigour, and a lot of HFT advice is cargo-culted rather than measured.
Research/backtesting runs on **separate hosts**, so it can't compete for cache, cores or PCIe.

### The three HFT tricks are the same idea
| | the standard way | what we do | what changed |
|---|---|---|---|
| **kernel bypass** | syscall + copy through the stack | NIC DMAs into our memory, we poll | removed a layer of indirection |
| **ring buffer** | allocate per message | fixed slots, reused | removed allocation |
| **flat map** | buckets of linked nodes | one array, index arithmetic | removed pointer chasing |

Each trades generality for **knowing something about our data**: packets are fixed-size and
transient, refs are sequential, prices are dense ticks. **The question isn't "what's the fast data
structure?" — it's "what do I know about my data that `std::unordered_map` isn't allowed to assume?"**

---

## This project

### End-to-end structure, and what this machine limits
A closed loop for **one symbol** first: data in → book → quote → order out → fills → position → next
quote. The exchange is a stub (ITCH replay + a gateway that acks/fills by a dumb rule).
```
UDP ITCH -> MoldUDP64 session (seq/gaps/A-B) -> parse -> dispatch (stock_locate) -> book
book -> strategy (stub) -> OMS -> risk (2-3 checks) -> SBE encode -> TCP order entry
TCP exec reports (ack/fill/reject) -> parse -> OMS -> position -> strategy
```
| limitation here (M4 MacBook, macOS) | industry (x86 Xeon/EPYC, Linux) |
|---|---|
| no kernel bypass, sockets only | NIC DMAs into user memory (DPDK / ef_vi / Onload) |
| no core pinning/isolation, CAT, DDIO, hugepages | `isolcpus`, IRQ affinity, CAT, DDIO, hugepages |
| no sharding yet | shard symbols across cores, correlated names together |
| 128 B lines, 16 MB L2 shared per cluster | 64 B lines, ~1–2 MB L2 **per core** → a 1 MB map is borderline L2/L3 there |
| latency numbers are macOS-socket numbers | tuned bypass, tick-to-trade ~1–5 µs (FPGA ~10s–100s ns) |
| P&L meaningless (replay doesn't react, fake fills) | real matching, queue position modelled |

**What it does prove:** book correctness vs a real ITCH day, the full order lifecycle, and a
per-stage latency breakdown — quote these as *relative*, not absolute. Keep OS code behind
`transport/` and no globals in the book, so Linux/bypass/sharding later is a swap, not a redesign.

### Folders
```
types/       Price, Qty, Side — vocabulary, no wire knowledge, compiles alone
codec/       ITCH offsets + decoders — all inbound wire knowledge (SBE later)
state/       order map + book (ladders, bitmaps, best bid/ask) — the market state
transport/   UDP socket, busy-poll receive (MoldUDP64 later)
session/     sequence numbers, gap detection — not started
app/         main loop; next: 500 books + locate -> book table
test/        mirrors the source tree
```
Rule: `codec/` never includes `state/` (planned `decode_packet<Handler>`).

### Where the order map lives
**One order map per `Book`**, owned by whichever core runs that symbol — order state follows the
sharding.
```
locate_to_book[stock_locate] -> Book (one per traded symbol, static storage, ~1.08 MB)
                                 ├─ orders_   open-addressing map, 65,536 × 16 B = 1 MB
                                 ├─ bids_/asks_  4,097 × 4 B each, alignas(128) ≈ 32 KB
                                 ├─ bid/ask bitmaps  64 words each = 1 KB
                                 └─ best_bid_ / best_ask_ cursors, low_
```
500 books ≈ 540 MB — most of it cold order-map slots, which is the sizing problem in issue #25.
Why this layout and not a shared map: [decisions.md](decisions.md).

### What lives in memory, and why
| | size | notes |
|---|---|---|
| receive buffer | 64 KB alloc, **~1.4 KB hot** | holds one datagram; ~11 lines, permanently hot |
| decode locals | **0** | SROA keeps the `Fields` structs in registers |
| **order map** (per book) | **1 MB** | the only random-access structure → **the only real miss source** |
| ladders (per book) | ~32 KB | sequential near the touch, stays hot |
| bitmaps (per book) | 1 KB | scanned only when the best level empties |
| the code | tens of KB | competes for L1i, and with data in the unified L2 |

Order maps scale with **live orders**; ladders scale with **symbols × window** (4,096 ticks × 2 sides
× 4 B × 500 books ≈ 16 MB). Everything except the order map is tiny, sequential or heavily reused,
which is why the map gets all the layout attention (entry size, alignment, prefetch, hugepages).

**Later phases add:** our own open-orders map (tens of quotes — separate from the market's, needed to
cancel and to recognise our own fills), a log ring (could be MBs — size it deliberately), sequence /
session state, position / PnL, strategy state, risk limits, an SBE TX buffer, and a TCP accumulation
buffer (order entry is a stream).
