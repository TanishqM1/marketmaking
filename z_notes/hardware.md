# Hardware & OS

The CPU, its caches, and the OS path a packet takes to reach us. Numbers are this M4 MacBook unless
noted; production firms run x86-64 Linux. Every entry should end on *what a firm does about it*.

---

## CPU (arm64)

### Registers
31 general purpose, two names each: `x9` = full 64 bits, `w9` = low 32 of the **same** register.
Roles are the **ABI** (software convention), not hardware:
| | |
|---|---|
| `x0`–`x7` | arguments; `x0` is also the return value |
| `x8`–`x17` | scratch, **caller-saved** |
| `x19`–`x28` | **callee-saved** — restored before returning |
| `x29` / `x30` | frame pointer / link register (return address) |
| `sp`, `xzr`/`wzr` | stack pointer, hardwired zero |

The same ABI is why a struct >16 bytes is returned through memory. Instructions are a fixed
**32 bits** (x86 is 1–15 bytes). **arm64 is load/store:** load → compute → store, never operate on
memory directly — so decoding a field is `ldur` then `rev`.

### Instructions you'll see in our disassembly
| | |
|---|---|
| `ldrb w9, [x8]` / `ldur x9, [x8, #0xb]` | load 1 byte / 8 bytes from `x8 + 11` (`ur` = unscaled, unaligned OK) |
| `rev x0, x9` | reverse bytes — this **is** `std::byteswap` |
| `cmp` + `b.eq` / `b.ne` / `b.lo` | compare sets **flags**, the branch reads them. Every `if`/`switch` becomes this |
| `cset` / `csel` | set / select on a condition — a **branchless** `if` (our side-byte decode compiles to `cmp` + `cset`) |
| `clz` (`rbit`+`clz` for trailing) | count leading zeros — finds the highest set bit (the bitmap scans) |
| `ubfx` / `bfi` | bit-field extract / insert — what a C++ bit-field read/write compiles to |
| `br x12` | **indirect** jump to an address in a register (jump tables). Predicted by *target*, harder than a plain branch |
| `b` / `bl` / `ret` | branch / call (saves return address in `x30`) / return |
| `stp` / `ldp` | store/load a register pair — function prologue/epilogue |

Hex in disassembly is often ASCII: `0x53` = `'S'`, `0x44` = `'D'`. A leaf function needs no frame,
which is why `read_be64` is just `ldr`/`rev`/`ret`.

**vs MIPS:** same fixed 32-bit width and similar loads (`lw`/`ldr`), but **MIPS has no flags
register** — `slt $t0, $a0, $a1` writes the comparison to a register, then `bne` branches on it.

---

## Cache

### Sizes, and the 128 vs 64 byte line
| | perf core (M4) | typical x86 server |
|---|---|---|
| L1i | **192 KB** | ~32 KB |
| L1d | 128 KB | 32–48 KB |
| L2 | 16 MB **shared per cluster** | 1–2 MB **per core** |
| L3 | — | 8–256 MB shared |
| **line** | **128 B** (`sysctl hw.cachelinesize`) | **64 B** |

L1 is split (code and data are fetched in the same cycle); L2/L3 are unified.

The real target is 64 B, which makes our layout math **tighter**:
| | M4 (128 B) | Xeon (64 B) |
|---|---|---|
| `OrderEntry` @ 16 B | 8 per line | **4** per line |
| price levels @ 4 B | 32 per line | **16** per line |
| probes before crossing a line | 8 | **4** |

Make it a named constant (`CACHE_LINE = 128`) so porting is one line. Code that fits a 192 KB L1i
may thrash a 32 KB one — "measure in Release" eventually becomes "measure on the target."
⚠ Some AMD EPYCs prefetch 128 B pairs, so false-sharing padding is sometimes 128 there anyway.

### Line, way, set
| | |
|---|---|
| **line** | 128 bytes — the unit of transfer. Touch one byte, the whole aligned line comes in |
| **way** | one line-slot inside a set ("8-way" = 8 per set) |
| **set** | an address maps to **exactly one** set |
```
L1d = 128 KB / 128 B = 1,024 lines / 8 ways = 128 sets
address: [ tag | 7-bit set index | 7-bit offset ]
```
⚠ The question is never "is L1 full?" but "is **this address's set** full?" 9 hot addresses in one
8-way set thrash while 1,015 lines sit unused (a *set conflict*). A big contiguous array spreads
evenly; two hot structures at unlucky addresses can evict each other with a tiny working set.
> 8-way is illustrative — Apple doesn't publish M4 associativity.

