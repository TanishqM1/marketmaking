# Assumptions

Things the code relies on but doesn't check. If one stops being true, the listed thing breaks.

| assumption | relied on by | breaks how |
|---|---|---|
| The feed only sends the 7 types we decode: A, F, E, C, X, D, U | `decode_packet` `default:` ([codec/itch.cpp:261](../codec/itch.cpp)) | any other type (S, R, P, Q, …) never advances `offset` → the hot loop spins forever on that byte |
