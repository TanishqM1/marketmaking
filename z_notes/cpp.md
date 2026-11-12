# C++

Language features first, then inlining, then building.

---

## Language

### `static` — three meanings
| where | means |
|---|---|
| in a class | `static constexpr int x = 0;` — belongs to the **type**, not the object. **Zero bytes** per object; the value is baked into the instructions. `CAPACITY` / `MASK` / `TICK_BITS` cost nothing. |
| at file scope | private to that file |
| inside a function | persists between calls |

A real member instead (`std::size_t capacity_ = 65536;`) costs 8 bytes in every object for a value
that never changes.

### `std::span`
Non-owning view over memory someone else owns. Just `{pointer, size}`.
- Two words, passed in registers. Zero cost vs passing ptr + len separately.
- Carries the size → bounds checks possible, and ptr/len can't drift apart.
- `.subspan(offset)` / `.first(n)` to advance. They **return a new span** — discarding the result
  is a silent no-op.
- `std::span<const std::byte>` for a receive buffer: `const` because we never write it, `std::byte`
  because it's raw bytes (not text like `char`, not numbers like `uint8_t`).

| expression | type | is |
|---|---|---|
| `&packet` | `span*` | address of the **span object**. Wrong. |
| `packet.data()` | `const std::byte*` | the pointer inside the span ✓ |
| `&packet[11]` | `const std::byte*` | pointer to byte 11 |
| `packet[11]` | `const std::byte` | the byte itself |

⚠ Gives **bounds, not lifetime**. A span over the receive buffer dies on the next `recvfrom`.

⚠ A raw `const std::byte*` carries no length — `read_be64(ptr)` can legally read 8 bytes or 2,000.
So bounds checks live at the span layer:
```
decode_packet(span)   knows the extent, checks it   <- protection HERE
  read_be64(ptr)      just an address, trusts caller
```

### Registers vs memory — when does a variable or struct actually exist in memory?
C++ has **no concept of registers**; placement is the compiler's choice. Small locals whose address
doesn't escape live in registers — almost all decode code. A small local struct gets inlined, then
**SROA** splits it into scalars: no address, no layout, no padding.

It materialises in memory only when:
- it **outlives the function** — container, member, `static`, heap ← the one that matters
- its **address escapes** to code the compiler can't see (other TU without LTO, function pointer)
- it's an **array** indexed at runtime
- it's **not inlined and >16 bytes** — arm64 returns bigger structs via a hidden pointer
- register pressure (arm64 has 31 GP registers, x86-64 ~14), `volatile`, `-O0`

**Why: the as-if rule** — the compiler preserves observable *behaviour*, not the operations you wrote.
| written | compiles to |
|---|---|
| `uint64_t raw; memcpy(&raw, p, 8);` | one `ldr`, no stack slot |
| `struct Price { int64_t v; };` | identical code to bare `int64_t` |
| returning a 7-field struct | 7 values in registers |

⚠ A stack spill is **not** a cache miss — the stack is always in L1 (~4 cycles). The access that
hurts is the order-map lookup: random, DRAM, ~200 cycles.

**Check, don't guess:** Compiler Explorer at `-O2`; `[sp, #...]` in the asm = a stack access.

The structs that **do** live in memory here get largest-first ordering and a minimised `sizeof`:
the order-map entry and the price ladders.

### Struct layout: `alignof`, `sizeof`, padding, bit-fields
```
alignof(struct) = the largest alignof among its members
each member     = placed at an offset that's a multiple of its own alignment
sizeof(struct)  = rounded up to a multiple of alignof(struct)
```
Why: hardware loads a value in one access when it's naturally aligned, and `sizeof` rounds up so
`arr[1]` is aligned too. **Declare largest members first** — a `uint64_t` after a `uint8_t` inserts
7 bytes of padding.

**Bit-fields** — `std::uint32_t abs_tick : 26;` means *26 bits*, not bytes. Consecutive bit-fields
share one storage unit:
```
OrderEntry:  ref (8) | remaining (4) | abs_tick:26 side:1 occupied:1 (+4 unused bits) = 16 B
```
- Same machine code as hand-written masks (`and`, `ubfx` extract, `bfi` insert) — checked.
- ⚠ An out-of-range assignment is **silently truncated** (`abs_tick = 2²⁶` stores 0) → prove the
  range with a `static_assert`.
- ⚠ Adding `uint8_t pad[2]` after them makes the struct **24**, not 16: the bit-field word is full,
  so `pad` starts a new byte at offset 16 and the size rounds up to 8.
