# Record Tract Design

## Core Components

`RecordTape` owns runtime lifecycle, bounded storage, `head`, `tail`, producer
publication, reclamation, position exhaustion, and borrowed read-only position
access. It has no filesystem path, physical writer, durable frontier, or I/O
failure state.

The tract is single-publisher, multi-consumer at the record-flow level: one
producer publishes the ordered record stream, while multiple independently
coordinated processing stages or readers may consume retained records. This is
not a built-in broadcast runtime. The library does not register consumers,
dispatch records to them, schedule them, or automatically derive reclamation
from their progress; those relationships belong to the composition.

Progress boundaries are owned by the components whose work they describe.
`RecordTape::GetFrontier()` exposes the exclusive published boundary and is
equivalent to `head()`. Each Slider owns an exclusive processed boundary and
exposes it through `Slider::GetFrontier()`. There is no standalone public
frontier object shared by these owners.

`Slider<Predecessor, Module>` is a header-only template. It holds separate
references to a `RecordTape` record source and a predecessor. The predecessor
structurally supplies the permitted exclusive end through `GetFrontier()`; the
tape supplies records through `try_view(position)`. The Slider processes the
range visible in one predecessor observation and publishes completed progress
to its internally owned frontier. The caller owns repeated execution and all
waiting or scheduling.

`ExecutionPolicy::read_count` partitions that traversal into internal read
passes. It does not bound the total work of `process_available()`: absent
failure, the call drains the complete range from its one predecessor-frontier
observation. Because the current tape path borrows one zero-copy `RecordView` at
a time, the pass size has no externally observable effect on successful
processing. The boundary remains part of the mechanics for read paths that may
later need bounded materialization, without specifying such a path now.

`ExecutionPolicy::publish_count` controls release-publication cadence for the
Slider-owned frontier. Complete publication batches are published during the
call; final successful residual progress and a successful residual prefix before
failure are flushed before return. A zero in either policy field invalidates the
value object and normalizes the whole policy to `{1, 1}`.

A module owns processing semantics. The generic slider requires only synchronous
`process(const RecordView&)` success or failure; it does not know what the
module computes or publishes outside its own progress frontier.

The composition owns stage topology, execution policy, and reclamation policy.
A root stage uses `RecordTape` as predecessor; a downstream stage may use
another Slider or another structurally compatible frontier owner. The library
does not provide a runtime stage registry or prescribe a fixed number of
stages.

The first concrete static composition uses `NoOpModule`:

```text
producer -> RecordTape::GetFrontier() -> Slider<NoOpModule>
                                           |
                                           v
                                Slider::GetFrontier() -> reclaimer -> tail
```

The Slider publishes its owned frontier; after the synchronous Slider call
retires all borrowed views, the composition reads `slider.GetFrontier()` and
advances `tail`.
These are distinct operations. If the composition does not run the slider or
does not reclaim, the bounded producer eventually observes `Full`.

`RecordTape::Storage` owns one aligned allocation. It implements exactly four
lifecycle and addressing responsibilities:

```text
initialize/allocate -> warm/touch -> block_at_slot(slot) -> release
```

It does not implement frontier policy or persistence.

`RecordTape` keeps absolute `tail` and `head` boundaries. The producer keeps a
cached head slot for its sequential hot path. Absolute view access derives the
slot only after validating the position. Reclamation publishes an exclusive
absolute boundary and needs no slot cursor.

`try_view(position)` validates the absolute position against `tail` and `head`
before mapping it with `position % capacity`. It returns position and a const
span over the existing payload block. There is no second data store or
per-reader payload copy. It may expose retained records regardless of whether a
persistence stage has completed them and never reclaims them.

The caller or composition owns retention coordination: no reclaimer may pass a
borrowed position during access or use. The view is not a reader registration
or a slot pin. Lifecycle operations require all views to be retired. `Slider`
enforces its upstream permission separately from this storage-access check.

## Generic Stage Walkthrough

`RecordTape::try_publish()` validates the call, uses the block at `head`, fills
it, publishes `head + 1`, and returns the published position.

A Slider snapshots `predecessor.GetFrontier()`. For each available position it
obtains the immutable `RecordView` from its separately held `RecordTape`, calls
its module, and publishes completed progress to its own frontier only after
successful module completion. A failed module call leaves that position
unpublished by the stage.

The composition decides when to invoke stages and when completed positions may
be reclaimed. Frontier publication by a stage does not itself advance
`RecordTape::tail`.

## Persistence Specialization

Persistence uses a dedicated stage because its frontier publication is
conditional on successful synchronization of a complete batch rather than
per-record module completion.

`Persistence<Predecessor>` is the dedicated bounded persistence stage. It
holds separate references to the `RecordTape` record source and its predecessor.
It obtains the permitted boundary from `predecessor.GetFrontier()`, selects a
bounded range, appends records read from the tape, performs one sync, and
publishes its owned frontier only after the complete batch succeeds. In a
persistence composition that frontier is conventionally named `durable`.