### Caches hold copies of RAM
RAM is the real storage; caches hold **copies of 128-byte chunks** of it (actual bytes), each tagged
with the address it copies. There's no "read from L2" instruction:
```
load 0x7fff5313:  L1? ~4 cycles | L2? ~15 | L3? ~50 | DRAM ~250, installed on the way back
```
Same instruction, same result, four latencies. Consequences:
- you can't tell a hit from a miss in code — only from timing or `perf` counters
- invisible to correctness — purely a performance concern
- one address can have several copies (L1+L2+L3) → the **coherence protocol** keeps them consistent

**Every level evicts, L3 included.** "Never leaves cache" is true only if the **working set fits**:
```
1 MB table,   16 MB L2   -> fits, effectively always cached
256 MB table, 32 MB L3   -> thrashes, most lookups miss to DRAM
```
So "make the table huge so the load factor is tiny" backfires — size to **peak live entries at
~50% load**. Firms also influence this when buying hardware (big L3, fewer active cores).

### Inclusion policy (inclusive / exclusive / non-inclusive)
On a DRAM miss a line can be installed in several levels at once; the policy decides which:
| | guarantee | firms run it? |
|---|---|---|
| **inclusive** | outer levels always hold a copy (L1 ⟹ L2 ⟹ L3) | old Intel |
| **exclusive** | a line is in exactly one level | AMD Zen L3 is a *victim cache* (holds lines evicted from L2) |
| **non-inclusive** | no guarantee either way | **Intel Skylake-SP and later Xeons** |

- The **search order is always L1 → L2 → L3 → DRAM** under every policy.
- Inclusive = less usable capacity but cheap coherence (L3 is a complete directory); exclusive = more
  capacity, needs a separate snoop filter.
- ⚠ **Eviction cascades:** a dirty L1 eviction writes to L2, whose full set evicts a dirty L2 line
  to L3… a variable latency source, and why write-heavy structures hurt the tail more.

⚠ **None of this changes your code.** Firms pick CPUs for cores, cache size, clock and PCIe lanes;
the policy comes along. The levers you control are **working-set size** and **access pattern**.

### Clean vs dirty lines
One bit per line: **clean** = matches the next level out; **dirty** = we wrote it, and the write
hasn't propagated (the cache holds the only current value). That bit *is* MESI's **M**odified state
(MESI = Modified / Exclusive / Shared / Invalid, the per-line coherence states).

**Writes land in L1 and propagate only on eviction** — write-back caching:
```
write -> L1, dirty | write again -> free | write again -> free | eviction -> written out once
```
100 writes, one writeback. Eviction of a clean line is free (just drop it); a dirty one must be
written out first. Nothing is lost on eviction — the next level has (or receives) the data.

In our code:
```cpp
find(ref)                 // READ  -> line stays clean -> eviction free
entry->remaining -= qty;  // WRITE -> dirty, but the line is already in L1 from find(): L1 speed
```
You pay the ~200-cycle miss once to *get* the line; everything after is L1 until eviction.

- ⚠ **Writes can miss too (write-allocate):** writing an uncached address loads the line first.
- ⚠ **Coherence → false sharing:** if two cores write the same line, each write invalidates the
  other's copy and the line ping-pongs. That's the expensive case.
- ⚠ The OS can also **swap** pages to disk under pressure — `mlock`/`mlockall` prevents it; standard
  HFT setup, since a page fault on the hot path is milliseconds.

### Laying out data for cache lines
**Goal: every byte you fetch is a byte you use.** Two requirements, both needed:
```cpp
struct OrderEntry { ... };                     // sizeof must DIVIDE the line (8/16/32/64/128)
alignas(128) OrderEntry slots_[CAPACITY];      // the array must START on a line boundary
```
```
start aligned, sizeof 24:  128/24 isn't whole -> some entries span 2 lines
sizeof 16, unaligned:      array starts mid-line -> some entries span 2 lines
both:                      8 per line, never straddles
```
Reading **one** straddling entry costs **two** misses.

⚠ **`alignas` goes on the ARRAY, not the struct.** On the struct, `sizeof` becomes 128 — 8× the
memory, 1 entry per line.

| space waster | fix |
|---|---|
| bad member order (`uint64_t` after `uint8_t` = 7 B padding) | largest first |
| constant stored as a member | `static constexpr` (0 bytes) |
| fields you never read (`match_number`, `tracking_num`) | don't store them |
| oversized types | smallest type that fits |

**The one place you WANT padding: false sharing.** Two variables written by *different threads*
get their own lines with `alignas(128)`. Opposite problem, opposite fix.

### Code layout and the I-cache
**The bytes you fetch should be bytes you execute.** Cold code (error handling) placed beside hot
code wastes part of every line. Code is demand-filled — you can't *load* it into L1i, only stop
cold code from evicting hot code. Locality is by **address**, not call order: two functions called
back to back but 200 KB apart don't help each other. Set conflicts apply too, so "50 KB hot in a
192 KB cache" guarantees nothing.

