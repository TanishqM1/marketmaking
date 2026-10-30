# TODO

Deferred work. Each entry: what, why it was deferred, how to check it.

---

## Verify the codec's hot functions actually get inlined

**File:** `codec/itch.cpp` — `read_be16/32/64` and the per-message decoders.

**Why deferred:** no profile yet, and the codec doesn't work end to end. Forcing inlining now
would be guessing.

**How to check:**
```sh
clang++ -std=c++23 -O2 -Rpass=inline -Rpass-missed=inline -c codec/itch.cpp
```
Prints what got inlined and what the compiler **declined** to inline, with reasons. Release
build only — `-O0` inlines nothing.

**What to expect:**

- `read_be16/32/64` — should always be inlined, no exceptions. Each is one load + one `bswap`,
  smaller than the call sequence needed to invoke it. If these show up in `-Rpass-missed`,
  something is wrong (probably the definition isn't visible to the caller's TU).
- The six per-message decoders — **probably** should be, but don't assume. Six fully-inlined
  bodies in one `switch` bloats `decode_packet`. If the hot loop stops fitting in L1i, inlining
  them all is a net loss. Measure before forcing.

**If the compiler declined something that matters:** `[[gnu::always_inline]]`. Surgically, on the
specific function, after the profile — not sprayed around.

**Related:** if the book builder in `state/` ever calls `read_be*`, they can't be inlined from
there while the definitions live in `itch.cpp`. Two fixes: move them to the header as `inline`,
or turn on `-flto` in Release.
