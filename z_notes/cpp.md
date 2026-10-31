`static` in cpp can have three meanings.
1. `static constexpr int msg_type = 0;` means the member belongs to the type itself, not the object.
2. `static int counter = 0;` is file scope, meaning the variable is private to the file itself.
3. inside a function, static means it persists between calls of the function and we can continue to use it.


### Q: what is std::span?

An abstraction to reference memory owned somewhere else. It's a **non-owning view** — typically
just a pointer and a size.

- Two words wide, passed in registers. Zero cost vs passing the pointer and length separately.
- Carries the size, so bounds checks are possible: `if (msg.size() < AddOrder::length) bail`.
- The pointer and length can't drift out of sync the way two separate arguments can.
- `.subspan(offset)` for advancing through a buffer.

**Example:** `std::span<const std::byte>` for a receive buffer. `const` because we never modify
it; `std::byte` because it's raw bytes, not text (`char`) or numbers (`uint8_t`).

**Gotcha:** span gives you bounds, **not lifetime**. It doesn't own the memory, so a span over
the receive buffer is dead the moment the next `recvfrom` overwrites it — same as a raw pointer.


### Q: when is something in memory vs in a register?

C++ has **no concept of registers**. The language only describes objects and their storage
duration — register allocation is a codegen decision. (The `register` keyword never meant
anything binding; removed in C++17.)

**Always real memory:**
- globals, `static` — lifetime outlives the function
- heap (`new`, container contents)
- arrays indexed at runtime — can't index into a register
- `volatile`
- anything whose address **escapes** to a function the compiler can't see into
- big objects — over ~16 bytes on arm64/x86-64

**Compiler's choice, and it picks register:** small locals (scalars, small structs) whose address
doesn't escape. That's almost all decode code. Two things push them back to memory:
- **register pressure** — x86-64 has ~14 usable GP registers, arm64 has 31. More live values than
  that and some get **spilled** to the stack.
- **`-O0`** — spills nearly everything. Never judge codegen at `-O0`.

**Example:** taking an address is not enough to force memory.
```cpp
uint64_t x = 5;
memcpy(&y, &x, 8);     // address used locally + inlined -> stays in a register
some_opaque_fn(&x);    // address escapes -> must exist in memory
```

**Gotcha:** a stack spill is **not** a cache miss. The stack is the hottest memory you have —
basically always L1.
```
register     ~0 cycles
stack (L1)   ~4
L2           ~12
L3           ~40
DRAM         ~200-300     <- the only one worth engineering around
```
So don't fear spills. The access that hurts is the order-ref map lookup (random, millions of
entries, DRAM).

**How to check instead of guessing:** Compiler Explorer at `-O2`, grep the asm for `[sp, #...]`.
That's a stack access. No `sp` references = everything stayed in registers.


### Q: when should we use auto in our case?

`auto` = deduce the type from the initializer. Still fully static typing, just not spelled out.

**The rule:**
> `auto` when the right-hand side makes the type obvious, or the type is verbose.
> Spell it out for numeric widths and for our strong types.
> Never `auto x = 0` for an index or size.

**Use it for:**
- verbose/unspeakable types — `auto msg = packet.first(msglen);`, `auto it = map.find(ref);`
- **preventing silent narrowing** — this is the one that matters for priority #1:
  ```cpp
  int  n = packet.size();    // size_t -> int. silent narrowing, -Wconversion catches it.
  auto n = packet.size();    // exactly size_t. cannot narrow.
  ```

**Don't use it for:**
- numeric literals, because the deduced type is usually `int` and usually wrong:
  ```cpp
  auto offset = 0;           // int! then `offset < packet.size()` is a signed/unsigned warning
  std::size_t offset = 0;    // what we want
  auto offset = 0uz;         // C++23 size_t literal suffix, also fine
  ```
- our strong types — the type name at the point of use *is* the safety:
  ```cpp
  auto  p = read_price(msg);   // Price? int64_t? can't tell.
  Price p = read_price(msg);   // documented AND enforced
  ```

**Gotcha:** hiding `Price`/`Qty` behind `auto` gives back exactly the protection we created them
for. See also: [cpp.md](cpp.md) — strong types vs bare typedefs.


### Q: how do we pass memory addresses as pointers to a byte?

Given `std::span<const std::byte> packet`:

