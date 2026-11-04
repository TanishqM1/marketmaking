# Bad Habits

- **No `new` / `delete` / `std::make_unique` on the hot path.** Dynamic allocation is slow.
  Pre-allocate everything at startup.

- **No `throw` / `try-catch` on the hot path.** Exceptions bloat code and force stack unwinding.
  Use error codes, status enums, or `std::expected` (return `std::unexpected`, check the result).
  Startup is the exception — a failed bind *should* throw.

- **No `std::list` / `std::map` / `std::unordered_map`.** Storage is scattered across the heap, so
  every access is a pointer chase. Use `std::vector` or flat arrays — contiguous.

- **No rarely-taken code inlined into the hot path** (error handling, logging, gap recovery). It
  runs almost never but occupies L1i footprint forever, evicting code that does run. Mark it
  `[[gnu::cold]]` (which propagates — it marks every branch reaching it unlikely) and
  `[[gnu::noinline]]`, with `[[unlikely]]` on the branch. Write error paths as separate functions
  from the start; retrofitting once the bail logic is tangled into the loop is painful.

- **Never format a log string on the hot path.** `snprintf` is microseconds and a lot of code.
  Increment a counter or push a fixed-size binary record to a ring buffer; format elsewhere.

- **No float prices, ever.** Scaled integers or tick counts. Floats break equality (`0.1+0.2 !=
  0.3`), and equality is the whole job of a book.

- **No bare typedefs for domain types.** `using Price = int64_t` gives zero safety — it's an alias,
  so `Price` and `Qty` are the same type and transposing them compiles silently. Use a one-field
  struct: distinct type, identical codegen.
