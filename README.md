# record_tract

`record_tract` is a C++20 library for a single-publisher, multi-consumer ordered
record tract. Its core is a bounded `RecordTape` with explicit `Frontier`
progress and statically composed `Slider<Module>` processing stages. Physical
WAL persistence is an optional specialized stage built on the same tract.
Low-level serialization helpers are exposed under `<fexma/binary/...>`.

The exported CMake targets are:

- `fexma::wal`
- `fexma::binary`

## Core Record Tract

`RecordTape` owns only:

```text
tail <= retained positions < head
head - tail <= capacity
```

`RecordTape` provides single-producer publication, absolute-position immutable
views, bounded reclamation, and position exhaustion handling. It contains no
persistence frontier, file writer, or persistence failure state.

A `Frontier` is a monotonic exclusive progress boundary. `Frontier N` certifies
that positions `[0, N)` have completed the work represented by that frontier.
Each processing stage publishes its own frontier.

`Slider<Module>` provides synchronous stage mechanics over `RecordTape`. It
reads either `RecordTape::head()` or an explicit upstream `Frontier`, processes
available records in position order through one statically bound module, and
publishes its own `Frontier` after each successful record. It owns no worker,
polling loop, wait strategy, runtime topology, or domain semantics.

The first bare composition is:

```text
producer -> RecordTape::head -> Slider<NoOpModule> -> Frontier -> reclaimer -> tail
```

More generally, multiple stages may consume directly from `head` or depend on
explicit upstream frontiers. Stage topology, execution policy, and reclamation
policy belong to the composition rather than to `RecordTape` or `Slider`.

`NoOpModule` is the trivial always-successful module used to prove the minimal
tract composition.

RecordTape public value types live in `record_tape_types.hpp`; physical WAL
format and lifecycle types live in `types.hpp`. RecordTape headers do not
include the physical WAL types. The current RecordTape `default_alignment` and
physical `wal_default_alignment` are both 64 but are independent defaults, not
a shared contract.

## Optional WAL Persistence

Persistence is a specialized tract stage, not an intrinsic property of
`RecordTape`. `PersistenceModule` owns live append/sync and its terminal failure
state for the current open writer lifetime. Its lifecycle is independent from
the tape; a later successful `open()` starts a new writer lifetime and clears
the previous failure state.

`PersistenceSlider` binds persistence processing to `RecordTape::head()` and a
frontier representing durable progress. Unlike the generic per-record slider,
it appends a bounded batch, performs one OS-level physical sync, and publishes
the durable frontier only after the complete batch has synchronized.

One persistence composition can therefore be wired as:

```text
producer -> head -> PersistenceSlider -> durable Frontier -> downstream stage
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
