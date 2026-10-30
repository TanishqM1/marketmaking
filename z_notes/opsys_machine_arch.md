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
