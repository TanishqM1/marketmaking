`static` in cpp can have three meanings.
1. `static constexpr int msg_type = 0;` means the member belongs to the type itself, not the object.
2. `static int counter = 0;` is file scope, meaning the variable is private to the file itself.
3. inside a function, static means it persists between calls of the function and we can continue to use it.
