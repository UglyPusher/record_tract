# Record Tract Contract

## Core public API

```cpp
namespace fexma::record_tract {

using Position = std::uint64_t;
using Frontier = std::atomic<Position>;

class RecordTape {
public:
  RecordTapeOpenStatus open(const RecordTapeConfig&) noexcept;
  PublishResult try_publish(std::span<const std::byte>) noexcept;
  AccessResult try_view(Position) const noexcept;
  void SetTailRef(const Frontier&) noexcept;
  void close() noexcept;

  bool is_open() const noexcept;
  Position head() const noexcept;
  Position tail() const noexcept;
  std::uint32_t payload_size() const noexcept;
  const Frontier& GetFrontier() const noexcept;
};

template <class Module>
class Slider {
public:
  Slider(const RecordTape&, const Frontier&, Module&) noexcept;
  Slider(const RecordTape&, const Frontier&, Module&, ExecutionPolicy) noexcept;

  SliderStatus process() noexcept;
  const Frontier& GetFrontier() const noexcept;
  Position current() const noexcept;
};
}
```

`Slider` has no upstream object dependency or upstream template parameter. It
stores only a read-only pointer to the upstream `Frontier`. `GetFrontier()` is a
composition accessor; the processing hot path loads the stored upstream atomic
directly.

## Topology and frontier meaning

```text
Head Frontier -> Slider A -> Slider B -> ... -> terminal Frontier == Tail
```

The tract is linear. `RecordTape` is a bounded preallocated ring. The Head is
the producer publication boundary. Each Slider owns and publishes one
Frontier. A Frontier value `N` certifies that its owner completed all positions
`< N` and no longer retains their views or references.

The terminal stage Frontier is the Tape Tail boundary. RecordTape observes it
directly through a `const Frontier*`; it does not own a second Tail atomic.
With no stages, the default terminal Frontier is the Tape Head, so
`Tail == Head`.

Topology wiring is performed before `RecordTape::open()`:

```cpp
RecordTape tape;
Module module_a;
Slider a{tape, tape.GetFrontier(), module_a};
tape.SetTailRef(a.GetFrontier());
```

After open, topology is immutable. `SetTailRef()` after open is a lifecycle
violation and terminates. The referenced Frontier owner must outlive the Tape.

## Slider processing

```cpp
const SliderStatus status = slider.process();
```

The Slider processes only:

```text
[current_frontier, upstream_frontier)
```

`Processed` means its Frontier advanced. `Empty` means no new upstream
positions were observed. `ModuleFailed` is the module's runtime failure
outcome. Broken Frontier, view, and lifecycle invariants fail fast and are not
returned as processing statuses. `ViewStatus` remains part of direct
`RecordTape::try_view()` results for callers that use that API directly.

`ExecutionPolicy::read_count` and `publish_count` are bootstrap preconditions
and must be greater than zero.

`read_count` specifies the maximum number of consecutive records in one
processing portion.

A single `process()` call processes as many portions as needed to cover the
range observed at the beginning of the call. Therefore, `read_count` does not
limit the total number of records processed by one call.

`publish_count` specifies how many successfully processed records may
accumulate before the Slider publishes advancement of its Frontier.

Processing-portion boundaries do not cause Frontier publication. Publication
is controlled solely by `publish_count`.

Any successfully processed remainder is published before `process()` returns,
including a successful prefix before `ModuleFailed`.

## RecordTape lifecycle

```text
Constructed -> Open -> Closed
```

- A failed first `open()` leaves the object Constructed.
- A successful `open()` starts its only lifetime.
- `open()` while Open returns `RecordTapeOpenStatus::AlreadyOpen`.
- `close()` is terminal and idempotent.
- `open()` after Close is a programmer/lifecycle violation and terminates.
- `open()` and `close()` require quiescent runtime roles.

`SetTailRef()` must precede the successful open. RecordTape has no reset or
reopen operation.

## Persistence API and durability

Persistence is declared in `namespace fexma::wal` and uses Core types from
`namespace fexma::record_tract`.

```cpp
struct PhysicalWalConfig {
  std::uint32_t alignment{wal_default_alignment};
  std::uint32_t payload_schema_version{};
  StreamKind stream_kind{StreamKind::Generic};
  StreamId stream_id{};
  EpochId epoch_id{};
  std::uint64_t first_sequence{1};
  ManifestId manifest_id{};
};

struct PersistencePolicy {
  std::size_t sync_count{1};
};

Persistence persistence{source, upstream, policy};
```

The boundary is:

```text
RecordTape / upstream Frontier -> Persistence -> PhysicalWalAdapter
```

The source Tape must be open before `Persistence::open()`. This is a bootstrap
precondition; violation terminates and is not returned as `OpenStatus`.
Payload size is derived from `RecordTape::payload_size()`. Physical WAL
alignment and other physical geometry remain WAL responsibilities.

Persistence selects at most `sync_count` records from its current Frontier to
the acquired upstream Frontier. It appends the complete batch, synchronizes,
and only then publishes its durable Frontier.

Append failure, sync failure, and physical sequence exhaustion set sticky
terminal `failed_`. Failed is absorbing: every later `process()` returns
`SliderStatus::ModuleFailed`, including an empty selection; it performs no
further append or sync and does not advance the durable Frontier. Lifecycle
misuse does not set `failed_`. Persistence close is terminal and processing and
close require quiescent roles. Reopen is not part of the contract.

## Borrowed views and memory ordering

`try_view()` returns a non-owning `std::span<const std::byte>`. The caller must
ensure that the terminal Frontier cannot pass the position while the view is
used. The API does not pin a slot or register a reader.

Payload writes precede Head release publication; readers acquire Head before
exposing payload. Stage completion precedes release publication of its
Frontier; downstream stages acquire the upstream Frontier. The producer
acquires the terminal Frontier before reusing a slot. Lifecycle operations are
not concurrent with runtime operations.
