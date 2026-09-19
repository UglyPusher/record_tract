# record_tract

`record_tract` is a C++20 library for a single-publisher, multi-consumer ordered
record tract. Its core is a bounded `RecordTape` with owner-specific progress
boundaries and statically composed `Slider<Predecessor, Module>` processing
stages. Physical WAL persistence is an optional specialized stage built on the
same tract.
Low-level serialization helpers are exposed under `<fexma/binary/...>`.

The available build-tree CMake targets are:

- `fexma::record_tract`
- `fexma::wal`
- `fexma::binary`

`fexma::record_tract` contains the Core implementation and public/header-only
Core facilities. `fexma::wal` contains Persistence, physical WAL, Reader, and
Recovery and has a `PUBLIC` dependency on `fexma::record_tract`; existing WAL
consumers therefore retain transitive access to Core. These are build-tree
aliases; the repository does not currently define install or package-export
rules.

```cmake
target_link_libraries(core_consumer PRIVATE fexma::record_tract)
target_link_libraries(wal_consumer PRIVATE fexma::wal)
```

The source tree follows the same ownership boundary:

```text
include/fexma/record_tract/  Core public headers
include/fexma/wal/           WAL, Reader, and Recovery public headers
src/record_tract/            Core implementation
src/wal/                     WAL implementation and private physical headers
tests/core/                  Core-only tests
tests/wal/                   WAL, Reader, and Recovery tests
tests/binary/                Standalone binary-helper tests
```

## Core Record Tract

`RecordTape` owns only:

```text
tail <= retained positions < head
head - tail <= capacity
```

`RecordTape` provides single-producer publication, absolute-position immutable
views, bounded reclamation, and position exhaustion handling. It contains no
persistence frontier, file writer, or persistence failure state.

A frontier is an owner-specific monotonic exclusive progress boundary. A value
of `N` certifies that positions `[0, N)` have completed the work represented by
that owner. `RecordTape::GetFrontier()` returns its published boundary and is
equivalent to `head()`. Each Slider owns its processed boundary and exposes it
through `Slider::GetFrontier()`. There is no standalone public frontier object.

`Slider<Predecessor, Module>` provides synchronous stage mechanics over
`RecordTape`. The separately supplied `RecordTape` provides record data through
`try_view(position)`. The predecessor provides the permitted exclusive end
through `GetFrontier()`. The Slider processes permitted records in position
order through one statically bound module and publishes completed progress to
its own frontier. It owns no worker, polling loop, wait strategy, runtime
topology, or domain semantics.

`ExecutionPolicy::read_count` is the internal read-pass size;
`process_available()` still drains the complete range visible in its single
predecessor-frontier observation unless processing fails. With the current
zero-copy `try_view(position)` path, read-pass boundaries do not change
externally observable successful behavior. They are retained for read mechanics
that may later require bounded materialization. `publish_count` is the number of
successfully processed records between frontier publications. Final successful
progress and a successful prefix before failure are flushed before return. If
either policy field is zero, the entire policy is normalized to the canonical
`ExecutionPolicy{}` value `{1, 1}`.

The root and downstream construction forms are:

```cpp
RecordTape tape;
NoOpModule first_module;
NoOpModule second_module;

Slider first(tape, first_module);         // predecessor is tape
Slider second(tape, first, second_module); // predecessor is first
```

`RecordTape` can be the root predecessor because its `GetFrontier()` is its
published `head`. A downstream predecessor need not be a Slider; it need only
provide the structurally required `GetFrontier()` operation. In every case the
record data still comes from `tape`.

The first bare composition is:

```text
producer -> RecordTape::GetFrontier() -> Slider<NoOpModule>
                                           |
                                           v
                                Slider::GetFrontier() -> reclaimer -> tail
```

More generally, multiple stages may use the tape or another structurally
compatible owner as their predecessor. Stage topology, execution policy, and
reclamation policy belong to the composition rather than to `RecordTape` or
`Slider`.

`NoOpModule` is the trivial always-successful module used to prove the minimal
tract composition.

RecordTape public value types live in
`<fexma/record_tract/record_tape_types.hpp>`; physical WAL format and lifecycle
types live in `<fexma/wal/types.hpp>`. RecordTape headers do not include the
physical WAL types. The current RecordTape `default_alignment` and physical
`wal_default_alignment` are both 64 but are independent defaults, not a shared
contract.

## Optional WAL Persistence

Persistence is a specialized tract stage, not an intrinsic property of
`RecordTape`. `Persistence<Predecessor>` owns its durability mechanics and
terminal failure state for the current open writer lifetime. Its lifecycle is
independent from the tape; a later successful `open()` starts a new writer
lifetime and clears the previous failure state.

`Persistence<Predecessor>` obtains its permitted boundary from its predecessor
and owns a frontier representing durable progress. Unlike the generic
per-record Slider, it appends a bounded batch, performs one OS-level physical
sync, and publishes the durable frontier only after the complete batch has
synchronized. `Persistence::GetFrontier()` is the exclusive end of the records
whose required durability operation has succeeded.

The default `PersistencePolicy::sync_count` is one. It is the maximum number of
records appended and synchronized as one durability batch by a single
`process_available()` call; zero is normalized to the default of one.

One persistence composition can therefore be wired as:

```text
producer -> RecordTape::GetFrontier() -> Persistence<RecordTape>
                                              |
                                              v
                                   durable GetFrontier() -> downstream stage
```

For that specific composition:

```text
tail <= durable <= head
head - tail <= capacity
```

Here `durable` is the semantic name of the persistence stage's frontier, not a
third boundary intrinsically owned by `RecordTape`. The composition reclaims
positions only after every mandatory stage that protects retention has completed
them and all borrowed views have been retired.

`open()` creates only a new WAL file and never truncates an existing path. The
physical file uses canonical little-endian headers and aligned record offsets.
Its immutable identity binds one stream kind and ID to one epoch, manifest, and
payload schema; runtime ring capacity is intentionally not persisted.

`WalReader` opens an existing file only when its complete persisted identity
matches the caller's expected configuration. It exposes a record only after
validating physical headers, sequence, CRCs, payload, and zero padding.
`scan_wal()` reports the longest trusted prefix without modifying the file.

`recover_incomplete_tail()` is the separate, conservative mutation boundary.
With exclusive ownership of a quiescent file, it may truncate only a physically
incomplete trailing record to the scanner-proven trusted offset, synchronize
that truncation, and validate the complete file again. Complete invalid records,
middle corruption, sequence errors, and identity mismatches are refused without
mutation.

After crash, the maximal contiguous fully validated prefix is the authoritative
WAL: every complete record in it participates in replay and rebuild regardless
of client acknowledgement. Batches are live append-and-sync units only. The
file contains no batch commit records or commit markers.

Start with [CONTRACT.md](doc/CONTRACT.md), then see
[DESIGN.md](doc/DESIGN.md) and [INVARIANTS.md](doc/INVARIANTS.md).
The physical WAL layout is in [FILE_FORMAT.md](doc/FILE_FORMAT.md); build and
test commands are in [BUILDING.md](doc/BUILDING.md).

## Build And Test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Extracted from UglyPusher/ll_exp at commit 63681e8.