| expression | type | what it is |
|---|---|---|
| `&packet` | `std::span<...>*` | address of the **span object**. Wrong — not what you want. |
| `packet.data()` | `const std::byte*` | the pointer **inside** the span. This one. |
| `&packet[0]` | `const std::byte*` | same as `.data()` |
| `&packet[11]` | `const std::byte*` | pointer to byte 11 |
| `packet[11]` | `const std::byte` | the byte **itself** (indexing dereferences) |

**Rule:** need a byte → `packet[i]`. Need to hand a raw pointer to a function → `packet.data()`
or `&packet[i]`. Only use `.data()` to escape to something that wants a C-style pointer.

**Example:**
```cpp
read_be16(packet.data(), MessageHeader::stock_locate);   // reader takes ptr + offset
char type = static_cast<char>(packet[MessageHeader::msg_type]);  // single byte, needs cast
```

**Why `&packet` feels right but isn't:** raw arrays *decay* to pointers automatically, so `&arr[0]`
is the habit. A span is a **class** holding a pointer and a size — no decay — so you ask it for
its pointer with `.data()`. Same as `vector::data()` / `string::c_str()`.

**Gotcha:** a `const std::byte*` is **just an address**. It carries no length. The type can't tell
"points at one byte" from "points at the first of 144" — which is why `read_be64(ptr)` can legally
consume 8 bytes, and nothing would stop it consuming 2000.

That's the whole reason `span` exists, and why the bounds check lives at the span layer:
```
decode_packet(span)   <- knows the extent, checks it   <- protection lives HERE
  read_be64(ptr)      <- just an address, trusts the caller
```
Calling `.data()` throws the safety away. Fine — but only because you checked first.


### Q: what is an anonymous namespace and what does internal linkage get me?

A `namespace { }` with no name. Everything inside gets **internal linkage** — the symbol is
private to that translation unit and never exported.

(**TU / translation unit** = one `.cpp` plus everything it `#include`s, after preprocessing. The
unit the compiler compiles at a time.)

```cpp
namespace itch
{
namespace          // <- no name
{
    void decode_add_order(const std::byte* msg) { ... }
}
}
```

| | |
|---|---|
| **external linkage** (default for functions) | symbol exported; other `.cpp` files can call it |
| **internal linkage** | symbol private to this TU; not in the object file's symbol table |

**What internal linkage buys:**
- **Compiler knows every call site** → can inline aggressively, and once all call sites are
  inlined it can delete the out-of-line copy entirely. With external linkage it must emit the
  body regardless, in case another TU calls it.
- **Nothing outside can call it** → no way for other code to call a raw decoder and skip the
  bounds check in `decode_packet`.
- No cross-TU name collisions, smaller binary, faster link.

`static` at file scope does the same thing, but `static` already means three other things (see
the `static` entry at the top of this file), and anonymous namespaces also work for *types*. Use
the namespace.

**Example:** confirm it with `nm -gU build/itch.o | c++filt` — anything in the anonymous
namespace should **not** appear in that list.

**Gotcha 1 — `.cpp` files only, never a header.** Each TU that includes the header gets its own
private copy of everything inside, with distinct symbols. Six includes = six copies. Anything
holding state would silently be per-TU.

**Gotcha 2 — it cannot be declared in a header.** Declaring `itch::read_be16` in the header while
defining it in the anonymous namespace gives two candidates at the call site:
`call to 'read_be16' is ambiguous`. So: anonymous namespace means unusable from tests. If tests
need it, put it in the header as `inline` instead.

**Gotcha 3 — `using namespace itch;` is NOT the same as `namespace itch { }`.** The first only
affects name *lookup*; it does not put your definitions into the namespace. Definitions stay at
global scope, and the header's declarations end up with no definition → ambiguous call, or
undefined reference at link.


### Q: how do [[likely]] and [[unlikely]] work and why do they help?

C++20 attributes telling the compiler which branch is taken most of the time, so it can lay the
code out accordingly.

```cpp
void process_data(int value) {
    if (value > 0) [[likely]] {
        // high-probability primary path
    } else [[unlikely]] {
        // rare error / edge case
    }
}
```

```cpp
enum class Status { Success, Timeout, Disconnected, Corrupted };

void handle_status(Status s) {
    switch (s) {
        case Status::Success: [[likely]]
            // 99% of the time
            break;
        case Status::Timeout:
        case Status::Disconnected:
        case Status::Corrupted: [[unlikely]]
            // rare recovery logic
            break;
    }
}
```

