# OS & Machine Architecture

## arm64

### Registers
31 general purpose, two names each: `x9` = full 64 bits, `w9` = low 32 of the **same** register.

Roles are the **ABI** (software convention), not hardware — the CPU doesn't care:
| | |
|---|---|
| `x0`–`x7` | arguments; `x0` is also the return value |
| `x8`–`x17` | scratch, **caller-saved** (a callee may clobber freely) |
| `x19`–`x28` | **callee-saved** — must be restored before returning |
| `x29` / `x30` | frame pointer / link register (return address) |
| `sp`, `xzr`/`wzr` | stack pointer, hardwired zero |

That same ABI is why a struct >16 bytes is returned through memory — the convention says so, not
the chip.

Instructions are fixed **32 bits** each (a RISC property; x86 is variable, 1–15 bytes), stored in
`.text` and fetched into L1i as the CPU runs.

**arm64 is load/store:** you can't operate on memory directly. Load → compute → store. That's why
decoding a field is `ldur` then `rev`, not "byteswap in place."

### Instructions from our decode loop
| | |
|---|---|
| `ldrb w9, [x8]` | load 1 byte from address `x8` |
| `ldur x9, [x8, #0xb]` | load 8 bytes from `x8 + 11`. `ur` = unscaled, allows unaligned. |
| `rev x0, x9` | reverse bytes — this **is** `std::byteswap` |
| `cmp w9, #0x58` | compare: subtracts, sets **condition flags**, stores nothing |
| `b.eq` / `b.ne` / `b.lo` | branch if equal / not-equal / lower (unsigned `<`), reading those flags |
| `b` / `bl` / `ret` | branch / branch-with-link (a call, saves return addr in `x30`) / return via `x30` |
| `mov w8, #0x17` | move an immediate |
| `stp` / `ldp` | store/load **pair** — two registers at once, used for save/restore |

**`cmp` + `b.xx` is the fundamental pattern** — one instruction sets flags, the next acts on them.
Every `if` and `switch` becomes this.

Hex in a disassembly is often ASCII: `0x58` = 88 = `'X'`, `0x44` = 68 = `'D'`, `0x17` = 23 =
`OrderCancel::length`.

`stp x22, x21, [sp, #-0x30]!` at the top of a function is saving **callee-saved** registers
because the function both uses them and makes a call. A leaf function needs no frame at all —
which is why `read_be64` is just `ldr`/`rev`/`ret`.

### vs MIPS
| | MIPS | arm64 |
|---|---|---|
| registers | 32 (`$0`–`$31`) | 31 + `xzr` |
| return address | `$ra` | `x30` |
| call / return | `jal` / `jr $ra` | `bl` / `ret` |
| load word / byte | `lw` / `lb` | `ldr` / `ldrb` |
| arg registers | `$a0`–`$a3` (4) | `x0`–`x7` (8) |
| instruction width | fixed 32-bit | fixed 32-bit |
| **condition flags** | **none** | **yes (NZCV)** |

**The one real difference: MIPS has no flags register.** Comparison results go into a general
register, then you branch on it:
```
MIPS:    slt $t0, $a0, $a1      # $t0 = (a0 < a1)
         bne $t0, $zero, label
arm64:   cmp x0, x1             # sets flags
         b.lo label             # reads them
```

## Network receive path

### Where a packet sits
```
1. NIC RX ring     <- NIC DMAs here (KERNEL memory)
   |  interrupt -> kernel does the stack work
2. socket queue    <- processed packet, waiting (KERNEL memory)
   |  recvfrom copies it
3. our buffer      <- what decode_packet sees (USER memory)
```

**The NIC ring holds descriptors, not packet bytes** — small structs (~16-32 bytes) pointing to
separate buffers:
```
ring:     [desc][desc][desc]...   <- circular, wraps
             |     |     |
buffers:  [pkt] [pkt] [pkt]       <- each separate, each contiguous
```
So "what if a packet wraps the end of the ring" **can't happen** — packets aren't in the ring.
Buffers are fixed-size slots (typically 2 KB); the packet in one is whatever length arrived.

### `recvfrom` does only 4 things
1. syscall transition (user → kernel)
2. look at the socket's receive queue
3. **copy** the bytes into our buffer
4. transition back

Not "check if a packet arrived and process it" — the processing already happened earlier, in
interrupt context, on whatever core took the NIC's interrupt.

**What happened between ring and queue — all of it is what bypass deletes:**
- interrupt → softirq scheduled
- driver pulls from the RX ring, wraps it in an **skb** (kernel packet struct = an allocation)
- ethernet header check
- IP: checksum, routing decision, **netfilter** hooks (firewall rules)
- UDP: checksum, **look up which socket** owns the port
- queue the skb on that socket

None runs on our thread, but it's all latency between wire and us.

| | normal socket | bypass |
|---|---|---|
| NIC DMAs into | kernel ring | **our buffer** |
| who notices | interrupt → softirq | we poll |
| skb allocated | yes | no |
| eth/IP/UDP stack, netfilter | yes | **none** |
| socket lookup | yes | none |
| copy to us | yes | **no** |
| syscall | yes | **no** |

Bypass collapses the three locations into **one**: our buffer *is* the DMA target.

⚠ An skb *can* be non-linear (fragments, for large or offloaded packets). `recvfrom` walks them
and hands us one contiguous result either way.