- Bit order is compiler-chosen — fine in memory, **never** `memcpy` one onto the wire. No pointers
  to bit-field members.

Lock layouts with `static_assert(sizeof(OrderEntry) == 16)` — a wrong size becomes a build failure.

### `auto`
Deduces the type from the initializer; still fully static typing.

| use | don't |
|---|---|
| verbose types: `auto it = map.find(ref);` | numeric literals — `auto offset = 0;` is an **`int`** → sign-compare warning against `.size()`. Use `std::size_t` |
| **preventing narrowing**: `int n = packet.size();` narrows; `auto n =` is exactly `size_t` | strong types — `Price p = read_price(msg)` documents and enforces; `auto p =` throws that away |

### `size_t` vs fixed-width
| use | for |
|---|---|
| `std::size_t` | sizes, counts, indices, memory offsets |
| `std::uint64_t` / `32` / `16` | **externally-defined** widths: wire and protocol fields |

`order_ref` is `uint64_t` (ITCH says 8 bytes, forever), not `size_t` — on a 32-bit target
`size_t` is 4 and `memcpy` reads half the field.

⚠ **Same width, different types:** here `size_t` is `unsigned long`, `uint64_t` is `unsigned long
long`. Overload resolution treats them as different. Pick by **meaning**, not width.

### Enums: a cast relabels, it doesn't translate
```cpp
enum class Side : std::uint8_t { Buy, Sell };   // Buy = 0, Sell = 1
static_cast<Side>(packet[AddOrder::side]);      // 'B' is 66 -> Side(66): legal, nameless
```
With a fixed underlying type, all 256 values are valid — enumerator names label *some* of them. So
`Side(66) == Side::Buy` is false on **every** message: every order lands on the ask side, the book
has no bids, and nothing warns. Translate explicitly instead:
```cpp
mm::Side side = mm::Side::Buy;      // defined start value — no uninitialized read
if (temp_side == 'S') { side = mm::Side::Sell; }
```
Compiles to `cmp` + `cset` (no branch). Casting works for `MsgType` only because its enumerators
*are* the wire chars (`AddOrder = 'A'`). See [decisions.md](decisions.md) — why we don't validate.

**`switch` on an enum:** with no `default:`, `-Wswitch` (in `-Wall`) warns when a named enumerator
has no `case`. A `default:` silences that. A `switch` on a plain `char` never warns.

**ASCII:** `'A'` = 65, `'B'` = 66, `'S'` = 83. A character literal *is* its code.

### Linkage: anonymous namespace
`namespace { }` → everything inside is private to that TU (internal linkage), absent from the
symbol table. The compiler knows every call site (inlines aggressively), nothing outside can call a
raw decoder and skip the bounds check, no cross-TU collisions. `static` at file scope does the same.

- ⚠ **`.cpp` only.** In a header, each including TU gets its own private copy.
- ⚠ Can't be declared in a header (two candidates → "call is ambiguous"), so tests can't reach it.
  That's why our decoders live in `namespace detail` instead — tests call them directly.
- ⚠ `using namespace itch;` ≠ `namespace itch { }` — the first only affects lookup; definitions
  stay at global scope.

Check: `nm -gU build/itch.o | c++filt`.

### `[[likely]]` / `[[unlikely]]`
Tell the compiler which branch dominates so it **lays the code out** that way: the hot path becomes
the fall-through (no taken jump) and the cold body moves to the end of the function, off the hot
path's I-cache lines.
```cpp
if (return_code <= 0) [[unlikely]] { return {}; }
return {m_buffer, static_cast<std::size_t>(return_code)};
```
- ⚠ **Not a branch-predictor hint** — the hardware predictor learns at runtime. These are static
  layout. The test itself still runs every time.
- Annotate only when you know (error paths, bounds failures, gap recovery). PGO supersedes them.

### `noexcept`, `explicit`, `const`
| | means | here |
|---|---|---|
| `noexcept` | never throws. Without it the caller emits unwinding machinery and the optimiser can't reorder across the call (across a TU it must *assume* a throw). Throwing anyway → `std::terminate` | all book/map handlers, `receive()`. The constructor *may* throw (startup) |
| `explicit` | blocks implicit conversion into the class. Without it `UDP_Receiver r = 8080;` compiles — and opens a socket | every single-argument constructor. Only for constructors / conversion operators |
| `const` member fn | "doesn't modify the object" — writes are compile errors | `best_bid()` / `best_ask()`. A `const Book&` handed to the strategy can read the touch but never move a cursor |