| control | what it does |
|---|---|
| `[[unlikely]]` / `[[likely]]` | moves a cold branch body out of line ([cpp.md](cpp.md)) |
| `[[gnu::cold]]` | whole function → `.text.unlikely`; marks every branch reaching it unlikely |
| `[[gnu::hot]]` | → `.text.hot`, grouped together |
| `-ffunction-sections` + linker order file | explicit symbol order from `perf` data |
| PGO | measured branch frequencies; splits cold blocks *within* hot functions |
| BOLT | post-link relayout from `perf` data (Linux/ELF only) |

It's **density, not size**: 50 KB of hot code packed together beats 10 KB scattered. Inlining is
part of this budget — see [cpp.md](cpp.md), inlining.
> Acting on it needs `perf record -e L1-icache-load-misses` — Linux only (issues #12, #16).

### Spatial vs temporal locality
**Spatial** tells you how to *lay out* data (neighbours share a line). **Temporal** tells you how to
*sequence* work: finish with data while it's still resident (loop tiling; LRU replacement exists to
exploit it).

| in our code | temporal reuse | result |
|---|---|---|
| receive buffer | **excellent** — same ~11 lines, millions of times | permanently hot |
| ladder near the touch | good — the same few levels | stays hot |
| **order map** | **poor** — random refs, touched a few times then gone | **the bottleneck** |

The map isn't slow just because access is random — there's **no reuse to exploit**, so the right
tools are a small footprint and prefetching, not better locality.

### TLB and hugepages — do quant firms use hugepages?
**Yes, on every latency-critical Linux box.** The **TLB** (translation lookaside buffer) caches
"virtual page → physical page". Miss it and the CPU walks the page table in RAM *before* the real
load even starts.

| page size | pages for 500 MB (500 books × 1 MB order maps) | fits a ~1–2k-entry TLB? |
|---|---|---|
| 4 KB (Linux default) | ~128,000 | no → page walks on random lookups |
| 2 MB | 250 | yes |
| 1 GB | 1 | yes |

The same applies to code: 300 KB of hot code over 4 KB pages is ~75 iTLB entries; hugepages for the
text segment make it 1–2.

**How firms do it:**
- **Explicit HugeTLB, THP off** (or `madvise`-only). THP's background `khugepaged` compacts memory
  at random times → latency spikes.
- **Reserve at boot** (`hugepagesz=1G hugepages=N`) before RAM fragments; allocate once at startup
  with `mmap(MAP_HUGETLB)`, then placement-new objects into it. Not a C++ feature — an OS one.
- **NUMA-local:** reserve on the socket where the trading thread and NIC live.
- Hugepages are never swapped. DPDK requires them; Onload/ExaNIC use them optionally ⚠️.

**Ours:** issue **#24**, Linux-only — macOS on Apple Silicon has no user-controlled hugepages ⚠️.

### Reserving cache: Intel CAT
**CAT** partitions the **L3 only**, by ways: "this core's class gets 4 of 16 L3 ways." You can't pin
a specific array. Firms use it so a **noisy neighbour** streaming memory can't evict their hot
structures, guaranteeing at least L3 (~50 cycles) over DRAM (~250). L1/L2 need no API — isolate the
core (`isolcpus` + IRQ affinity) and you own them. Intel *cache pseudo-locking* (Linux `resctrl`)
is the closest to true pinning; niche. ⚠ None of this exists on macOS; ARM's MPAM is immature.

### DDIO: the NIC writes straight into L3
**Intel DDIO** lets the NIC DMA packets into **L3 instead of DRAM** — the first read of every packet
hits L3 (~50 cycles) instead of DRAM (~250). The DMA allocates the line in L3 (dirty) and
**invalidates** any copies in L1/L2 so you never read stale bytes. That invalidation is cheap
(hardware, overlapped with the DMA, and our copies are clean because we only read the buffer).

- **Coherence is cheap one-way, expensive as ping-pong.** NIC → core: invalidate, done. Core A ↔
  core B both writing: the line bounces. So **pin the receive thread to one core.**
- **Why a ring of buffers:** with one buffer the NIC must wait or overwrite the packet you're still
  decoding (a correctness bug); with a ring it fills N+1 while you decode N. Size it for bursts.
- DDIO ≠ CAT, but they interact through one tunable (**IIO LLC ways**): too many and packets evict
  app data, too few and they spill to DRAM.
- **NUMA matters more than any of this:** run on the NIC's socket, or every packet crosses the
  inter-socket link.

### Can a ring buffer be evicted, and how is it kept in cache?
Yes — eviction is **pressure-driven, not time-driven**, one line at a time. A busy-polled ring is
touched constantly, so the danger is *competition*. Firms layer four defences:
| threat | defence |
|---|---|
| other threads / IRQs on your core (L1/L2) | `isolcpus` + `nohz_full` + IRQ affinity |
| other cores (L3 is **shared**) | CAT — reserve L3 ways |
| your own book / map / code | keep the total working set under L2 |
| the NIC overrunning its own ring | size the ring to fit the DDIO ways |