### Blocking vs busy-poll vs bypass
```
blocking recvfrom   syscall + copy + THREAD SLEEPS, then a context switch to wake
busy-poll           syscall + copy. never sleeps.
bypass              neither
```
The sleep-and-wake is microseconds *and* highly variable — the worst thing for tail latency. HFT
always busy-polls: burn a core spinning so the thread is never asleep when a packet lands.

### `udp_receiver.cpp` — three lifetimes
```
ctor        socket() -> setsockopt(SO_RCVBUF) -> bind()    setup, ONCE
~dtor       close(fd_)                                      RAII, ONCE
receive()   recvfrom() -> return {buffer_, n}               hot path, PER PACKET
```
- **`socket()`** — asks the kernel for an endpoint object + receive queue, returns an fd. NOT a
  network interface (that's the NIC). `AF_INET` = IPv4, `SOCK_DGRAM` = UDP.
- **`setsockopt(SO_RCVBUF)`** — grows the **kernel's** queue. Prevents DROPS during bursts; does
  nothing for latency. The kernel silently clamps it (macOS: `kern.ipc.maxsockbuf`), so
  `getsockopt` it back or you don't know your real queue depth.
- **`bind()`** — "give me packets on this port." Skip it and you receive nothing, silently.
  `htons(port)` is mandatory: ports are network byte order (big-endian).
- **`receive()`** — does **not** decode. Returns bytes; `app/main.cpp` calls `decode_packet`.
  Keeping codec out of transport is what makes the bypass swap a one-file change.

**Two buffers, often confused:**
| | `m_buffer` | `SO_RCVBUF` |
|---|---|---|
| whose memory | **ours** (user) | **kernel's** |
| have a pointer to it? | yes | **no** — we only set a size |
| holds | **one** datagram | a **queue** of many |
| in `recvfrom` | destination | source |
| sized by | largest single datagram (65507) | tolerable backlog |
| too small → | **truncation** (got the packet, lost its tail) | **drops** (never saw it) |

**`recvfrom`'s 6 params:**
```
recvfrom(fd_,          // 1 which socket's queue
         buffer_,      // 2 destination
         BUFFER_SIZE,  // 3 CAPACITY of that destination (not expected size)
         0,            // 4 MSG_* flags bitmask. 0 = no modifiers = BLOCKING.
         nullptr,      // 5 out-param for sender's address — we don't care
         nullptr);     // 6 pairs with 5
```
Returns only an `ssize_t` — the byte count. The data arrived as a side effect.
```
n > 0    bytes copied -> return {buffer_, n}
n == 0   valid EMPTY datagram, not EOF
n == -1  error, reason in errno:
           EAGAIN/EWOULDBLOCK -> nothing available (normal with MSG_DONTWAIT)
           EINTR              -> signal interrupted, retry
           anything else      -> socket broken, BAIL (returning {} spins the loop at 100%)
```

⚠ **Check `n < 0` BEFORE casting to `size_t`.** `(size_t)-1` is 1.8e19, so the span is 16 exabytes
and the first read is instant UB.

⚠ **Param 4 takes `MSG_*`, not `IPPROTO_*`.** `IPPROTO_UDP` belongs to `socket()`; it's the number
17, so passing it here silently sets two arbitrary MSG bits. Compiles fine — both params are plain
`int`. Exactly the mix-up strong types prevent. `MSG_DONTWAIT` is **128 on macOS, 64 on Linux** —
always use the constant.

⚠ **Our flat array is not a ring buffer.** One 64 KB array, one datagram, overwritten every read.
A ring earns its place only later: receive and decode on separate threads (SPSC ring), or bypass.

### Syscalls
A privilege transition, ~100 ns – 1 µs, made worse by Spectre/Meltdown mitigations. Plus the copy.
In a receive loop this is usually the **single dominant cost**, bigger than all decoding combined.

Three consequences:
| | |
|---|---|
| venues batch | ~40 messages per datagram = one syscall amortised over 40 |
| `recvmmsg` | several *datagrams* per syscall. **Does not exist on macOS** (only `recvmsg`). |
| kernel bypass | no syscall, no copy, no kernel |

### UDP is message-oriented
One `recvfrom` = exactly one complete datagram. Never partial, never two. So no accumulation
logic and no ring buffer needed.

⚠ TCP is a byte **stream** with no message boundaries — one `read` can give you half a message or
three and a half. The order-entry side will need accumulation.

### Kernel vs userspace
Two memories with a **hardware-enforced** wall: privilege rings plus page-table permissions. Your
process **cannot** touch kernel memory — not "shouldn't", *cannot*; you get a segfault. That wall
is the basis of process isolation.

So there are **two copies** on the receive path: NIC → kernel (DMA), then kernel → us (`recvfrom`).

### ASCII
`'A'`=65, `'B'`=66, `'S'`=83. A character literal **is** its code — that's why `case 'B':` compares
against 66, and why `static_cast<char>` on a byte works.

### The three HFT tricks are all the same idea
| | the standard way | what we do | what changed |
|---|---|---|---|
| **kernel bypass** | syscall + copy through the stack | NIC DMAs into our memory, we poll | removed a layer of indirection |
| **ring buffer** | allocate per message | fixed slots, reused | removed allocation |
| **flat map** | buckets of linked nodes | one array, index arithmetic | removed pointer chasing |

Each one trades generality for **knowing something specific about our data**: that packets are
fixed-size and transient, that order refs are sequential, that prices are dense ticks. The
general-purpose version can't assume any of that, so it pays for flexibility we don't need.

**The question to ask, then, isn't "what's the fast data structure?" — it's "what do I know about
my data that `std::unordered_map` isn't allowed to assume?"**
