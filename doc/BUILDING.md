# Building And Testing

## Standalone default

From the repository root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The standalone defaults build Core, WAL, tests, and `example_basic_tract`.

The project-specific options are:

| Option | Top-level default | Subproject default |
| --- | --- | --- |
| `RECORD_TRACT_BUILD_WAL` | `ON` | `OFF` |
| `RECORD_TRACT_BUILD_EXAMPLES` | `ON` | `OFF` |
| `RECORD_TRACT_BUILD_TESTS` | `ON` | `OFF` |

## Core-only standalone

```sh
cmake -S . -B build-core \
  -DRECORD_TRACT_BUILD_WAL=OFF \
  -DRECORD_TRACT_BUILD_EXAMPLES=OFF \
  -DRECORD_TRACT_BUILD_TESTS=OFF

cmake --build build-core
```

This configuration creates `fexma::record_tract` without WAL, repository test,
or example targets.

## Disabling tests through CTest

When `RECORD_TRACT_BUILD_TESTS` is enabled, the project uses the standard CTest
`BUILD_TESTING` option. To configure the repository without test executables:

```sh
cmake -S . -B build \
  -DBUILD_TESTING=OFF
```

`RECORD_TRACT_BUILD_TESTS` controls whether this project participates in test
configuration. It does not overwrite a parent project's `BUILD_TESTING` value.

## FetchContent

As a subproject, `record_tract` defaults to Core only. A consumer can use:

```cmake
include(FetchContent)

FetchContent_Declare(
  record_tract
  GIT_REPOSITORY https://github.com/UglyPusher/record_tract.git
  GIT_TAG <pinned-commit-or-tag>
)

FetchContent_MakeAvailable(record_tract)

target_link_libraries(consumer PRIVATE fexma::record_tract)
```

The dependency must be pinned to a release tag or commit. WAL, repository tests,
and examples are not created by default when the project is brought in through
`FetchContent`.

To opt in to WAL, set the option before `FetchContent_MakeAvailable()`:

```cmake
set(RECORD_TRACT_BUILD_WAL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(record_tract)

target_link_libraries(consumer PRIVATE fexma::wal)
```

Tests and examples remain disabled unless explicitly enabled through
`RECORD_TRACT_BUILD_TESTS` and `RECORD_TRACT_BUILD_EXAMPLES`.

## Targets

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

Tests are grouped by ownership: `tests/core/` links only
`fexma::record_tract`, `tests/wal/` links `fexma::wal`, and `tests/binary/`
exercises the standalone binary helpers.

The contract executables cover configuration, warmed aligned storage, hot-path
allocations, FIFO and wrap-around, durability batching, physical file format,
CRC and padding, injected append/sync failures, concurrent producer,
durability-writer, and consumer roles, and ordinary slider ordering, frontier,
failure, and upstream behavior. The bare-pipeline executable covers
linear reclamation, stopped-stage backpressure, retained-slot stability,
wraparound, and concurrent producer/slider operation. The reader and recovery
executables cover validated scanning, corruption classification, conservative
incomplete-tail truncation, physical synchronization, and refusal without
mutation. The persistence executable covers bounded batches, sync-before
publication, downstream gating, append/sync failure, durable-prefix draining,
and reader/scanner compatibility.
