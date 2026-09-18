# Record Tract Contract

## Core Model

`record_tract` provides a single-publisher, multi-consumer ordered record tract.
The core contract is defined by `RecordTape`, owner-specific progress
boundaries, `Slider<Predecessor, Module>`, the module processing contract, and
composition-owned topology and reclamation.
Physical WAL persistence is a specialized optional stage described separately
below.

`RecordTape` is independent of persistence:

```cpp
class RecordTape {
public:
  RecordTapeOpenResult open(const RecordTapeConfig& config) noexcept;
  PublishResult try_publish(std::span<const std::byte> payload) noexcept;
  AccessResult try_view(Position position) const noexcept;
  ReclaimStatus reclaim(Position end) noexcept;
  void close() noexcept;

  Position head() const noexcept;
  Position GetFrontier() const noexcept;
  Position tail() const noexcept;
};
```

RecordTape value types are declared in `record_tape_types.hpp`; physical WAL
format and lifecycle types are declared in `types.hpp`. RecordTape headers do
not depend on physical WAL definitions. Tape `default_alignment` and physical
`wal_default_alignment` are independent defaults even though both currently
equal 64.

`RecordTape` owns bounded warmed storage and the intrinsic `head` and `tail`
boundaries. It performs no file operation and has no durable boundary or
persistence failure state. `reclaim(end)` accepts only monotonic exclusive
boundaries in `[tail, head]`; the composition is responsible for proving that
all mandatory readers have finished below `end`.

`RecordTapeConfig` contains only runtime storage fields: fixed payload size,
capacity, and allocation alignment.

## Progress Boundaries And Slider

Ordinary stage mechanics are provided by `slider.hpp`:

```cpp
RecordTape tape;
NoOpModule first_module;
NoOpModule second_module;

Slider first(tape, first_module);
Slider second(tape, first, second_module);

SliderResult first_result = first.process_available();
SliderResult second_result = second.process_available();
Position visible_downstream = second.GetFrontier();
```

Every frontier value is an exclusive end: value `N` certifies completion of
positions `[0, N)` for the owner that exposes it. `RecordTape::GetFrontier()` is
its published boundary and is equivalent to `head()`. Each Slider owns its
processed boundary and exposes acquire observation through `GetFrontier()`.
These owners do not share a public frontier type or necessarily use identical
internal mechanics.

`Slider<Predecessor, Module>` holds a const reference to its predecessor. The
predecessor structurally provides a suitable `GetFrontier()` operation; it need
not be a Slider. The separately supplied `RecordTape` remains the source of
record data. A root Slider uses the tape itself as predecessor, while a
downstream Slider may use another Slider as shown above.

`process_available()` is one synchronous call. It snapshots
`predecessor.GetFrontier()`, obtains every consecutive immutable `RecordView`
from the source tape through that exclusive end, and calls
`module.process(record)`. The module returns `true` only after processing that
position is complete. Only completed progress may be published to the Slider's
own frontier according to its current execution policy. The library does not
prescribe the number of processing stages or provide a runtime topology.

### Execution Policy

```cpp
struct ExecutionPolicy final {
  std::size_t read_count{1};
  std::size_t publish_count{1};
};
```

`read_count` is the internal read-pass size. It partitions the traversal of the
single observed predecessor range; it is not a maximum record count for
`process_available()` and is not a yield or scheduling quantum. The current
source path obtains one zero-copy borrowed `RecordView` at a time through
`RecordTape::try_view()`, so read-pass boundaries currently do not change
externally observable successful behavior. The pass structure is retained for
read mechanics that may later require bounded materialization, without defining
such a path today.

`publish_count` is the processed-frontier publication batch size. After that
many successful module calls, Slider release-publishes the next exclusive end.
A residual successful prefix is published before successful return and before
an existing module/view failure return. The failed or unavailable position is
never included.

ExecutionPolicy is normalized as one value object. If either count is zero, the
entire policy becomes the canonical `ExecutionPolicy{}` value `{1, 1}`:

```text
{8, 4} -> {8, 4}
{0, 4} -> {1, 1}
{8, 0} -> {1, 1}
{0, 0} -> {1, 1}
```

The slider owns no thread, scheduling loop, wait/spin/yield behavior, runtime
registry, virtual dispatch, neighbor type, persistence operation, or snapshot
interpretation. Calling and retry cadence belongs to the composition. A
`ViewUnavailable` result indicates a violated upstream/retention composition
contract or lifecycle transition; the slider does not reclaim `RecordTape`
storage.

