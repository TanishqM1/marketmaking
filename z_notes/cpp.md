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
