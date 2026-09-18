# Building And Testing

From the repository root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The build-tree library aliases and their dependency are:

```text
fexma::record_tract
        ^
        | PUBLIC
        |
    fexma::wal
```

Core-only consumers link `fexma::record_tract`. Consumers requiring
Persistence, physical WAL, Reader, or Recovery link `fexma::wal` and receive
Core transitively:

```cmake
target_link_libraries(core_consumer PRIVATE fexma::record_tract)
target_link_libraries(wal_consumer PRIVATE fexma::wal)
```

These are build-tree aliases. This repository does not currently provide
install rules, package exports, or `find_package` configuration.

The compiled library targets enable the repository's maximum standard warning
set: `/W4` and `/permissive-` on MSVC, or `-Wall -Wextra -Wpedantic` elsewhere.

The contract executables cover configuration, warmed aligned storage, hot-path
allocations, FIFO and wrap-around, durability batching, physical file format,
CRC and padding, injected append/sync failures, concurrent producer,
durability-writer, and consumer roles, and ordinary slider ordering, frontier,
failure, and upstream behavior. The bare-pipeline executable covers
linear reclamation, stopped-stage backpressure, retained-slot stability,
wraparound, and concurrent producer/slider operation. The reader and recovery
executables cover validated scanning, corruption classification, conservative
incomplete-tail truncation, physical synchronization, and refusal without
mutation. The persistence-slider executable covers bounded batches, sync-before
publication, downstream gating, append/sync failure, durable-prefix draining,
and reader/scanner compatibility.