## Module Contract

A generic module provides synchronous processing compatible with:

```cpp
bool process(const RecordView& record) noexcept;
```

Returning `true` means processing of that position is complete and permits the
Slider to publish the next exclusive end. Returning `false` leaves the own
frontier at the failed position so the composition may retry or stop according
to its policy.

`NoOpModule` accepts every complete `RecordView` without changing application
state. It exists as the minimal module for composition tests. In the linear
bare pipeline, the composition may call
`tape.reclaim(no_op_slider.GetFrontier())` only after the slider call has
returned and all views from the reclaimed range are retired. Publishing the
module frontier alone does not release storage or remove producer backpressure.

## Persistence Specialization

`PhysicalWalConfig` contains persisted identity and physical layout fields and
does not contain runtime capacity. Public Persistence is the independent
`Persistence<Predecessor>` tract mechanism; it is not a generic Slider paired
with a persistence module. It receives a `RecordTape` as its record source and a
predecessor as the source of its permitted boundary. A root Persistence may use
the same tape as both source and predecessor.

```cpp
struct PersistencePolicy final {
  std::size_t sync_count{1};
};

template <class Predecessor>
class Persistence {
public:
  Persistence(const RecordTape& source, const Predecessor& predecessor,
              PersistencePolicy policy = {}) noexcept;
  explicit Persistence(const RecordTape& source,
                       PersistencePolicy policy = {}) noexcept
      requires std::same_as<Predecessor, RecordTape>;

  OpenResult open(const std::filesystem::path& path,
                  const PhysicalWalConfig& config) noexcept;
  bool close() noexcept;
  bool is_open() const noexcept;
  bool failed() const noexcept;

  SliderResult process_available() noexcept;
  Position GetFrontier() const noexcept;
  Position current() const noexcept;
  void reset_quiescent(Position initial) noexcept;
};
```

The supported root and chained construction forms are:

```cpp
RecordTape tape;
Persistence root_persistence(tape, PersistencePolicy{8});

NoOpModule module;
Slider predecessor(tape, module);
Persistence chained_persistence(tape, predecessor, PersistencePolicy{8});
```

Both Persistence objects read records from `tape`. The root object also uses
the tape as its predecessor; the chained object obtains its permitted boundary
from `predecessor.GetFrontier()`.

`Persistence<Predecessor>` is separate from generic Slider processing because
it selects a bounded batch, appends every selected record, synchronizes the
complete batch once, and only then publishes its owner-held frontier. In a
persistence composition that frontier is conventionally named `durable`.
Append or sync failure leaves durable progress unchanged and is terminal for
the current open writer lifetime.

`detail::PersistenceCore` implements the non-template mechanics behind the
public template. It owns the current policy, durable progress atomic, terminal
failure state, and selected `PhysicalWalAdapter`. It is an implementation
detail, not a separately composed public stage.

`PhysicalWalAdapter` is the internal boundary to the selected concrete physical
WAL implementation. Persistence chooses the batch, requests append and sync,
publishes durable progress after success, and propagates failure. The adapter
creates and writes the physical file, appends physical records, synchronizes,
and closes its native handle.

### Physical WAL Payload And Identity

Each physical WAL instance has one non-zero `payload_size`, one file-level
`payload_schema_version`, one stream identity, one epoch, and one non-zero
`first_sequence`. Payload bytes and the meaning of the schema version are opaque
to the WAL. Schema version `0` is reserved for callers that do not declare an
application payload schema. Runtime `capacity` belongs to the in-memory tract
configuration and is not a property of the physical WAL file.

Generic infrastructure WALs may use zero stream, epoch, and manifest identities.
Command and Event WALs require non-zero `stream_id`, `epoch_id`, and
`manifest_id`. Runtime `capacity` is not part of the physical file identity.

`payload_size` is fixed for the entire file. Physical records do not carry an
individual payload length. Application schemas that encode shorter logical
values into the fixed payload are responsible for deterministic initialization
of every remaining byte.

The cold-path API in `reader.hpp` provides `WalReader` and `scan_wal()`.
`WalReader::open()` requires the expected persisted WAL configuration; runtime
capacity is ignored. `read_next()` is sequential and allocation-free after
open. It returns `Record` only after complete physical validation. Supplying an
output span whose size differs from the configured payload size returns
`ReadStatus::InvalidPayloadSize` without advancing the reader or making the
reader failed; a subsequent call with a correctly sized span addresses the same
next record.