`const` is for correctness, **not speed**. `const` ≠ `constexpr` (computable at compile time).

---

## Inlining

### What it does
Pastes the callee's machine code at the call site. Removes the call overhead and — more importantly
— the **optimisation barrier** a call creates.

`read_be64(packet, AddOrder::order_ref)` where `order_ref` is `constexpr 11`:
```
NOT inlined                          INLINED
  mov  w1, #0xb     <- pass 11         ldur x8, [x0, #0xb]   <- 11 is an immediate
  b    read_be64                       rev  x0, x8
    ldr x8, [x0, x1] <- reg offset      ret
    rev x0, x8
    ret
5 instructions, 2 branches           3 instructions, 0 branches
```
The offset folds into the addressing mode — *that's* what makes `constexpr` offsets pay off.
Calling a small function from another is free once inlined.

### The compiler can only inline code it can see
Inlining needs the **body** in the caller's TU. `#include` pastes the header in, so a body in the
`.hpp` is visible everywhere; a body in a `.cpp` is a promise → a real call.

| code | goes in | why |
|---|---|---|
| hot (book handlers, order map, scans) — every message | `.hpp`, inside the class | inlined: no call, fields stay in registers |
| cold (setup, error reporting) | `.cpp` | call cost irrelevant; keeps headers and I-cache lean |

⚠ **The `inline` keyword doesn't request inlining.** It's an ODR exemption letting a definition
appear in many TUs (i.e. live in a header). Member functions defined inside a class are implicitly
`inline`. The optimiser decides separately.

⚠ Visible ≠ inlined. `[[gnu::always_inline]]` forces, `[[gnu::noinline]]` / `[[gnu::cold]]`
prevents — use only after measuring. LTO also inlines across `.cpp`s, but only in builds with the
flag, so firms keep hot code header-only regardless ⚠️.

### Inlining is a cache budget
| | |
|---|---|
| **always inline** | leaf functions smaller than their own call sequence (`read_be16` = 1 load + 1 `rev`). 2 instructions × 6 call sites = 12, vs a function + 6 calls ≈ 30 — the binary gets *smaller* |
| **verify** | big bodies, e.g. whole decoders in one `switch`. Inlined copies grow the hot loop; if it stops fitting L1i you pay misses on the exact path you sped up |

```sh
clang++ -O2 -Rpass=inline -Rpass-missed=inline -c codec/itch.cpp
```
Shows what was inlined and what was **declined**, with reasons. Release only — `-O0` inlines
nothing. In a disassembly, `bl <name>` = a real call. See [hardware.md](hardware.md) — code layout.

---

## Build & toolchain

### TU (translation unit)
One `.cpp` plus everything it `#include`s — compiled **completely alone**. A **TU boundary** is where
the compiler loses visibility. Which is why headers exist, why a `.cpp` body can't be inlined into
another TU, why `inline` exists, and why an anonymous namespace is per-TU.

Our loop pays for two boundaries per packet — `receiver.receive()` (`libmm_transport.a`) and
`decode_packet()` (`libmm_codec.a`) are opaque calls, and `main` re-tests `packet.empty()` because
it can't see that `receive()` already decided.

### LTO (Link Time Optimization)
Each `.o` stores a half-compiled form; the optimiser sees all of them at **link** time and can
inline across files. Doesn't weaken layering — sources, headers and the CMake graph are untouched.
```cmake
set_property(TARGET trading_system mm_transport mm_codec
             PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
```
Release only (slow builds, harder debugging). Verify: `ninja -C build-release -t commands | grep flto`.

### `-DNDEBUG`
Makes `assert()` compile to nothing. Overriding `CMAKE_CXX_FLAGS_RELEASE` drops CMake's default
`-DNDEBUG` — put it back, or every assert stays live in the build you measure latency in.

### Builds
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug             # -O0 -g + sanitizers
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release   # -O2 -g -march=native -DNDEBUG
cmake --build build --target test_book test_order_map test_codec  # tests are EXCLUDE_FROM_ALL
ctest --test-dir build --output-on-failure
```
- **Latency numbers only from Release.**
- ⚠ Test targets are `EXCLUDE_FROM_ALL` — build them **by name** before `ctest`, or it runs stale
  binaries and "passes".
- macOS: LeakSanitizer is unsupported on arm64 (`leaks --atExit -- ./binary` instead). ASan runs can
  hang here — use `-DMM_SANITIZE=OFF` / UBSan, or `ASAN_OPTIONS=symbolize=0`.
