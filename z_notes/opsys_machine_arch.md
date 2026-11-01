# OS & Machine Architecture

### Q: big functions can extend past L1 cache, making cache misses — so we may not want them inline?

Right. Inlining copies the function body into every call site, so it grows the hot loop's
**instruction** footprint. If the loop stops fitting in L1i you pay I-cache misses on the exact
path you were trying to speed up. Inlining is a budget, not a free win.

**L1 is split in two** (the CPU fetches code and data in the same cycle, so one shared cache
would contend with itself). L2/L3 are unified.

- **L1i** — the code being executed
- **L1d** — the data it operates on

**Example:** this machine (10-core Apple silicon):

| | perf cores | efficiency cores |
|---|---|---|
| L1i | **192 KB** | 128 KB |
| L1d | 128 KB | 64 KB |
| L2 | 16 MB | 4 MB |

Cache line is **128 bytes**, not the usual 64 — matters later for false-sharing padding.

**When to inline anyway:** leaf functions smaller than the call sequence needed to invoke them.
`read_be16` is one load + one `bswap` — not inlining it is strictly worse. No judgement call there.

**When to measure first:** whole message decoders. Six inlined bodies in one `switch` makes
`decode_packet` big. Probably still worth it, but don't assume.

**Tool:**
```sh
clang++ -O2 -Rpass=inline -Rpass-missed=inline -c codec/itch.cpp
```
Shows what got inlined and what the compiler **declined**, with reasons. Release only — `-O0`
inlines nothing.

`[[gnu::always_inline]]` forces it, `[[gnu::noinline]]` prevents it. `noinline` on a cold branch
is a real technique — keeps rarely-taken code out of the hot path's footprint.

**Gotcha:** 192 KB of L1i is huge. Typical x86 server cores have **~32 KB**, so the inlining
budget here is ~6x what it'd be on the Linux/x86 box this would really run on. Code that fits
comfortably on this laptop could thrash on a Xeon. Same category of problem as kernel-bypass and
core-pinning APIs not existing on Darwin: "measure in Release" eventually becomes "measure on the
target."


### Q: does the cache depend on the order my code executes in? should hot path code run right beside each other?

**The conclusion is right, the mechanism isn't.** Cache depends on **address locality in the
binary**, not on execution order. Two functions called back-to-back but living 200 KB apart in the
binary don't help each other at all.

So "hot code beside each other" means **adjacent addresses**, not adjacent in time.

**You also can't *put* code in the I-cache.** It's demand-filled — code lands in L1i when it
executes, and gets evicted by other code. The goal isn't "load my hot 50 KB", it's "stop the cold
250 KB from evicting it."

**Why address adjacency matters — two mechanisms:**

- **Line granularity.** A cache line is 128 bytes on this machine. A hot function sandwiched
  between cold ones means every line fetched is part-hot/part-cold — capacity spent on code that
  never runs. Hot functions packed together means ~100% of each line is useful.
- **Set conflicts.** L1i is set-associative, not fully associative. An address maps to one
  specific set, and only N lines fit per set. Two hot functions whose addresses collide evict each
  other **even if total hot footprint is tiny**. This is why "50 KB hot, 192 KB cache" guarantees
  nothing — distribution matters, not just size.

**Gotcha — source file order is only a weak hint.** Putting functions next to each other in the
`.cpp` does *not* guarantee adjacent addresses. Within one TU they often land in source order by
default, so it partly works by accident, but the linker can arrange TUs however it likes, and
`-ffunction-sections` lets it reorder everything.

**The actual controls, cheapest first:**
```
[[gnu::hot]] / [[gnu::cold]]        coarse, free, most of the benefit.
                                    cold also marks every branch reaching it as unlikely,
                                    and optimises for size instead of speed.
-ffunction-sections + order file    explicit symbol ordering from perf data
                                    (-Wl,-order_file on ld64, --symbol-ordering-file on lld)
PGO                                 automatic; also splits cold branches WITHIN a hot function
BOLT (llvm-bolt)                    post-link relayout from perf data. Linux/ELF only.
```

