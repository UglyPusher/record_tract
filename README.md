# record_tract

`record_tract` is a C++20 library for a single-publisher, ordered linear
record tract. Its Core is a bounded preallocated `RecordTape` and statically
composed `Slider<Module>` stages. Optional WAL persistence is a specialized
stage over the same tape.

Build-tree targets:

- `fexma::record_tract` — Core;
- `fexma::wal` — Persistence, physical WAL, Reader, and Recovery;
- `fexma::binary` — binary helpers.

## Core model

```text
Head frontier -> Slider A -> Slider B -> ... -> terminal frontier == Tail
```

`RecordTape` owns a fixed-size, preallocated ring. The producer owns and
publishes its Head frontier. Every Slider owns one mutable Frontier and exposes
it to downstream stages as `const Frontier&`. A Frontier value `N` certifies
that its stage completed every position `< N` and no longer retains views of
those records.

The terminal stage Frontier is also the RecordTape reclamation boundary. The
tape observes it directly as its Tail boundary. A tract without stages uses
the tape Head as Tail.

Topology is linear and immutable during runtime. All `SetTailRef()` wiring must
be complete before `RecordTape::open()`. Calling it after open is a lifecycle
violation and terminates.

## RecordTape

```cpp
namespace core = fexma::record_tract;

core::RecordTape tape;
core::Frontier terminal{};

tape.SetTailRef(terminal); // optional; must precede open()
const auto status = tape.open({payload_size, capacity,
                               core::default_alignment});
```

The default terminal reference points to the tape's own Head, which gives the
zero-stage `Tail == Head` behavior. `try_publish()` is single-producer and
returns `Full` for normal bounded backpressure. `try_view()` is a direct
borrowed view API for callers that maintain the retention precondition.

RecordTape lifecycle is one-shot:

```text
Constructed -> Open -> Closed
```

An invalid first open or allocation failure leaves the object Constructed. An
open call while Open returns `RecordTapeOpenStatus::AlreadyOpen`. `close()` is
terminal and idempotent. Reopening after a successful lifetime terminates.
Lifecycle operations require a quiescent tract; concurrent `close()` and
runtime operations are not supported.

## Slider

```cpp
class Module final {
public:
  bool process(const core::RecordView&) noexcept { return true; }
};

Module module_a;
Module module_b;
core::Slider a{tape, tape.GetFrontier(), module_a};
core::Slider b{tape, a.GetFrontier(), module_b};
tape.SetTailRef(b.GetFrontier()); // before tape.open()
```

The upstream Frontier is passed explicitly. Slider does not know or retain an
upstream object. `process()` returns `Processed`, `Empty`, or `ModuleFailed`.
`ModuleFailed` is the module's runtime outcome. Broken Frontier, view, or
lifecycle invariants fail fast rather than becoming processing statuses.

`ExecutionPolicy::read_count` and `publish_count` must both be greater than
zero. `publish_count` controls publication cadence; a successful residual
prefix is published before return, including before a module failure return.

Only one executor may call `process()` on a given Slider. The module, tape,
upstream Frontier owner, and downstream observers must obey the composition's
lifetime and quiescence rules.

## Persistence

```text
RecordTape / upstream Frontier -> Persistence -> PhysicalWalAdapter
```

```cpp
namespace wal = fexma::wal;

wal::Persistence persistence{tape, tape.GetFrontier(), policy};
tape.SetTailRef(persistence.GetFrontier()); // before tape.open()

if (!persistence.open(path, physical_wal_config).ok()) return 1;
core::Slider downstream{tape, persistence.GetFrontier(), module};
```

The source Tape must already be open before `Persistence::open()`. Payload
size is derived from `RecordTape::payload_size()`; physical WAL alignment and
other WAL geometry remain owned by the WAL configuration and adapter.

Persistence appends a bounded batch and synchronizes it before publishing its
durable Frontier. Append failure, sync failure, and physical sequence
exhaustion set sticky terminal `failed_`. Failed is absorbing: subsequent
`process()` calls return `ModuleFailed`, perform no I/O, and do not advance the
durable Frontier. Lifecycle misuse does not set `failed_`. Persistence close is
terminal; reopening is not part of the contract. Its processing and close
operations also require quiescence.

## Build

```cmake
target_link_libraries(core_consumer PRIVATE fexma::record_tract)
target_link_libraries(wal_consumer PRIVATE fexma::wal)
```

The repository has no install or package-export rules.