**Why it helps — two things:**
- **Branch layout.** The likely path becomes the *fall-through* (no taken branch at all), and the
  unlikely path gets moved out of line. A fall-through is free; a taken branch costs a pipeline
  redirect.
- **I-cache.** The cold path stops being interleaved with hot code, so the hot cache lines stay
  dense instead of half-full of code that never runs.
  See also: [bad_habits.md](bad_habits.md) — keeping error handling out of the hot path.

**Gotcha — this is NOT a branch predictor hint.** The hardware predictor learns from actual
runtime behaviour and is very good at it. These attributes affect *static code layout at compile
time*, not the dynamic predictor. Common misconception.

**Gotcha 2 — only annotate when you actually know.** The compiler already guesses well, and if
you guess wrong you've pessimised the hot path. Good candidates: error branches, bounds-check
failures, gap recovery. Bad candidate: anything you're assuming.

**Gotcha 3 — PGO supersedes these.** Profile-guided optimisation uses measured branch frequencies,
which beat any hand annotation. If PGO is on, these are mostly redundant.


### Q: when should I use size_t? (only for memory/offset stuff?)

Yes. **`size_t` = sizes, counts, indices, offsets into memory.** Anything whose magnitude scales
with the machine's address space.

`std::size_t` is an unsigned integer guaranteed big enough to hold the size of any object. It's
what `sizeof` returns and what every container's `.size()` returns. 64-bit here, 32-bit on a
32-bit platform. It's a typedef, not a distinct type.

| use | for |
|---|---|
| `std::size_t` | sizes, counts, indices, memory offsets |
| `std::uint64_t` / `uint32_t` / `uint16_t` | values with a **fixed, externally-defined** width — wire fields, protocol fields, anything where the byte count is part of a spec |

**Example:** from our codec —
```cpp
AddOrder::order_ref = 11        // size_t   — an index into memory
packet.size(), offset, msglen   // size_t   — sizes and counts
order_ref, timestamp            // uint64_t — ITCH says 8 bytes, on every machine, forever
shares, price                   // uint32_t — ITCH says 4 bytes
stock_locate                    // uint16_t — ITCH says 2 bytes
```

An order reference is **not a size**, so `size_t order_ref` is wrong even though it happens to be
8 bytes here. On a 32-bit target `sizeof(size_t)` is 4 and a `memcpy` would silently read half
the field.

**Why not just use `uint64_t` everywhere:** interop. `sizeof`, `.size()`, `subspan()` and array
indexing all speak `size_t`. Anything else means a conversion at every boundary — exactly what
`-Wconversion` exists to flag.

**Gotcha — same width, different types.** Verified on this machine:
```
sizeof(size_t) = 8, sizeof(uint64_t) = 8
same type?  FALSE
size_t   is unsigned long
uint64_t is unsigned long long
```
So they're not interchangeable for overload resolution or template deduction — you can get "no
matching function" between two things that are both 64-bit unsigned ints. Specific to Darwin/LP64.
One more reason to pick the type by **meaning**, not by width.


### Q: why didn't casting the wire byte straight to my enum work?

Casting an integer to an enum **relabels the bits, it does not translate them** — the number
rides through unchanged. Enum names are labels on *some* values, not a list of the only allowed
values, so any other number is still a legal, nameless value of that type.

**Example:**
```cpp
enum class Side : std::uint8_t { Buy, Sell };     // Buy = 0, Sell = 1

static_cast<mm::Side>(packet[AddOrder::side]);    // 'B' is 66 -> Side(66)
// Side(66) is legal and nameless. Not Buy, not Sell.
// side == Side::Buy  ->  66 == 0  ->  false, every message.
```

The switch is the actual translation: it compares (is this 66?) then assigns a **different**
number (0).
```
cast:    66 -> 66     relabels
switch:  66 ->  0     translates
```

**Why it DID work for MsgType:** its enumerators *are* the wire chars (`AddOrder = 'A'` = 65), so
casting a 65 lands on it. A cast is only a translation when the enum's values are literally the
source encoding.