The file must be quiescent: no writer may append, synchronize, truncate, or
replace it while a reader or scanner is active. Validation proves physical
integrity, not the still-open runtime writer's durable frontier. After a crash,
the maximal contiguous fully validated prefix is the authoritative recovered
WAL and all of its records participate in replay and rebuild. Validation of
that prefix includes file identity and format, header integrity, contiguous
sequence, payload integrity, record boundaries, and zero padding; it is not
limited to CRC checks.

The reader validates file identity, format, header CRC, record header CRC,
contiguous sequence, payload CRC, record boundaries, and zero padding. Any
physical validation failure is terminal for that reader instance. It never
skips or attempts to resynchronize after a damaged record.

`scan_wal()` is read-only. It returns the longest trusted record prefix and its
ending file offset. Partial record header, payload, or padding is classified as
`IncompleteTail`; other integrity failures are classified as corruption. File
truncation and recovery mutation are outside this API.

The cold-path API in `recovery.hpp` provides
`recover_incomplete_tail(path, expected)`. The caller must own exclusive access
to the quiescent file for the entire operation. Recovery first performs the same
validated scan, and mutates the file only when that scan reports an incomplete
trailing record. It truncates to `last_valid_offset`, physically synchronizes
the file, and performs a complete validated rescan before returning
`RecoveryStatus::Recovered`.

The following conditions are always refused without mutation:

- incomplete or invalid file header;
- invalid complete record, including the final record;
- corruption in the middle of the file;
- sequence gap or duplicate;
- stream, epoch, manifest, physical format, payload schema, or layout mismatch.

`RecoveryResult` reports the original and recovered sizes, removed byte count,
trusted record count, last sequence, and trusted offset. A clean file returns
`Clean` without opening it for mutation. A truncate or physical-sync failure
returns `IoError`; callers must not infer successful durability from that
result. If synchronization fails after truncation, the reported current size
may already differ from the original size; the process must remain fail-closed
instead of treating a subsequent clean scan as proof of durable recovery.
Every complete record in the validated post-crash trusted prefix is part of
history regardless of whether a client received an acknowledgement. Batches
are live-writer append-and-sync units only; the physical format has no batch
commit records or commit markers. Client retry and ingress idempotency are
outside the `RecordTape` tail contract.

## Roles

For `RecordTape`, one producer owns `try_publish()` and one composition reclaimer
owns `reclaim()`. Coordinated read-only users may call `try_view()` while the
retention precondition is maintained. `open()` and `close()` require all these
roles to be stopped.

Each Slider owns and publishes its processing frontier. A valid composition has
one runtime executor mutating a Slider while other stages observe its progress
through `GetFrontier()`. Stage topology, execution policy, and selection of the
frontier or frontiers that protect reclamation belong to the composition.

## Payload Lifetime

Publish input spans remain owned by the caller. Publication finishes its copy
synchronously and does not retain the span or access caller memory after
return.

## Retained Position View

`try_view(position)` is a non-blocking, allocation-free borrowed read of the
`RecordTape`. `Position` is an absolute zero-based position, never a slot number
or a physical sequence. The result contains `ViewStatus` and a `RecordView`
with `position` and the complete fixed-size
`std::span<const std::byte> payload`. It does not copy payload bytes or move any
frontier. The current storage has no additional per-position service fields or
stage pockets.

Status checks precede address calculation:

- `Closed`: the `RecordTape` is not open;
- `Reclaimed`: `position < tail`;
- `Unpublished`: `position >= head`;
- `Ok`: `tail <= position < head`.

Unsuccessful results contain an empty payload and zero position.
Status reflects the observed frontiers; publication may advance concurrently.
The operation acquires `head` before exposing producer-written bytes. It does
not require persistence success or consult `durable`: retained pending records
are accessible to persistence and other appropriately coordinated readers.
A downstream stage must obtain and obey its permitted boundary through its
predecessor's `GetFrontier()` before using a view.

Coordinates remain:

```text
retained positions                  [tail, head)
exclusive frontier after position p p + 1
```

Only the implementation maps `position % capacity` to a block. A reclaimed
absolute identity cannot be used to read its replacement after ring wraparound.

