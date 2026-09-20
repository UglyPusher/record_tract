# record_tract

`record_tract` is a small C++20 library for building bounded, ordered, low-overhead record-processing pipelines.

The Core consists of two mechanics:

* `RecordTape` — a preallocated bounded ring of immutable records;
* `Slider<Module>` — an ordered processing stage with its own progress frontier.

Stages are composed into a static linear tract:

```text
Producer
   |
   v
Head Frontier
   |
   v
Slider A
   |
   v
Frontier A
   |
   v
Slider B
   |
   v
Frontier B
   |
   v
Tail
```

The terminal frontier is the tape reclamation boundary.

There is no separate Tail progress state.

## Why

The library is intended for pipelines where the data path should remain small and explicit:

```text
preallocated storage
+ immutable records
+ monotonic frontiers
+ ordered processing
```

`record_tract` provides these mechanics without owning the runtime around them.

Core does **not** provide:

* worker threads;
* executors;
* schedulers;
* wait strategies;
* dynamic topology;
* queues between stages;
* persistence;
* domain semantics.

The application decides where and how stages execute.

## Core model

A `RecordTape` owns a fixed-capacity preallocated ring.

Records are addressed by absolute monotonic `Position` values rather than by physical ring slots.

The producer publishes the Head frontier:

```text
Head = exclusive end of published records
```

Every Slider owns another frontier:

```text
Frontier N = all positions < N are complete for this stage
```

A published stage frontier also certifies that the stage no longer retains views or references to those positions.

The terminal stage frontier is observed directly by `RecordTape` as Tail:

```text
Tail = terminal Frontier
```

Therefore:

```text
tail <= ... <= stage frontier <= ... <= head

head - tail <= capacity
```

When no stages exist, the tape observes its own Head as Tail:

```text
Tail == Head
```

## Minimal example

```cpp
#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

namespace rt = fexma::record_tract;

class Module final {
public:
    bool process(const rt::RecordView& record) noexcept {
        // Complete processing of this record.
        return true;
    }
};

int main() {
    rt::RecordTape tape;
    Module module;

    rt::Slider slider{
        tape,
        tape.GetFrontier(),
        module
    };

    // The terminal Slider frontier becomes Tail.
    // Topology must be wired before open().
    tape.SetTailRef(slider.GetFrontier());

    if (tape.open({
            .payload_size = 64,
            .capacity = 4096,
            .alignment = rt::default_alignment
        }) != rt::RecordTapeOpenStatus::Ok) {
        return 1;
    }

    // Producer:
    //
    // rt::PublishResult result = tape.try_publish(payload);

    // Stage executor:
    //
    // switch (slider.process()) {
    // case rt::SliderStatus::Processed:
    // case rt::SliderStatus::Empty:
    //     break;
    // case rt::SliderStatus::ModuleFailed:
    //     // Domain/runtime failure.
    //     break;
    // }

    tape.close();
}
```

A complete concurrent example is available in:

```text
examples/basic_tract/main.cpp
```

See [doc/HOW_TO_USE.md](doc/HOW_TO_USE.md) for a practical walkthrough.

## Building a tract

A Slider receives its upstream frontier explicitly.

It does not retain or depend on an upstream stage object.

```cpp
ModuleA module_a;
ModuleB module_b;

rt::Slider a{
    tape,
    tape.GetFrontier(),
    module_a
};

rt::Slider b{
    tape,
    a.GetFrontier(),
    module_b
};

tape.SetTailRef(b.GetFrontier());
```

This creates:

```text
Head
  |
  v
Slider A
  |
  v
Frontier A
  |
  v
Slider B
  |
  v
Frontier B == Tail
```

The tape is the record source.

The upstream frontier is the processing permission boundary.

These responsibilities are independent:

```text
RecordTape      -> where the record lives
Frontier        -> how far this dependency has completed
Slider          -> ordered processing mechanics
Module          -> what processing means
```

## Frontier

A public Core frontier is:

```cpp
using Frontier = std::atomic<Position>;
```

A frontier is an exclusive monotonic progress boundary.

If its value is `N`, its owner certifies:

```text
all positions < N are complete
```

and that it retains no borrowed record views below `N`.

Frontiers form the synchronization edges of the tract.

The producer release-publishes Head after filling a record.

A stage acquire-loads its upstream frontier before processing records and release-publishes its own frontier after completing them.

The producer acquire-loads the terminal frontier before reusing ring capacity.

The runtime path does not require `seq_cst`.

## Slider

`Slider<Module>` provides synchronous ordered stage mechanics.

Its essential operation is:

```cpp
rt::SliderStatus status = slider.process();
```

The Slider processes:

```text
[current frontier, observed upstream frontier)
```

in absolute position order.

Possible runtime outcomes are:

```cpp
SliderStatus::Processed
SliderStatus::Empty
SliderStatus::ModuleFailed
```

`Processed` means progress was completed and published.

`Empty` means the acquired upstream frontier contained no new positions.

`ModuleFailed` means the module explicitly returned failure.

Broken tract invariants are programmer/composition errors and fail fast rather than becoming ordinary runtime statuses.

## Module

A module provides the domain operation:

```cpp
bool process(const rt::RecordView&) noexcept;
```

Returning `true` certifies successful completion of that position.

Returning `false` produces `SliderStatus::ModuleFailed`.

A Slider never publishes a failed position.

A successfully completed prefix before the failed position is published before `process()` returns.

Core does not interpret record payloads or module semantics.

## Execution policy

A Slider may be configured with:

```cpp
rt::ExecutionPolicy{
    .read_count = 32,
    .publish_count = 8
};
```

