- we should not use `new`, `delete`, `std::make_unique` because dynamically requesting memory is slow. we should pre-allocate it ALL at runtime.

- we should NOT use `throw` or `try-catch` blocks, because thrown exceptions generate too much bloat code, and force the CPU to disassemble the stack.
Instead, we use error codes, status enums, or `std::expected` (we can return std::unexpected, and then check if(return_var)).

- We should not use `std::list` or `std::map` because the variable storage in memory is scattered across the heap. We should use `std::vector` or flat arrays, 
because they are stored contigously.

- We should not let rarely-taken code (error handling, logging, gap recovery) get inlined into the hot path, because it runs
almost never but takes up L1i instruction cache footprint in the hot loop forever, evicting the code that does run. Mark it
`[[gnu::noinline]]` and `[[gnu::cold]]`, and use `[[unlikely]]` on the branch so the compiler lays the cold side out-of-line.
Write error paths as separate functions from the start — it costs nothing and is annoying to retrofit once the bail logic is
tangled into the loop.