**Gotcha:** "isolate the core and it's never evicted" overstates it — L3 is shared and DDIO writes
into it regardless.

---

## OS and the network receive path

### Kernel vs userspace, and syscalls
Kernel and user memory are separated by **hardware** (privilege rings + page permissions) — your
process *cannot* touch kernel memory. So a normal receive makes **two copies**: NIC → kernel (DMA),
then kernel → us (`recvfrom`).

A **syscall** is a privilege transition, ~100 ns – 1 µs (worse with Spectre/Meltdown mitigations),
plus the copy. In a receive loop it's usually the **single dominant cost**. Hence:
| | |
|---|---|
| venues batch | ~40 messages per datagram = one syscall amortised over 40 |
| `recvmmsg` | many *datagrams* per syscall — **not on macOS** |
| kernel bypass | no syscall, no copy, no kernel |

### Where a packet sits, and what `recvfrom` does
```
1. NIC RX ring    <- NIC DMAs here (kernel memory). Ring holds descriptors -> separate ~2 KB buffers
   | interrupt -> softirq -> driver wraps it in an skb (an allocation) -> IP/UDP checks,
   | netfilter, socket lookup -> queued on the socket
2. socket queue   <- kernel memory
   | recvfrom: syscall in -> look at the queue -> COPY into our buffer -> syscall out
3. our buffer     <- user memory, what decode_packet sees
```
All the stack work already happened in interrupt context on another core — it's latency between
wire and us, and it's exactly what **kernel bypass** deletes:

| | normal socket | bypass (DPDK / ef_vi / Onload) |
|---|---|---|
| NIC DMAs into | kernel ring | **our buffer** |
| who notices | interrupt → softirq | we poll |
| skb, IP/UDP stack, netfilter, socket lookup | yes | none |
| copy to us / syscall | yes / yes | **no / no** |

### Blocking vs busy-poll vs bypass
```
blocking recvfrom   syscall + copy + THREAD SLEEPS, then a wake-up   (µs, highly variable)
busy-poll           syscall + copy, never sleeps                      <- ours: MSG_DONTWAIT
bypass              neither                                            (not possible on macOS)
```
HFT always busy-polls — burn a core so the thread is never asleep when a packet lands. Ours still
pays one syscall per poll, even an empty one (~0.5–1 µs ⚠️); only bypass removes that.

### `udp_receiver.cpp`
```
ctor        socket() -> setsockopt(SO_RCVBUF) -> bind()    setup, ONCE (may throw)
~dtor       close(fd)                                       RAII, ONCE
receive()   recvfrom(..., MSG_DONTWAIT, ...) -> {buffer, n} hot path, PER POLL (noexcept)
```
- `socket()` asks the kernel for an endpoint + receive queue and returns an fd (not the NIC).
  `AF_INET` = IPv4, `SOCK_DGRAM` = UDP.
- `SO_RCVBUF` grows the **kernel's** queue — prevents **drops** in bursts, does nothing for latency.
  The kernel silently clamps it (macOS `kern.ipc.maxsockbuf`) — `getsockopt` it back.
- `bind()` = "give me packets on this port"; `htons(port)` because ports are big-endian.
- `receive()` doesn't decode — keeping codec out of transport makes a bypass swap one file.

| | `m_buffer` | `SO_RCVBUF` |
|---|---|---|
| whose memory | ours | the kernel's |
| holds | **one** datagram | a **queue** of many |
| sized by | largest datagram (65,507) | tolerable backlog |
| too small → | **truncation** | **drops** |

`recvfrom(fd, buffer, BUFFER_SIZE /*capacity*/, flags /*MSG_* bitmask*/, nullptr, nullptr)` returns
an `ssize_t`:
```
n > 0    bytes copied
n == 0   a valid EMPTY datagram, not EOF
n == -1  errno: EAGAIN/EWOULDBLOCK = nothing yet (normal with MSG_DONTWAIT), EINTR = retry,
         anything else = socket broken -> bail (today it looks like "nothing yet" and spins)
```
- ⚠ Check `n < 0` **before** casting to `size_t` — `(size_t)-1` is a 16-exabyte span.
- ⚠ The flags param takes `MSG_*`, not `IPPROTO_*` (`IPPROTO_UDP` = 17 silently sets two flag
  bits). `MSG_DONTWAIT` is **128 on macOS, 64 on Linux** — always use the constant.

### UDP is message-oriented; TCP is a stream
One `recvfrom` = exactly one complete datagram, never partial, never two — so a flat buffer needs
no accumulation logic and no ring. ⚠ TCP is a byte **stream**: one `read` can return half a message
or three and a half, so order entry will need an accumulation buffer.
