# record_tract

`record_tract` is a small C++20 library for building bounded, ordered record-processing pipelines over preallocated storage.

It is intended for systems where:

* one producer publishes an ordered sequence of records;
* records pass through a fixed linear sequence of processing stages;
* stages may run at different rates and on different threads;
* records should remain in one shared storage instead of being copied between stage queues;
* memory use must remain bounded;
* a slow downstream stage must naturally apply backpressure to the producer;
* progress through every stage must be explicitly observable.

A typical tract looks like this:

```text id="11phwq"
Producer
   |
   v
RecordTape / Head
   |
   v
Stage A
   |
   v
Frontier A
   |
   v
Stage B
   |
   v
Frontier B / Tail
```

`record_tract` provides the storage and progress mechanics for this model. The application provides the processing logic and decides where and how stages execute.

## Core Model

The Core is built around three concepts:

### RecordTape

`RecordTape` is a bounded, preallocated storage for records.

The producer publishes records into the Tape in monotonically increasing `Position` order. Physical slots are reused, but logical positions continue to increase.

### Frontier

A `Frontier` is a monotonically increasing progress boundary.

All Core boundaries use the same exclusive semantics:

```text id="7cf6d3"
Head     = N  -> positions < N have been published
Frontier = N  -> positions < N have been completed by its owner
Tail     = N  -> positions < N are no longer retained by the tract
```

The producer publishes Head.

Each processing stage publishes its own Frontier.

The Frontier of the final stage is observed by `RecordTape` as Tail and controls when physical Tape slots may be reused.

### Stage

A stage processes records from `RecordTape` up to an upstream Frontier and publishes its own Frontier after completing them.

Core provides `Slider<Module>` as the standard implementation of a sequential stage.

The `Module` defines what processing means:

```cpp id="bq1wcs"
bool process(const RecordView& record) noexcept;
```

The `Slider` provides the mechanics of ordered traversal and Frontier publication.

Using `Slider<Module>` is not mandatory. Applications may implement their own stage mechanics directly using `RecordTape` and `Frontier`.

## Minimal Example

```cpp id="m3jd0g"
#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

namespace rt = fexma::record_tract;

class ValidationModule final {
public:
    bool process(const rt::RecordView& record) noexcept {
        // Process one record.
        return true;
    }
};

int main() {
    rt::RecordTape tape;
    ValidationModule validation;

    rt::Slider validation_slider{
        tape,
        tape.GetFrontier(),
        validation
    };

    // The final stage Frontier becomes Tail.
    tape.SetTailRef(validation_slider.GetFrontier());

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
    // rt::SliderStatus status = validation_slider.process();

    tape.close();
}
```

For the first Slider:

```text id="8lzn5a"
upstream Frontier = RecordTape Head
```

A second stage uses the first stage's Frontier:

```cpp id="w9x3gk"
rt::Slider hash_slider{
    tape,
    validation_slider.GetFrontier(),
    hashing
};
```

The resulting tract is:

```text id="i4v8nb"
Head
 |
 v
Validation Slider
 |
 v
Validation Frontier
 |
 v
Hash Slider
 |
 v
Hash Frontier / Tail
```

## Bounded Storage and Backpressure

`RecordTape` has fixed capacity.

The producer may publish only while reusable Tape slots are available.

If downstream processing falls behind, Tail stops advancing. When all available capacity is retained, `try_publish()` returns:

```cpp id="cjg7z1"
PublishStatus::Full
```

This is normal bounded backpressure, not an error.

Advancing the terminal Frontier makes old Tape slots reusable. There is no separate reclaim operation.

## Execution

Core does not create or manage threads.

For example, an application may use:

```text id="1q0mbd"
Thread 1: Producer
Thread 2: Validation Slider
Thread 3: Hash Slider
```

or execute several stages from one thread.

Core assumes:

```text id="97rbtu"
one producer per RecordTape
one executor per Slider
one owner/writer per Frontier
```

Scheduling, polling, waiting, thread placement, and executor design belong to the application.

## Scope

Core provides:

* bounded preallocated record storage;
* monotonically increasing positions;
* read-only borrowed record views;
* explicit progress Frontiers;
* ordered sequential stage mechanics through `Slider<Module>`;
* bounded backpressure through the terminal Frontier;
* synchronization through published Frontiers.

Core does **not** provide:

* worker threads;
* executors;
* schedulers;
* wait strategies;
* dynamic topology;
* branching or DAG composition;
* queues between stages;
* persistence;
* replay or recovery;
* snapshots;
* domain semantics.

The supported Core topology is a static linear tract:

```text id="iwxq9d"
Head -> Stage -> Stage -> ... -> Tail
```

Topology is configured before `RecordTape::open()` and remains unchanged during processing.

## Complete Example

A complete concurrent Core example is available in:

```text id="0cl3j4"
examples/basic_tract/
```

Start with:

```text id="1i7ax2"
examples/basic_tract/demo_app.cpp
```

The example contains:

```text id="q4w8tb"
Producer
   |
   v
RecordTape / Head
   |
   v
PayloadValidationModule
   |
   v
Validation Frontier
   |
   v
RollingHashModule
   |
   v
Hash Frontier / Tail
```

The producer and both stages run on separate threads. A small Tape capacity forces physical slot reuse and demonstrates bounded backpressure.

## Build

The project requires C++20 and CMake.

```bash id="p9vb6n"
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

A Core consumer links:

```cmake id="nxk1y3"
target_link_libraries(my_target PRIVATE fexma::record_tract)
```

The repository currently provides build-tree targets. Install rules and package exports are not yet provided.

The repository also contains optional WAL functionality under a separate target:

```text id="m7s5ak"
fexma::record_tract   Core tract mechanics
fexma::wal            Persistence, physical WAL, Reader, Recovery
fexma::binary         low-level binary helpers
```

`fexma::wal` depends on `fexma::record_tract`. Core itself does not depend on WAL.

## Documentation

A practical reading order is:

```text id="r6u1eq"
README
  |
  v
HOW_TO_USE
  |
  +--> examples/basic_tract
  |
  v
CONTRACT / INVARIANTS
  |
  v
DESIGN
```

* [`doc/HOW_TO_USE.md`](doc/HOW_TO_USE.md) — practical Core walkthrough, from `RecordTape` to multi-stage tracts and custom stages.
* [`doc/CONTRACT.md`](doc/CONTRACT.md) — exact public behavioral contract.
* [`doc/INVARIANTS.md`](doc/INVARIANTS.md) — topology, ownership, ordering, lifetime, and concurrency invariants.
* [`doc/DESIGN.md`](doc/DESIGN.md) — architecture and implementation decisions.
* [`doc/BUILDING.md`](doc/BUILDING.md) — build and test instructions.
* [`doc/FILE_FORMAT.md`](doc/FILE_FORMAT.md) — physical WAL format.

For normal Core use, start with this README and `HOW_TO_USE.md`. Use `CONTRACT.md` and `INVARIANTS.md` when exact guarantees matter.