The public template delegates its non-template mechanics to
`detail::PersistenceCore`. The core owns the policy, durable progress atomic,
terminal failure state, and selected physical adapter. It is an implementation
detail inside one Persistence stage, not an independently composed stage.

`PhysicalWalAdapter` is the internal physical WAL boundary. The selected
filesystem implementation owns the native OS file handle, creates the file,
writes the file header, serializes physical records with CRC and padding,
synchronizes when requested, and closes the handle. It has no knowledge of
tract frontiers, predecessor topology, or batch-selection policy.

`src/wal/physical_wal_adapter.hpp` is the single compile-time selection point.
The current build selects `PhysicalWalAdapter` from the private
`src/wal/physical_wal_file.hpp` header and compiles
`src/wal/physical_wal_file.cpp`. These headers are not part of the public
include tree. `detail::PersistenceCore` uses that concrete type directly; there
is no CRTP, runtime registry, virtual dispatch, or runtime backend selection.

`WalReader` is the cold-path validated sequential reader. Its selected
`PhysicalWalReaderAdapter` performs only hardware-specific byte reads;
`WalReader` owns canonical decoding, identity checks, CRC validation, sequence
validation, and fail-closed state. `scan_wal()` drives the same reader to report
the longest trusted prefix and never mutates storage.

`recover_incomplete_tail()` is a separate cold-path policy layer. It uses
`scan_wal()` as the sole source of the trusted truncation offset and calls the
compile-time selected `PhysicalWalRecoveryAdapter` only for a scanner-proven
incomplete trailing record. The adapter performs the hardware-specific
truncate-and-sync operation. Recovery then scans the complete retained file
again; it never repairs, skips, or resynchronizes around corruption.

### Persistence Walkthrough

The composition sets the persistence stage's `sync_count` and invokes
`process_available()`. The stage reads its permitted boundary from
`predecessor.GetFrontier()`; a zero `sync_count` is normalized to the default
count of one. It obtains each selected `RecordView` from its separately supplied
`RecordTape`, appends the batch, requests one sync, and publishes the range end
as `durable` only after success. Physical WAL sequence derives from the stored
`first_sequence` and the tape position.

## Frontier Layout

`RecordTape` stores each intrinsic boundary in a 64-byte `TapeBoundary`.
`TapeBoundary` relies on `alignas` plus `alignof`/`sizeof` static assertions; it
does not use manual padding. A Slider separately owns an `alignas(64)` atomic
processed boundary. These are owner-specific implementations, not instances of
a common public frontier class.

The layout isolates independently written boundaries and stage frontiers from
one another to avoid false sharing. It does not remove legitimate cache traffic:
a producer acquires `tail`, stages acquire their upstream boundaries, and a
reclaimer observes the frontier or frontiers required by its composition.

The fixed 64-byte assumption is explicit and may need a portability layer on
platforms with a different destructive-interference size.

## Physical Durability

The physical writer uses native unbuffered application calls rather than C++
iostream buffering:

- Windows: `WriteFile` followed by `FlushFileBuffers`;
- POSIX: `write` followed by `fdatasync`;
- macOS: `write` followed by `fsync`.

One non-empty logical batch receives one physical sync. The durable frontier is
a publication of that completed sync, not of append completion alone.

File creation is exclusive (`CREATE_NEW` on Windows, `O_CREAT | O_EXCL` on
POSIX). The live writer never truncates an existing file. Explicit recovery
opens the quiescent file separately and truncates only to a scanner-proven
record boundary.

Filesystem recovery uses `SetEndOfFile` followed by `FlushFileBuffers` on
Windows, `ftruncate` followed by `fdatasync` on POSIX, and `ftruncate` followed
by `fsync` on macOS. The caller provides exclusive file ownership across the
initial scan, mutation, and verification scan, preventing a concurrent writer
from invalidating the trusted offset.

Physical headers are serialized field-by-field in canonical little-endian byte
order. Header CRCs are computed over those serialized bytes with the
corresponding header CRC field set to zero. The immutable file header identifies
one stream kind and ID, one epoch, one manifest, one first sequence, and one
application-owned payload schema. Runtime ring capacity is not persisted.
Stream, epoch, manifest, and schema metadata apply to every payload in the file
and are not repeated in records. Zero padding fills the gap between the
canonical file header and the first record so every record starts at an aligned
offset.

RecordTape public value types are isolated in
`include/fexma/record_tract/record_tape_types.hpp`. Physical format and
lifecycle types are isolated in `include/fexma/wal/types.hpp`; RecordTape does
not include or depend on that header. `default_alignment` configures tape
storage, while `wal_default_alignment` configures the physical layout. Their
current equal value does not couple the contracts.

## Test Boundary

The physical writer contains a narrow test control for failing a selected
record append or sync and counting calls. It is private to the component and is
not reachable through the public WAL API. Producer and consumer paths do not
consult it. The filesystem recovery adapter has an equivalent private control
for truncate and sync failure-path tests.
