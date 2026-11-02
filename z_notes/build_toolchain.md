# Build & Toolchain

### Q: what is a TU?

**TU = translation unit** = one `.cpp` plus everything it `#include`s. It's the chunk the compiler
compiles at a time, **completely alone** — when building `main.cpp` it has never seen
`udp_receiver.cpp`.

```
udp_receiver.cpp  ->  udp_receiver.cpp.o    compiled in isolation
main.cpp          ->  main.cpp.o            compiled in isolation
                           |
                      linker joins them
```

A **"TU boundary"** is the line between two separately-compiled files. It's where the compiler
loses visibility, which is why:
- headers exist at all (declarations so one TU can reference another's functions)
- a function defined in a `.cpp` **cannot be inlined** into a different TU
- `inline` is needed for a definition in a header (it may then appear in many TUs legally)
- an anonymous namespace makes something private to one TU

**Example:** our loop pays for a TU boundary twice per packet —
```cpp
auto packet = receiver.receive();    // in libmm_transport.a -> opaque call
if (packet.empty()) continue;        // re-tests what receive() already decided
mm::itch::decode_packet(packet);     // in libmm_codec.a -> opaque call
```
`receive()` already returned an empty span for the bad cases, but `main` can't see that, so the
same test runs twice.

**Gotcha:** **LTO (Link Time Optimization)** is the fix. Each `.o` stores a half-compiled
intermediate form instead of finished machine code, and the optimiser gets handed all of them at
**link** time — so it can finally inline across files. CMake:
```cmake
set_property(TARGET trading_system mm_transport mm_codec
             PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)   # Release only
```
Release only: it makes builds much slower and debugging harder. It does **not** weaken our
layering — LTO merges at code-generation time, so the source files, headers and CMake dependency
graph are untouched.
