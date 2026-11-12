# Assumptions

Things the code relies on but doesn't check. If one stops being true, the listed thing breaks.

| assumption | relied on by | breaks how |
|---|---|---|
| The feed only sends the 7 types we decode: A, F, E, C, X, D, U | `decode_packet` `default:` ([codec/itch.cpp:261](../codec/itch.cpp)) | any other type (S, R, P, Q, …) never advances `offset` → the hot loop spins forever on that byte |
| Add orders always have shares > 0 | the bitmap in [state/book.hpp](../state/book.hpp) `add` | a qty-0 add sets a level's bit with nothing behind it → it can become best with qty 0, and the bit only clears if that ref is later removed |
