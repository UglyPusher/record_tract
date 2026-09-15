# record_tract

`record_tract` is a C++20 library for an ordered record tract.

## Components

- `wal`: `RecordTape`, slider/frontier mechanics, and persistence.
- `binary`: low-level binary serialization helpers used by the library.

## CMake Targets

- `fexma::wal`
- `fexma::binary`

## Build And Test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Detailed WAL documentation is available in [`wal/README.md`](wal/README.md)
and [`wal/doc/`](wal/doc/).

Extracted from UglyPusher/ll_exp at commit 63681e8.
