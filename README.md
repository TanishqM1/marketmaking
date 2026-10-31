# marketmaking

Market-making system and mock exchange. ITCH market data in, SBE order entry out.

## Build

From the project root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

After the first configure, `cmake --build build` is enough — it re-runs CMake
automatically if a `CMakeLists.txt` changed.

```sh
rm -rf build        # guaranteed-clean slate
```

## Test

```sh
ctest --test-dir build --output-on-failure
```

Or run the binary directly, which is faster for iterating:

```sh
./build/test/test_codec
```

## Release build

Separate directory, so Debug and Release can coexist:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

| | flags | use for |
|---|---|---|
| Debug | `-O0 -g` + ASan/UBSan | tests, debugging |
| Release | `-O2 -g -march=native` | **any latency measurement** |

A latency number from a Debug build is meaningless. Measure in Release only.

Sanitizers are on by default in Debug; disable with `-DMM_SANITIZE=OFF`. Note
LeakSanitizer is unsupported on arm64 macOS — use `leaks --atExit -- ./binary`
for leaks, and if an ASan run appears to hang, try `ASAN_OPTIONS=symbolize=0`.

## Layout

```
types/        Price, Qty, Side, OrderRef — no wire knowledge, compiles alone
codec/        ITCH offsets, decoders — all inbound wire knowledge
state/        order-ref map, price levels (not started)
transport/    sockets, MoldUDP64 (not started)
session/      sequence numbers, gap detection (not started)
exchange/     mock exchange — separate binary (not started)
test/         mirrors the source tree
z_notes/      topic-organized FAQ + learning changelog
z_questions/  open questions
```

Working agreement and priorities: [AGENTS.md](AGENTS.md).
Wire format reference: [z_notes/inbound_outbound_data.md](z_notes/inbound_outbound_data.md).