**Gotcha:** no validation anywhere. `Side(66)` doesn't default to 0, doesn't warn, doesn't crash.
Downstream `if (side == Buy) bids[...] else asks[...]` would be false for every message — every
order lands on the ask side and the book has **no bids at all**. Silent. This is why `default:`
needs to do something loud.


### Q: when does a struct actually exist in memory? (and why by default it doesn't)

By default a small local struct **never exists in memory** — it gets inlined, then SROA splits it
into individual scalars that live in registers, so there's no address, no layout and no padding.
It only materialises when the compiler can't prove that's safe.

**It IS in memory when:**
- **it outlives the function** — stored in a container, a member of a long-lived object, `static`,
  heap. ← the one that matters most
- **its address escapes** — you pass `&f` / a reference / a span to a function the compiler can't
  see into (other TU without LTO, function pointer, virtual call)
- **it's an array** indexed at runtime — can't index into registers
- **not inlined AND >16 bytes** — arm64 returns bigger composites via a hidden pointer to caller
  stack. (≤16 bytes comes back in `x0`/`x1`, no memory.)
- **register pressure** — spills to stack. Cheap though: stack is L1, ~4 cycles. Don't fear it.
- `volatile`, or `-O0` (no escape analysis runs at all)

**Why:** the **as-if rule** — the compiler must preserve observable *behaviour*, not the
operations you wrote. Your struct/call/return describe what the result should be, not how.

Same rule behind three things:
| written | compiles to |
|---|---|
| `uint64_t raw; memcpy(&raw, p, 8);` | one `ldr`, no stack slot |
| `struct Price { int64_t v; };` | identical code to a bare `int64_t` |
| returning a 7-field `AddOrderFields` | 7 values in registers, no struct |

**Example — the two structs in this project that WILL be in memory:**
```
order-ref map entry  { book_id, side, price, remaining }   millions, stored all day
price level          { price, total_qty, order_count }     array, indexed at runtime
```
Those get largest-first member order and minimised `sizeof` (cache line is 128 bytes here).
`AddOrderFields` is transient and inlined, so wire order is fine there.

**Gotcha:** the anonymous namespace helps the compiler *prove* no address escapes, because it
knows every call site. See also: [cpp.md](cpp.md) — anonymous namespaces.

**How to check:** Compiler Explorer at `-O2`, grep the asm for `[sp, #...]` — that's a stack
access. None means everything stayed in registers.


### Q: what does inlining actually do, and why is it good for small functions?

It pastes the function's machine code at the call site instead of branching to it. That removes
the call overhead **and**, more importantly, the optimisation barrier a call creates.

**Example** — `read_be64(packet, AddOrder::order_ref)` where `order_ref` is `constexpr 11`:
```
NOT inlined                        INLINED
  mov  w1, #0xb     <- pass 11       ldur x8, [x0, #0xb]   <- 11 is an immediate
  b    read_be64                     rev  x0, x8
    ldr x8, [x0, x1] <- reg offset    ret
    rev x0, x8
    ret
5 instructions, 2 branches         3 instructions, 0 branches
```

**Three things it removed:**
1. **Argument marshalling** — `11` had to become a value in `x1` because the ABI says so.
2. **The branches.**
3. **The optimisation barrier — the real win.** The compiler couldn't fold the offset into the
   load's addressing mode because it couldn't see through the call. `[x0, x1]` becomes
   `[x0, #0xb]`. *This is what makes `constexpr` offsets pay off* — a non-inlined call throws the
   benefit away. Compounds across six field reads per message.

**Gotcha — it is NOT about stack overhead.** There's no `sub sp` / `add sp` in either version.
`read_be64` is a *leaf* function (calls nothing), so it never builds a stack frame. The "every
call pushes a frame" model comes from `-O0` and from functions with enough locals to spill.

**Gotcha — "at the expense of cache space" is conditional, and inverted here.**
```
inlined:      2 instructions x 6 call sites        = 12
not inlined:  3-instruction fn + 6 call sequences  ~ 30
```
A function smaller than its own call sequence makes the binary **smaller** when inlined. The
I-cache cost only applies to large bodies. See also:
[opsys_machine_arch.md](opsys_machine_arch.md) — big functions and L1i.

**Gotcha — the `inline` keyword does not request inlining.** It's an ODR exemption so the
definition can appear in multiple TUs (i.e. live in a header). The optimiser decides the actual
inlining, independently. Putting the definition in the header is what *enables* it, by making the
body visible to callers.