**Also at larger code sizes: iTLB.** 300 KB of code spans ~75 pages at 4 KB, and the instruction
TLB is small — you start missing on *address translation*, not on the code. Fix is hugepages for
the text segment (2 MB pages → one or two TLB entries for the whole binary). Standard HFT
technique, often a bigger win than I-cache tuning.

> All of this needs `perf record -e L1-icache-load-misses` to act on, which doesn't exist on
> macOS. It's Linux-only work — see [../todo.md](../todo.md).


### Q: how do registers and instructions work on arm64? (vs MIPS)

**Registers** — 31 general purpose, two names for each:
```
x9   the full 64 bits
w9   the low 32 bits of the SAME register
```

Roles assigned by the ABI (calling convention), not the hardware:

| | |
|---|---|
| `x0`–`x7` | function **arguments**; `x0` is also the **return value** |
| `x8`–`x17` | scratch — **caller-saved** (a callee may clobber them freely) |
| `x19`–`x28` | **callee-saved** — if a function uses these it must restore them before returning |
| `x29` | frame pointer |
| `x30` | link register — holds the return address |
| `sp` | stack pointer |
| `xzr`/`wzr` | hardwired zero |

**Instructions** are fixed **32 bits** each, stored in the binary's `.text` section and fetched
into L1i as the CPU runs. (Fixed width is a RISC property — x86 instructions are variable length,
1–15 bytes.)

**arm64 is a load/store architecture:** you cannot operate on memory directly. Load into a
register, compute, store back. That's *why* decoding a field is `ldur` then `rev` rather than
"byteswap in place."

**The commands from our decode loop:**

