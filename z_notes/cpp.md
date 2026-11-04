# C++

### `static` — three meanings
| where | means |
|---|---|
| in a class | `static constexpr int x = 0;` — belongs to the **type**, not the object. No per-object storage. |
| at file scope | private to that file |
| inside a function | persists between calls |

### `std::span`
Non-owning view over memory someone else owns. Just `{pointer, size}`.

- Two words, passed in registers. Zero cost vs passing ptr+len separately.
- Carries the size → bounds checks possible, and ptr/len can't drift apart.
- `.subspan(offset)` / `.first(n)` to advance through a buffer.
- `std::span<const std::byte>` for a receive buffer: `const` because we never write it, `std::byte`
  because it's raw bytes (not text like `char`, not numbers like `uint8_t`).

⚠ Gives **bounds, not lifetime**. A span over the receive buffer dies the moment the next
`recvfrom` overwrites it.

### Getting a pointer out of a span
| expression | type | is |
|---|---|---|
| `&packet` | `span*` | address of the **span object**. Wrong. |
| `packet.data()` | `const std::byte*` | the pointer inside the span ✓ |
| `&packet[11]` | `const std::byte*` | pointer to byte 11 |
| `packet[11]` | `const std::byte` | the byte itself |

Need a byte → `packet[i]`. Need to hand a raw pointer to a function → `.data()`.

`&packet` feels right because raw arrays *decay* to pointers; a span is a class, so no decay — ask
it with `.data()`. Same as `vector::data()`.

⚠ A `const std::byte*` is **just an address, carrying no length.** The type can't tell "one byte"
from "first of 144", which is why `read_be64(ptr)` can legally consume 8 — or 2000. That's why
bounds checks live at the span layer:
```
decode_packet(span)   knows the extent, checks it   <- protection HERE
  read_be64(ptr)      just an address, trusts caller
```
`.data()` throws the safety away. Fine, but only because you checked first.

### Memory vs registers
C++ has **no concept of registers** — allocation is a codegen decision. (`register` keyword was
removed in C++17.)

**Always memory:** globals/`static`, heap, arrays indexed at runtime, `volatile`, anything whose
address **escapes** to a function the compiler can't see into, objects >~16 bytes.

**Register (compiler's choice, and it picks register):** small locals whose address doesn't escape
— i.e. almost all decode code. Pushed back to memory by register pressure (arm64 has 31 GP
registers, x86-64 ~14) or `-O0`.

Taking an address isn't enough to force memory:
```cpp
memcpy(&y, &x, 8);     // used locally + inlined -> stays in a register
some_opaque_fn(&x);    // escapes -> must exist in memory
```

⚠ A stack spill is **not** a cache miss — the stack is always-L1.
```
register  ~0 cycles      L2   ~12
stack     ~4             L3   ~40
L1        ~4             DRAM ~200-300   <- the only one worth engineering around
```
The access that hurts is the order-ref map lookup: random, millions of entries, DRAM.

**Check, don't guess:** Compiler Explorer at `-O2`, grep asm for `[sp, #...]` = a stack access.

### When a struct exists in memory (by default it doesn't)
A small local struct gets inlined, then **SROA** splits it into scalars in registers — no address,
no layout, no padding. It materialises only when the compiler can't prove that's safe:

- **outlives the function** — container, member, `static`, heap ← the one that matters
- **address escapes** — passed to a function it can't see into (other TU without LTO, function
  pointer, virtual call)
- **it's an array** indexed at runtime
- **not inlined AND >16 bytes** — arm64 returns bigger composites via a hidden pointer to caller
  stack (≤16 bytes comes back in `x0`/`x1`)
- register pressure, `volatile`, `-O0`

**Why: the as-if rule** — the compiler preserves observable *behaviour*, not the operations you
wrote. Same rule behind:
| written | compiles to |
|---|---|
| `uint64_t raw; memcpy(&raw, p, 8);` | one `ldr`, no stack slot |
| `struct Price { int64_t v; };` | identical code to bare `int64_t` |
| returning a 7-field struct | 7 values in registers |

The two structs in this project that **will** be in memory, and so get largest-first ordering and
minimised `sizeof`: the order-ref map entry (millions, all day) and the price level (array,
runtime-indexed).

### `auto`
Deduces the type from the initializer. Still fully static typing.

> `auto` when the RHS makes the type obvious or the type is verbose.
> Spell it out for numeric widths and strong types. Never `auto x = 0` for an index.

| use | don't |
|---|---|
| verbose types: `auto it = map.find(ref);` | numeric literals — `auto offset = 0;` is an **`int`**, then `offset < packet.size()` is a sign-compare warning. Use `std::size_t` or `0uz`. |
| **preventing narrowing**: `int n = packet.size();` silently narrows; `auto n =` is exactly `size_t` | strong types — `Price p = read_price(msg)` documents AND enforces; `auto p =` throws that away |

### `size_t` vs fixed-width
| use | for |
|---|---|
| `std::size_t` | sizes, counts, indices, memory offsets — magnitude scales with the address space |
| `std::uint64_t` / `32` / `16` | **externally-defined** widths: wire fields, protocol fields |

```cpp
AddOrder::order_ref = 11        // size_t   — index into memory
packet.size(), offset           // size_t
order_ref, timestamp            // uint64_t — ITCH says 8 bytes, every machine, forever
shares, price                   // uint32_t
stock_locate                    // uint16_t
```
An order ref is **not a size**. `size_t order_ref` works here only by accident; on a 32-bit target
`sizeof(size_t)` is 4 and `memcpy` reads half the field.

