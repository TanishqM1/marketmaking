# OS & Machine Architecture

FAQ. Syscalls, memory, cache, registers, the network stack.

---

### Q: what is a buffer and why do we need one?

A plain array in our program that received bytes get copied into. We need one because you can't
read data "out of the air" — it has to live somewhere addressable before we can parse it.

- One fixed array, allocated once at startup, **overwritten on every read**. Not a queue, not
  growing — scratch space.
- Size it 64KB (max UDP payload). One allocation, costs nothing, makes truncation impossible.
- Undersize it and `recvfrom` silently **truncates** the datagram and you lose the tail.
  (`MSG_TRUNC` detects it. Or just don't undersize it.)

**Example:** `alignas(64) std::byte buf[65536];` — declared once, reused for every packet.

**Gotcha:** anything pointing into the buffer is dead the moment the next `recvfrom` runs. So:
decode fully → apply to book → *then* read again. Never hold a decoded pointer across a read.

---

### Q: why don't we need a ring buffer or "wait for more bytes" logic?

Because UDP is **message-oriented**. One `recvfrom` = exactly one complete datagram. Never a
partial one, never one and a half.

So there's no accumulation to do. Read → decode everything in it → read again.

**Gotcha:** TCP is the opposite — a byte **stream** with no message boundaries. A single `read`
can give you half a message, or three and a half. The order-entry side *will* need accumulation
logic. Different problem.

---

### Q: what is `recvfrom`?

A syscall that copies one waiting datagram out of the kernel and into your buffer.

```c
ssize_t n = recvfrom(fd, buf, buflen, flags, &src_addr, &addrlen);
```

Returns the byte count — that's the `len` the parse loop uses. `src_addr` tells you who sent it.

- `recv` — same thing, without the sender's address.
- `read` — also works, but can't tell you the source.
- `man 2 recvfrom`

---

### Q: what is kernel memory vs a userspace array?

Two separate memories with a **hardware-enforced wall** between them.

- **Userspace** — your process's own memory. A normal array you declared.
- **Kernel memory** — the OS's memory, including a **receive queue per socket** where arriving
  packets wait until you ask for them.

Your program **cannot** touch kernel memory. Not "shouldn't" — *cannot*. The CPU enforces it
with privilege levels (ring 3 = you, ring 0 = kernel) plus page-table permission bits. Trying
gets you a segfault.

**Why the wall exists:** without it, any process could read any other process's memory, all
network traffic, passwords in flight. It's the basis of process isolation.

---

### Q: what actually happens when a packet arrives?

```
1. NIC receives the ethernet frame
2. NIC writes it into KERNEL memory via DMA
3. kernel network stack: IP checks, UDP checksum, work out which socket
4. kernel queues it on your socket's receive queue   <- sits here waiting
5. you call recvfrom()
      -> CPU switches to kernel mode
      -> kernel memcpy's the bytes into YOUR buffer
      -> CPU switches back to user mode
6. your buffer now holds the datagram
```

Steps 1–4 happen whether you're ready or not. Step 5 is you going to fetch it.

So there are **two copies**: NIC → kernel, then kernel → you.

---

### Q: why do syscalls matter so much for latency?

A syscall is not a function call. It's a **privilege transition** — roughly 100ns to 1µs, made
worse by the Spectre/Meltdown mitigations. Then a memcpy on top.

In a receive loop this is usually the **single dominant cost**, bigger than all the decoding put
together.

Three consequences:

| | |
|---|---|
| venues batch messages | ~40 messages per datagram = one syscall amortised over 40, not 40 syscalls |
| `recvmmsg` | several *datagrams* per syscall. Real optimisation, later. |
| kernel bypass | DPDK / Solarflare ef_vi: NIC DMAs straight into memory you registered in userspace. No syscall, no copy, no kernel. Milestone 6. |

This is what "count your syscalls" in AGENTS.md means.

---

### Q: if I store a parsed field in a variable, does that cost memory?

No. It goes in a **CPU register**, not RAM. No address, no stack slot — it lives in a register
for a few nanoseconds and then the register gets reused.

So these compile to **the same instruction** (one load from the buffer into a register):

```cpp
use(read_be32(buf + off));          // "reading directly"
uint32_t shares = read_be32(buf + off);  use(shares);   // "into a variable"
```

There is no extra copy in the second one. Reading fields into locals **is** reading directly
from the buffer.

**Gotcha:** what *does* cost is materialising a whole **struct** in memory, filling every field,
and passing it around — that's a real copy, may spill to the stack, and touches fields you never
read.

| | cost |
|---|---|
| field → local → use it | one load. this is the fast path. |
| build full message struct, pass it around | real copy, stack traffic, unused fields |

---

### Q: are the loads from the buffer slow?

No — the buffer line is already in **L1 cache**. You just received it, and you walk it
sequentially so the prefetcher has it. ~4 cycles per load.

The expensive memory access in the feed handler is elsewhere: the **order-ref map** lookup.
Millions of entries, random access, so essentially a guaranteed cache miss — on the order of
100ns, roughly 25x the cost of a buffer load.

**Gotcha:** this is why micro-optimising the decode (jump table vs if-chain, struct vs view,
saving 2 bytes in the header) is the wrong place to spend effort. One cache miss dwarfs all of
it.