| instruction | what it does |
|---|---|
| `ldrb w9, [x8]` | **L**oa**D** **R**egister **B**yte — 1 byte from address `x8` into `w9` |
| `ldur x9, [x8, #0xb]` | load 8 bytes from `x8 + 11`. `ur` = unscaled offset, allows unaligned. |
| `rev x0, x9` | **REV**erse bytes — this *is* `std::byteswap` |
| `cmp w9, #0x58` | **C**o**MP**are — subtracts and sets **condition flags**. Stores nothing. |
| `b.eq` / `b.ne` / `b.lo` | branch if equal / not-equal / lower (unsigned `<`), reading those flags |
| `b 0x24` | unconditional branch |
| `bl target` | **B**ranch with **L**ink — a function call. Saves the return address in `x30`. |
| `ret` | return — branches to whatever is in `x30` |
| `mov w8, #0x17` | move an immediate (23) into a register |
| `stp`/`ldp` | **ST**ore/**L**oa**D** **P**air — two registers at once, used for save/restore |

**`cmp` + `b.xx` is the fundamental pattern:** one instruction sets flags, the next acts on them.
Every `if` and `switch` becomes this.

**Example** — the hex in a disassembly is often ASCII:
```
cmp w9, #0x58    0x58 = 88 = 'X'   -> if (type != 'X')
cmp w9, #0x44    0x44 = 68 = 'D'
mov w8, #0x17    0x17 = 23         = OrderCancel::length
```

**Why `stp x22, x21, [sp, #-0x30]!` appears at the top of a function:** it's saving
**callee-saved** registers (and `x29`/`x30`) because the function both uses them and makes a call.
A leaf function that calls nothing needs no stack frame at all — which is why `read_be64` is just
`ldr`/`rev`/`ret`.

### vs MIPS

| | MIPS | arm64 |
|---|---|---|
| registers | 32 (`$0`–`$31`), `$zero` hardwired | 31 + `xzr` |
| return address | `$ra` (= `$31`) | `x30` (link register) |
| call / return | `jal` / `jr $ra` | `bl` / `ret` |
| load word / byte | `lw` / `lb` | `ldr` / `ldrb` |
| arg registers | `$a0`–`$a3` (4) | `x0`–`x7` (8) |
| return value | `$v0`, `$v1` | `x0` (and `x1`) |
| instruction width | fixed 32-bit | fixed 32-bit |
| load/store arch | yes | yes |
| **condition flags** | **none** | **yes (NZCV)** |

**The one real difference to internalise: MIPS has no flags register.** Comparison results go
into a general register, then you branch on that register:
```
MIPS:    slt $t0, $a0, $a1      # $t0 = (a0 < a1)
         bne $t0, $zero, label  # branch if $t0 != 0
arm64:   cmp x0, x1             # sets flags
         b.lo label             # branch reading the flags
```
Same two-instruction shape, but arm64's flags are implicit state rather than a named register.

**Gotcha:** register *roles* (`x0` = first argument) are a **software convention**, not hardware.
The CPU doesn't care. It's the ABI — which is also why a struct >16 bytes gets returned through
memory: the ABI says so, not the chip. See also: [cpp.md](cpp.md) — when structs exist in memory.


### Q: what does recvfrom actually do? isn't there a ring buffer anyway, it just goes through the kernel stack first?

Yes — **the NIC uses a ring buffer by default.** It's just in *kernel* memory, and the *kernel*
reads it, not you. `recvfrom` doesn't touch that ring; it fetches an already-processed packet
from the socket's queue, which sits downstream of all the stack work.

**Three places a packet sits in the normal path:**
```
1. NIC RX ring       <- NIC DMAs the packet here (kernel memory)
   |  interrupt -> kernel does the stack work
2. socket queue      <- processed packet, waiting for you
   |  recvfrom copies it
3. your buffer       <- what decode_packet sees
```

**What `recvfrom` does — only 4 things:**
1. syscall transition (user -> kernel privilege)
2. look at the socket's receive queue
3. **copy** the bytes into your buffer
4. transition back

That's it. Not "check if a packet arrived and process it" — the processing already happened
earlier, in interrupt context, on whatever core the NIC's interrupt landed on.

**What happened between ring and queue (all of this is what bypass deletes):**
- interrupt fires, kernel schedules a softirq
- driver pulls the packet off the RX ring, wraps it in an **skb** (kernel packet struct = an allocation)
- ethernet header check
- IP layer: checksum, routing decision, **netfilter** hooks (firewall rules run here)
- UDP layer: checksum, **look up which socket** owns this port
- queue the skb on that socket

None of it runs on your thread, but it's all latency between the wire and you.

| | normal socket | bypass |
|---|---|---|
| NIC DMAs into | kernel ring | **your buffer** |
| who notices | interrupt -> softirq | you poll |
| skb allocated | yes | no |
| eth/IP/UDP stack, netfilter | yes | **none** |
| socket lookup | yes | none |
| copy to you | yes | **no** |
| syscall | yes | **no** |

Bypass collapses the three locations into **one**: your buffer *is* the DMA target. The NIC
writes straight into memory your process owns, and you poll a ring in your own address space.

**Gotcha — there's a middle ground, and it's the one that works on macOS:**
```
blocking recvfrom    syscall + copy + THREAD SLEEPS, then a context switch to wake
busy-poll            syscall + copy. no sleep, no wake.
bypass               neither
```
The sleep-and-wake is microseconds *and* highly variable — the worst thing for tail latency.
That's why trading systems busy-poll: burn a core spinning so the thread is never asleep when a
packet lands. Measuring blocking vs busy-poll is a real number we CAN get on this machine.


### Q: the NIC ring buffer holds pointers only, right? so a packet is always contiguous when copied to us?

Right. **The NIC ring contains descriptors, not packet bytes** — small fixed-size structs
(~16-32 bytes) that each *point to* a separate buffer elsewhere in memory. The ring wraps; the
packets don't, because they aren't stored in the ring at all.

```
NIC ring:   [desc][desc][desc][desc]...   <- circular, wraps around
               |     |     |
               v     v     v
buffers:    [pkt] [pkt] [pkt]              <- each separate, each contiguous
```

So the "what if a packet wraps around the end of the ring" problem **does not exist**. Each
packet sits in its own contiguous buffer.

**Example:** the buffers are fixed-size *slots* (typically 2KB, or page-sized). The packet
occupying one is whatever length actually arrived — a 1400-byte datagram in a 2KB slot. Fixed
slots, variable-length packets.

**The full path to our buffer** (note the skb step — it's not a direct copy from the
descriptor's buffer):
```
NIC DMAs packet into a buffer, writes a descriptor into the ring
driver reads the descriptor -> finds the buffer -> wraps it in an skb
stack processes the skb (eth/IP/UDP), queues it on the socket
recvfrom copies from the skb into OUR flat array
```

**Gotcha:** an skb *can* be non-linear — data split across fragments, for large or
hardware-offloaded packets. `recvfrom` handles that by walking the fragments and handing us one
contiguous result either way. For a 1400-byte UDP datagram it's typically one piece, so the
question is mostly moot.

**Gotcha 2:** our flat array is **not** a ring buffer. One 64KB array, holds exactly one
datagram, overwritten on every read. A ring buffer only earns its place later: receive and decode
on different threads (SPSC ring between them), or with kernel bypass (NIC DMAs into a ring of
*our* buffers and we poll it).


### Q: what does each part of udp_receiver.cpp do?

Three parts with three different lifetimes: **constructor** runs once (open + bind a socket),
**destructor** runs once (close it), **receive()** runs millions of times (one syscall, one
packet).

```
ctor        socket() -> setsockopt(SO_RCVBUF) -> bind()      setup, once
~dtor       close(fd_)                                        RAII cleanup, once
receive()   recvfrom() -> return {buffer_, n}                 hot path, per packet
```

- **`socket()`** — asks the kernel for an endpoint object + a receive queue, returns an fd.
  `AF_INET` = IPv4, `SOCK_DGRAM` = UDP.
- **`setsockopt(SO_RCVBUF)`** — grows the **kernel's** queue. Prevents DROPS during bursts; does
  nothing for latency.
- **`bind()`** — "give me packets arriving on this port." Skip it and you receive nothing,
  silently. `htons(port)` is mandatory: ports are network byte order (big-endian).
- **`receive()`** — does NOT decode. Returns bytes; `app/main.cpp` calls `decode_packet`.
  Keeping codec out of transport is what makes the kernel-bypass swap a one-file change.

**recvfrom's 6 params:**
```
recvfrom(fd_,          // 1 which socket's queue to read
         buffer_,      // 2 destination - where to copy TO
         BUFFER_SIZE,  // 3 CAPACITY of that destination (not expected size)
         0,            // 4 flags bitmask. 0 = no modifiers = BLOCKING.
         nullptr,      // 5 out-param for sender's address - we don't care
         nullptr);     // 6 pairs with 5, same value
```

It copies **one** datagram from the kernel queue into our buffer and **returns only an
`ssize_t`** — the byte count. The data arrived as a side effect.
```
n > 0    bytes copied -> return {buffer_, n}
n == 0   valid EMPTY datagram, not EOF
n == -1  error, reason in errno:
           EAGAIN/EWOULDBLOCK -> nothing available (normal with MSG_DONTWAIT)
           EINTR              -> signal interrupted, retry
           anything else      -> socket is broken, BAIL (don't loop, you'll spin at 100%)
```

**Gotcha — check `n < 0` BEFORE casting to size_t.** Cast -1 to unsigned and you get
18446744073709551615, so `span{buffer_, that}` is a 16-exabyte span and any read is instant UB.

**Gotcha 2 — param 4 takes `MSG_*` flags, not `IPPROTO_*`.** `IPPROTO_UDP` belongs to `socket()`.
It's the number 17, so passing it to recvfrom silently turns on two arbitrary MSG bits. Compiles
fine — both params are plain `int`. (Exactly the mix-up strong types like `Price`/`Qty` prevent.)

**Blocking (`flags = 0`) vs busy-poll (`MSG_DONTWAIT`):** blocking sleeps the thread and the
kernel wakes it — 2 context switches, microseconds, and **jittery**. Busy-poll spins and never
sleeps: burns a whole core, best tail latency. HFT always busy-polls. `receive()` returning a
possibly-empty span is what lets us switch with one line and no change to `main.cpp`.