**Caller-owned retention is a precondition, not a feature of the view.** Before
requesting a potentially accessible position, the caller must coordinate with
the sole reclaimer so that `tail` cannot pass that position during the call or
while any returned view is used. Multiple readers may borrow the same retained
position under that condition. The range checks do not pin storage, register a
reader, or protect against concurrent reclamation. Rechecking atomics does not
make an uncoordinated reader safe.

For a linear slider composition, the reclaimer may follow the final mandatory
published frontier only after all readers of those positions have finished.

The view expires when `tail` passes its position. All view users must also stop
and retire their views before `close()`, destruction, or a subsequent reopen.
The lifecycle operations do not track outstanding views. Views and runtime
positions from a previous open lifetime cannot be reused.

## Publish

`try_publish()` is non-blocking and allocation-free. On success it copies one
payload into the block at `head`, publishes `head + 1`, and returns the
published position.

It returns `Full` when `head - tail == capacity`. Success does not mean the
payload is durable or available to a downstream stage.

The persistence-free `RecordTape` does not infer downstream failure. A
composition must stop production when any mandatory stage whose progress is
required for safe operation can no longer advance.

If the exclusive `head` reaches the end of the `Position` domain,
`try_publish()` returns `PositionExhausted`; no wrapped position is published.
Downstream stages may finish the already published valid prefix.

`PositionExhausted` is a defensive arithmetic invariant and terminal boundary
condition, not an operationally reachable failure mode for the supported system
lifetime. At a sustained rate of 10,000,000 positions per second, exhausting
the 64-bit monotonic absolute position space takes approximately 58,455 years.
The testing policy is therefore:

```text
PositionExhausted:
    contract: REQUIRED
    wraparound: FORBIDDEN
    direct runtime test: NOT REQUIRED
    RC blocker if untested: NO
```

No test hook, reduced-width `Position`, artificial initialization near
`UINT64_MAX`, or equivalent production/test machinery is required solely to
exercise this boundary. Its lack of a direct runtime test is not a test-coverage
gap. Ordinary bounded-capacity exhaustion returns `Full` and remains a separate,
operationally tested condition.

## Persistence Progress

`Persistence<Predecessor>::process_available()` reads its permitted boundary
from `predecessor.GetFrontier()` and selects at most its configured
`sync_count` from `[durable, permitted boundary)`. A configured `sync_count` of
zero is normalized to the default count of one. An empty selection performs no
physical sync.

For a non-empty batch the physical writer:

1. appends every physical record in sequence order;
2. performs exactly one OS-level physical synchronization;
3. reports success to Persistence.

Only then does Persistence publish the batch end as the new
`durable` frontier.

Physical synchronization is `FlushFileBuffers` on Windows, `fdatasync` on
POSIX, and `fsync` on macOS. Creating a file also synchronizes its initial
header; POSIX creation additionally synchronizes the parent directory entry.

Append or sync failure leaves `durable` unchanged and puts Persistence into its
terminal failed state for the current open writer lifetime. On sync failure,
records from the failed batch may already have been appended to the physical
file; there is no rollback or truncation in the live writer. Those records
remain invisible to downstream runtime stages because `durable` is not
advanced. After a crash or writer shutdown, the authoritative physical history
is instead determined by validated WAL scanning and recovery rules,
independently of the lost runtime `durable` frontier.

## Lifecycle

`RecordTape::open()` validates configuration, allocates and warms ring storage,
and resets its intrinsic boundaries. Each generic Slider owns its processed
frontier; `reset_quiescent()` initializes that boundary only while processing is
stopped.

`Persistence::open()` creates and physically synchronizes a new WAL file.
Creation is exclusive: an existing path returns `OpenStatus::FileAlreadyExists`
and is not modified. A successful open starts a new writer lifetime and clears
any failure state left by a previous closed lifetime. Cold-path incomplete-tail
recovery is an explicit, separate operation and does not reopen the live writer.
The composition is responsible for stopping all roles and closing the
components in a safe order.

## Intentional Limits

- Existing WAL files cannot be reopened by Persistence.
- Recovery removes only scanner-proven incomplete trailing records. It does not
  repair corruption or reopen an existing live writer.
- There is no segment rotation, compaction, or consumer checkpoint.
- Storage is one monolithic allocation, not an external block pool.
- The library provides no runtime stage registry or prescribed stage count;
  topology is explicit and composition-owned.
- NUMA placement and CRC acceleration are not implemented.
