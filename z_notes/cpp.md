# C++

FAQ. The language itself — types, structs, layout, keywords.

---

### Q: what does `static` mean in C++?

Three different things depending on where it is.

1. **In a class/struct** — `static constexpr int msg_type = 0;` — the member belongs to the
   *type*, not to any object. No instance needed, no storage per object.
2. **At file scope** — `static int counter = 0;` — the variable is private to that `.cpp` file.
   Other translation units can't see it.
3. **Inside a function** — persists between calls. Initialised once, keeps its value.

---

### Q: why doesn't `using Price = int64_t` give me type safety?

Because it's an **alias**, not a new type. The compiler still sees `int64_t` for both.

```cpp
using Price = int64_t;
using Qty   = int64_t;
book.add(qty, price);   // transposed args. STILL COMPILES. silent bug.
```

You need a **distinct type** — a struct wrapping it:

```cpp
struct Price { int64_t v; };
struct Qty   { int64_t v; };
book.add(qty, price);   // compile error. caught.
```

**Gotcha:** the cost is that you must define the operators you want (`==`, `<`, `+`). Some of
that isn't boilerplate though — it's modelling. `Price * Price` is meaningless and now won't
compile. `Price * Qty` is notional, a different type again.

---

### Q: is a struct wrapping one `int64_t` slower or bigger than the raw `int64_t`?

No. `sizeof` is 8, same as the raw type, and a one-scalar trivially-copyable struct is **passed
in a register** exactly like the integer, per the ABI on both arm64 and x86-64.

This is the textbook zero-cost abstraction.

Three things that would break it:

- **Writing a copy constructor, move constructor, or destructor.** Real ABI rule: a
  non-trivially-copyable type gets passed in **memory** instead of a register. (A plain
  constructor is fine — `explicit Price(int64_t)` stays trivially copyable. Only
  copy/move/destructor matter.)
- **Any `virtual`.** Adds a vptr: 8 bytes → 16, plus indirection.
- **Comparing at `-O0`.** Unoptimised builds show extra moves. They vanish at `-O1`+.

**Example:** verify it yourself on Compiler Explorer at `-O2` — write `f(int64_t, int64_t)` and
`g(Price, Qty)` and diff the assembly. Identical. Same technique works any time you wonder
whether an abstraction costs something.

---

### Q: why is struct layout like this? (alignof / sizeof / offsets)

```
alignof(struct) = the largest alignof among its members
sizeof(struct)  = rounded up to a multiple of alignof(struct)
each member     = placed at an offset that's a multiple of its own alignment
```

For scalars `alignof == sizeof`: `char`=1, `uint16`=2, `uint32`=4, `uint64`=8, pointer=8. So in
practice the largest member sets the struct's alignment. Nested structs inherit the max.

**Why `sizeof` rounds up (trailing padding):** because of arrays. If `sizeof(X)` were 22 then
`arr[1]` starts at byte 22, and its `uint64_t` member would be misaligned. The compiler has no
choice. This holds even for a single standalone instance with nothing after it.

**Why the hardware cares:** an aligned 8-byte load never straddles a cache-line boundary. A
misaligned one can — two cache lines, a split load, slower on x86/arm64 and an outright fault on
some architectures.

---

### Q: what order should I declare struct members in?

**Largest first.** Same fields, different order, 8 bytes apart:

```
bad:   stock_locate(2) @0, [6 bytes PADDING], timestamp(8) @8,
       order_ref(8) @16, cancelled(4) @24              -> 32 bytes

good:  timestamp(8) @0, order_ref(8) @8, cancelled(4) @16,
       stock_locate(2) @20, [2 pad]                    -> 24 bytes
```

Badly ordered members get **internal** padding, which is worse than trailing padding.

**Example:** don't compute this by hand — make the compiler prove it:

```cpp
static_assert(sizeof(MyStruct) == 24);
static_assert(alignof(MyStruct) == 8);
static_assert(offsetof(MyStruct, order_ref) == 8);
```

Free, compile-time, and turns a layout assumption into an enforced one.

**Tool:** `-Wpadded` warns every time the compiler inserts padding. Far too noisy to leave on,
but worth pointing at your types once as an audit.

---

### Q: can I force a struct to be exactly 22 bytes with no padding?

Yes, with `[[gnu::packed]]`. **Don't.** Members then sit misaligned, access gets slower, and
taking a pointer to a misaligned member is UB.

It's the right tool for *describing a wire format* — which is exactly what an in-memory struct
isn't.

---

### Q: is there a 5-byte or 6-byte integer type?

No. C++ integer types are **1, 2, 4, or 8 bytes only**. There's no `uint48_t` and there never
will be.

It's a hardware thing — CPUs have load instructions for 8/16/32/64 bits. There is no "load 48
bits" instruction, so the language doesn't offer the type.

See also: [inbound_outbound_data.md](inbound_outbound_data.md) — the 6-byte ITCH timestamp, and
how to assemble it into a `uint64_t`.

---

### Q: does wire width have to match in-memory width?

No — they're **independent decisions**, and the decoder is the thing that translates.

| field | wire | memory |
|---|---|---|
| timestamp | 6 bytes | `uint64_t` (8) |
| price | 4 bytes, 4 implied decimals | `int64_t` (8) |

The wire is optimised for bandwidth by someone else. Your in-memory type is optimised for your
CPU and your arithmetic.

That's also the cleanest definition of what `codec/` is *for*: the only place that knows both
representations. Everything above it sees only our types.
