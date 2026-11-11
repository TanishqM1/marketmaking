# Build & Toolchain

### TU (translation unit)
One `.cpp` plus everything it `#include`s — the chunk the compiler compiles at a time,
**completely alone**. Building `main.cpp`, it has never seen `udp_receiver.cpp`.

A **"TU boundary"** is the line between two separately-compiled files, and it's where the compiler
loses visibility. Which is why:
- headers exist (declarations so one TU can reference another's functions)
- a function defined in a `.cpp` **cannot be inlined** into a different TU
- `inline` is needed for a header definition (it may then legally appear in many TUs)
- an anonymous namespace makes something private to one TU

Our loop pays for a boundary twice per packet:
```cpp
auto packet = receiver.receive();    // libmm_transport.a -> opaque call
if (packet.empty()) continue;        // re-tests what receive() already decided
mm::itch::decode_packet(packet);     // libmm_codec.a -> opaque call
```

### Q: cpp compiler can only inline code it can see (code/logic in hpp files!)
Yes. Inlining needs the **body** in the caller's TU. `#include` pastes the header's text in, so a
body in the `.hpp` is visible everywhere; a body in a `.cpp` is a declaration-only promise → real call.

| code | goes in | why |
|---|---|---|
| hot (book handlers, order map, scans) — runs every message | `.hpp`, inside the class | inlined: no jump/return/register save, fields stay in registers |
| cold (recentering, setup, error reporting) | `.cpp` | call cost irrelevant; keeps headers and I-cache lean |

**Example:** `decode_packet` (itch.cpp) → `book.add(...)`. Body in book.cpp = a call per message
(few ns × millions/s). Body in book.hpp = pasted into the switch case.

**Gotcha:** visible ≠ inlined — the compiler still decides. Check the asm (`bl <name>` = a real
call); force with `[[gnu::always_inline]]` only after measuring. LTO (below) also inlines across
`.cpp`s, but only in builds with the flag, so firms keep hot code header-only regardless.
> ⚠️ verify: "firms keep hot code header-only" — common practice, not confirmed for a specific firm.

### LTO (Link Time Optimization)
The fix. Each `.o` stores a half-compiled intermediate form instead of finished machine code, and
the optimiser gets all of them at **link** time — so it can inline across files.
```cmake
include(CheckIPOSupported)
check_ipo_supported(RESULT ok)
set_property(TARGET trading_system mm_transport mm_codec
             PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
```
Release only (`_RELEASE` suffix) — it makes builds much slower and debugging harder. Verify with
`ninja -C build-release -t commands | grep flto`.

Does **not** weaken layering: LTO merges at code generation, so source files, headers and the CMake
dependency graph are untouched.

### `-DNDEBUG`
What makes `assert()` compile to nothing. CMake's default Release is `-O3 -DNDEBUG`; overriding
`CMAKE_CXX_FLAGS_RELEASE` means you must put `NDEBUG` back yourself, or every assert stays live in
the build you measure latency in.

### Builds
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug     # -O0 -g + sanitizers
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release   # -O2 -g -march=native -DNDEBUG
cmake --build build
ctest --test-dir build --output-on-failure
```
Separate directories so both coexist. **Latency numbers only from Release.**

macOS: LeakSanitizer is unsupported on arm64 — use `leaks --atExit -- ./binary`. If an ASan run
hangs, try `ASAN_OPTIONS=symbolize=0`.
