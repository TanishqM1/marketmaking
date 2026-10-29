- we should not use `new`, `delete`, `std::make_unique` because dynamically requesting memory is slow. we should pre-allocate it ALL at runtime.

- we should NOT use `throw` or `try-catch` blocks, because thrown exceptions generate too much bloat code, and force the CPU to disassemble the stack.
Instead, we use error codes, status enums, or `std::expected` (we can return std::unexpected, and then check if(return_var)).

- We should not use `std::list` or `std::map` because the variable storage in memory is scattered across the heap. We should use `std::vector` or flat arrays, 
because they are stored contigously.