Not `uint64_t` everywhere because `sizeof`, `.size()`, `subspan()` and indexing all speak `size_t`
— anything else converts at every boundary.

⚠ **Same width, different types** (verified here): `size_t` is `unsigned long`, `uint64_t` is
`unsigned long long`. Not interchangeable for overload resolution — you can get "no matching
function" between two 64-bit unsigned ints. Pick by **meaning**, not width.

### Casting an int to an enum doesn't translate it
It **relabels the bits**; the number rides through unchanged. Enum names label *some* values, not
the only legal ones — any other number is a valid, nameless value of that type.

```cpp
enum class Side : std::uint8_t { Buy, Sell };   // Buy = 0, Sell = 1
static_cast<Side>(packet[AddOrder::side]);      // 'B' is 66 -> Side(66), legal and nameless
// side == Side::Buy  ->  66 == 0  ->  false, every message
```
```
cast:    66 -> 66     relabels
switch:  66 ->  0     translates   <- the switch IS the conversion
```
It worked for `MsgType` only because its enumerators *are* the wire chars (`AddOrder = 'A'` = 65).

⚠ No validation anywhere. `Side(66)` doesn't default, doesn't warn, doesn't crash. Downstream
`if (side == Buy) bids[...] else asks[...]` is false for **every** message — all orders land on the
ask side and the book has **no bids**. Silent. Hence `default:` must be loud.

### Anonymous namespace / internal linkage
`namespace { }` with no name → everything inside is private to that TU, never exported.

| linkage | |
|---|---|
| external (default) | symbol exported; other `.cpp`s can call it |
| internal | private to this TU; absent from the object file's symbol table |

**What it buys:** the compiler knows every call site (inlines aggressively, can delete the
out-of-line copy entirely); nothing outside can call a raw decoder and skip the bounds check in
`decode_packet`; no cross-TU collisions, smaller binary, faster link.

`static` at file scope does the same, but `static` already means three other things — and
anonymous namespaces work for types too.

Confirm: `nm -gU build/itch.o | c++filt` — anything in the anonymous namespace shouldn't appear.

⚠ **`.cpp` only, never a header.** Each including TU gets its own private copy with distinct
symbols. State would silently be per-TU.

⚠ **Can't be declared in a header.** Declaring `itch::read_be16` there while defining it in the
anonymous namespace gives two candidates → `call to 'read_be16' is ambiguous`. So it's unusable
from tests; if tests need it, make it `inline` in the header instead.

⚠ **`using namespace itch;` ≠ `namespace itch { }`.** The first only affects *lookup*; your
definitions stay at global scope and the header's declarations have no definition → ambiguous call
or undefined reference.

### What inlining actually does
Pastes the machine code at the call site. Removes the call overhead and — more importantly — the
**optimisation barrier** a call creates.

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
Three things removed: argument marshalling, the branches, and **the barrier** — the compiler
couldn't fold the offset into the addressing mode through an opaque call. `[x0, x1]` becomes
`[x0, #0xb]`. *That's what makes `constexpr` offsets pay off*, and it compounds across six field
reads per message.

⚠ **Not about stack overhead.** No `sub sp` in either version — `read_be64` is a *leaf* function
(calls nothing) so it never builds a frame. The "every call pushes a frame" model comes from `-O0`.

⚠ **"Costs cache space" is inverted for small functions.** 2 instructions × 6 call sites = 12,
versus a 3-instruction function + 6 call sequences ≈ 30. A function smaller than its own call
sequence makes the binary *smaller*. I-cache cost only applies to large bodies.

⚠ **The `inline` keyword does not request inlining.** It's an ODR exemption letting a definition
appear in multiple TUs (i.e. live in a header). The optimiser decides separately. Putting the
definition in the header is what *enables* it, by making the body visible.

### `[[likely]]` / `[[unlikely]]`
C++20. Tell the compiler which branch dominates so it lays the code out accordingly.
```cpp
if (return_code <= 0) [[unlikely]] { return {}; }   // cold body moves out of line
return {m_buffer, static_cast<std::size_t>(return_code)};   // fall-through
```
Switch form: put the attribute after the `case` label.

Two wins: the hot path becomes the **fall-through** (a taken branch costs a pipeline redirect, a
fall-through is free), and the cold body stops sharing I-cache lines with hot code.

⚠ **Not a branch-predictor hint.** The hardware predictor learns at runtime and is very good.
These affect *static layout at compile time*. Common misconception.

⚠ Only annotate when you actually know — a wrong guess pessimises the hot path. Good: error
branches, bounds-check failures, gap recovery.

⚠ PGO supersedes these; it uses measured frequencies.

### `noexcept`
A promise the function never throws. If a function *might* throw, the caller emits a landing pad
(stack-unwinding machinery) and the optimiser can't reorder across the call — and across a TU
boundary the compiler must **assume** it might. `noexcept` removes that.

If a `noexcept` function does throw → `std::terminate`, instantly, no unwinding.

Right split here: **constructor throws** (startup, a failed bind should be loud), **`receive()` is
`noexcept`** (hot path, never throws).

### `explicit`
Blocks implicit conversion from the argument type to the class type.
```cpp
UDP_Receiver r = 8080;    // compiles without `explicit`!
f(8080);                  // ditto, if f takes a UDP_Receiver
```
Matters most when the constructor has side effects — `UDP_Receiver` opens a socket and binds a
port, and you don't want that happening because a number was passed where an object was expected.
Default to `explicit` on every single-argument constructor.