Both values must be greater than zero.

`read_count` partitions traversal into internal passes.

`publish_count` controls how many successfully processed records may accumulate between frontier publications.

The final successful residual prefix is always published before return.

Execution policy changes processing mechanics, not ordering semantics.

## Bounded storage and backpressure

`RecordTape` has fixed capacity.

For a valid tract:

```text
head - tail <= capacity
```

When:

```text
head - tail == capacity
```

the producer receives:

```cpp
PublishStatus::Full
```

This is normal bounded backpressure.

No record can be overwritten until the terminal frontier has passed its absolute position.

There is no explicit reclaim operation: advancement of the terminal frontier makes capacity reclaimable by the producer.

## Zero-copy views

`RecordTape::try_view(position)` returns a borrowed immutable view:

```cpp
struct RecordView {
    Position position;
    std::span<const std::byte> payload;
};
```

The view does not own or pin its slot.

A caller using `try_view()` directly must guarantee that Tail cannot pass the position while the view is in use.

Slider provides that guarantee through tract ordering: its frontier advances only after processing has completed and the borrowed view has been retired.

## Static topology

The supported Core topology is deliberately linear:

```text
Head -> Stage -> Stage -> ... -> Tail
```

There is no:

* branch;
* DAG;
* runtime topology registry;
* dynamic stage insertion;
* separate Tail publication object.

Topology wiring is a bootstrap operation.

The terminal frontier is connected before `RecordTape::open()`:

```cpp
tape.SetTailRef(last_stage.GetFrontier());
```

After the tape is open, topology is immutable.

Calling `SetTailRef()` after `open()` is a lifecycle violation.

Every referenced frontier owner must outlive the object observing that frontier.

## Lifecycle

`RecordTape` has a one-shot lifecycle:

```text
Constructed -> Open -> Closed
```

A failed initial configuration or allocation leaves the tape in `Constructed`.

A successful `open()` starts its only runtime lifetime.

Calling `open()` while already open returns:

```cpp
RecordTapeOpenStatus::AlreadyOpen
```

`close()` is terminal and idempotent.

Reopening a successfully used tape is not part of the contract.

Bootstrap and shutdown are quiescent operations. Publishing, processing, and borrowed views must not race with lifecycle changes.

## Memory behavior

Record storage is allocated during `RecordTape::open()`.

The tape uses fixed-size aligned slots in a bounded ring.

The steady-state Core path performs no record-storage allocation:

```text
publish
view
Slider processing
Tail observation
```

Head and stage frontiers are cache-line isolated where they are independently written.

The current implementation uses a 64-byte cache-line assumption for this isolation.

## Concurrency model

Core assumes:

```text
one producer per RecordTape
one executor per Slider
one owner/writer per Frontier
```

Multiple stages may execute on different threads.

The library does not create those threads and does not prescribe polling or waiting behavior.

For example:

```text
producer thread
      |
      v
    Head
      |
      v
Slider A thread
      |
      v
     F1
      |
      v
Slider B thread
      |
      v
     F2 / Tail
```

A different application may execute several stages from one thread without changing the tract model.

## Optional WAL

Persistence is separate from Core.

The repository currently exposes three build-tree targets:

```text
fexma::record_tract   Core tract mechanics
fexma::wal            Persistence, physical WAL, Reader, Recovery
fexma::binary         low-level binary helpers
```

A Core-only application needs no WAL:

```text
Producer -> RecordTape -> Slider -> ... -> Tail
```

Persistence can instead occupy a stage boundary:

```text
Head / upstream Frontier
          |
          v
     Persistence
          |
          v
   durable Frontier
```

Persistence publishes its frontier only after the selected records have been appended and physically synchronized.

WAL lifecycle, physical format, validation, Reader, and Recovery are separate from the Core tract contract.

See:

* [doc/CONTRACT.md](doc/CONTRACT.md)
* [doc/DESIGN.md](doc/DESIGN.md)
* [doc/FILE_FORMAT.md](doc/FILE_FORMAT.md)

## Repository layout

```text
include/fexma/record_tract/   Core public API
src/record_tract/             Core implementation
tests/core/                   Core tests

include/fexma/wal/            WAL public API
src/wal/                      WAL implementation
tests/wal/                    WAL tests

include/fexma/binary/         binary helpers
tests/binary/                 binary-helper tests

examples/basic_tract/         minimal concurrent Core example

doc/                          detailed documentation
```

## Build

The project requires C++20 and CMake.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Core consumers link:

```cmake
target_link_libraries(my_target PRIVATE fexma::record_tract)
```

WAL consumers link:

```cmake
target_link_libraries(my_target PRIVATE fexma::wal)
```

The repository currently exposes build-tree targets only. Install and package-export rules are not yet provided.

## Documentation

* [HOW_TO_USE.md](doc/HOW_TO_USE.md) — minimal Core integration;
* [CONTRACT.md](doc/CONTRACT.md) — public behavioral contract;
* [INVARIANTS.md](doc/INVARIANTS.md) — topology, ownership, ordering, and concurrency invariants;
* [DESIGN.md](doc/DESIGN.md) — architecture and implementation details;
* [FILE_FORMAT.md](doc/FILE_FORMAT.md) — physical WAL format;
* [BUILDING.md](doc/BUILDING.md) — build and test instructions.

## Scope

`record_tract` is a mechanism, not a processing framework.

Its Core reduces to:

```text
bounded preallocated records
        +
monotonic frontiers
        +
ordered stages
        +
terminal-frontier reclamation
```

Scheduling, thread placement, waiting, domain behavior, persistence, replay, snapshots, and higher-level recovery policy remain outside the Core.
