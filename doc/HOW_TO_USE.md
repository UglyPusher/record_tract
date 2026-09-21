# How to Use `record_tract`

`record_tract` is a small C++20 library for building linear record-processing pipelines on top of a bounded, preallocated buffer.

The library is responsible for record storage, ordered progression through processing stages, and explicit progress boundaries. It does not create worker threads or decide where and when processing should run.

---

## 1. What Problems Does `record_tract` Solve?

`record_tract` is intended for systems where a producer publishes an ordered sequence of records and those records must then pass through one or more sequential processing stages.

For example:

```text
Producer
    |
    v
RecordTape
    |
    v
Validation
    |
    v
Normalization
    |
    v
Projection
```

Each stage processes the same ordered stream and can only start processing records that are already available from the preceding stage.

This foundation can be used to build:

* sequential processing pipelines;
* chains of dependent processing stages;
* validation and normalization;
* risk processing;
* persistence stages;
* projections;
* other systems where record ordering and explicitly observable stage progress matter.

A record is stored in `RecordTape`. Its payload does not need to be copied into a separate queue when moving from one stage to another: stages work with the record stored in the Tape.

Tape capacity is bounded. The producer cannot overwrite a slot while its record may still be needed by a stage. If no free space is available, the producer receives `Full`. Bounded memory therefore provides natural backpressure.

`record_tract` is **not**:

* a thread pool;
* a scheduler;
* a message broker;
* a dynamic routing system;
* a framework for arbitrary application graphs.

Core provides bounded storage and the mechanics for linear processing progress:

```text
Producer -> Stage 1 -> Stage 2 -> ... -> Stage N
```

The application decides where, when, and on which thread each stage runs.

---

## 2. The Basic Model

Five concepts are enough to understand the basic model.

### RecordTape

`RecordTape` is a bounded, preallocated storage for records.

The producer publishes records sequentially into the Tape. Physical Tape slots are reused over time, while logical record positions increase monotonically.

### Head

`Head` separates already published records from records that have not yet been published.

Its value is the first unpublished position.

If:

```text
Head = N
```

then the published positions are:

```text
[0, N)
```

and the next record will be published at position `N`.

For example:

```text
Head = 0    Tape is empty
Head = 1    position 0 has been published
Head = 2    positions 0, 1 have been published
Head = 3    positions 0, 1, 2 have been published
```

A stage must not process position `N` until Head has advanced past it.

### Frontier

Each stage has its own `Frontier`.

A Frontier shows how far that stage has progressed through the stream.

For example, if a stage has completed records:

```text
0 1 2 3 4 5 6 7 8 9
```

its Frontier becomes:

```text
10
```

In other words, a Frontier value of `N` means:

> all positions `< N` have been completed by this stage.

The next stage uses the preceding stage's Frontier as the boundary of records available to it.

Head and Frontier therefore use the same exclusive-boundary semantics:

```text
Head     = N  -> all positions < N have been published
Frontier = N  -> all positions < N have been completed by this stage
```

### Slider

`Slider<Module>` is a ready-made implementation of a conventional sequential stage.

A Slider:

1. observes the preceding stage's Frontier;
2. accesses available records from the Tape in order;
3. passes each record to a user-provided `Module`;
4. advances its own Frontier after successful processing.

The `Module` defines **what processing means**.

The `Slider` provides **the mechanics of progressing through the Tape**.

### Tail

The Frontier of the final stage is connected to `RecordTape` as its Tail.

Tail indicates which records have passed through the entire tract and no longer need to be retained for consumers.

When:

```text
Tail = N
```

positions `< N` are no longer retained by the tract, and their physical Tape slots may be reused by the producer.

This gives all boundaries the same basic model:

```text
Head     = N  -> positions < N have been published
Frontier = N  -> positions < N have been completed by this stage
Tail     = N  -> positions < N are no longer retained by the tract
```

In a linear tract:

```text
Producer
   |
   v
  Head
   |
   v
Slider 1
   |
   v
Frontier 1
   |
   v
Slider 2
   |
   v
Frontier 2
   |
   v
  Tail
```

---

## 3. RecordTape Without Stages

The simplest case does not require a `Slider` at all.

Create a Tape:

```cpp
fexma::record_tract::RecordTape tape;
```

and open it:

```cpp
const auto status = tape.open({
    payload_size,
    capacity,
    fexma::record_tract::default_alignment
});

if (status != fexma::record_tract::RecordTapeOpenStatus::Ok) {
    // initialization failed
}
```

After `open()`, the producer can publish records:

```cpp
const auto result = tape.try_publish(payload);
```

A successful publication returns:

```cpp
result.status == PublishStatus::Ok
```

and `result.position` contains the position of the published record.

After a successful publication, Head advances to the next position.

If no free slot is currently available:

```cpp
result.status == PublishStatus::Full
```

A published record can be accessed by its position:

```cpp
const auto result = tape.try_view(position);
```

The resulting `RecordView` is a view of a record stored inside the Tape, not a separate copy of its payload.

When no stages are connected to the Tape, there is no separate consumer retaining records. Tail therefore follows Head.

This case is primarily useful for understanding `RecordTape` itself. In an actual tract, Tail is normally defined by the final processing stage.

When finished:

```cpp
tape.close();
```

---

## 4. Writing a Module

Usually, a record needs to be processed by application code.

For this, define a Module:

```cpp
class ValidationModule {
public:
    bool process(const fexma::record_tract::RecordView& record) noexcept {
        // validate record
        return true;
    }
};
```

The contract is minimal:

```cpp
bool process(const RecordView& record) noexcept;
```

`true` means that the record was processed successfully.

`false` means that processing of this record did not complete successfully.

A Module does not need to know about:

* Head;
* Tail;
* Frontier;
* Slider positioning;
* execution threads;
* backpressure.

It is ordinary application code that processes one record.

---

## 5. Adding a Slider

Create a Module:

```cpp
ValidationModule validation;
```

Now create a Slider:

```cpp
fexma::record_tract::Slider validation_slider(
    tape,
    tape.GetFrontier(),
    validation
);
```

There are three important objects here:

```text
tape
```

— where records are read from;

```text
tape.GetFrontier()
```

— the boundary up to which records are available;

```text
validation
```

— what to do with each record.

For the first Slider, the upstream Frontier is the Tape's own Frontier — its Head.

The resulting topology is:

```text
Producer -> RecordTape / Head -> Validation Slider
```

The Slider has its own Frontier:

```cpp
validation_slider.GetFrontier()
```

It represents the progress of the Validation stage.

Processing is performed by calling:

```cpp
const auto status = validation_slider.process();
```

If available records were processed:

```cpp
status == SliderStatus::Processed
```

If there are currently no new records available:

```cpp
status == SliderStatus::Empty
```

If the Module reports a failure:

```cpp
status == SliderStatus::ModuleFailed
```

---

## 6. Building a Chain of Stages

Add a second Module:

```cpp
RollingHashModule hashing;
```

and a second Slider:

```cpp
fexma::record_tract::Slider hash_slider(
    tape,
    validation_slider.GetFrontier(),
    hashing
);
```

Notice the second argument.

The first Slider receives:

```cpp
tape.GetFrontier()
```

The second receives:

```cpp
validation_slider.GetFrontier()
```

Therefore, the second Slider cannot overtake the first one.

The resulting chain is:

```text
RecordTape / Head
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
    Hash Frontier
```

The chain can be extended in the same way:

```cpp
Slider stage_c(
    tape,
    hash_slider.GetFrontier(),
    module_c
);
```

Each stage depends only on the Frontier of its immediate predecessor.

---

## 7. Connecting Tail

The Tape needs to know which stage is the final consumer of a record.

For this chain, it is `hash_slider`:

```cpp
tape.SetTailRef(hash_slider.GetFrontier());
```

The complete tract is now:

```text
Head
 |
 v
Validation
 |
 v
Validation Frontier
 |
 v
Hash
 |
 v
Hash Frontier
 |
 v
Tail
```

Until the Hash stage completes a record, the corresponding Tape slot cannot be reused.

A slow final stage therefore eventually limits the producer's progress.

This creates a bounded pipeline without requiring an additional queue-management mechanism.

The topology is configured before `open()` and does not change after processing begins.

---

## 8. Where Stages Run

`record_tract` does not create threads.

The simplest Slider execution loop looks like this:

```cpp
while (running) {
    const auto status = validation_slider.process();

    if (status == SliderStatus::Empty) {
        std::this_thread::yield();
    }
}
```

The application may run different Sliders on different threads:

```text
Thread 1: Producer
Thread 2: Validation Slider
Thread 3: Hash Slider
```

This is how the `basic_tract` demo is structured.

However, this is not a Core requirement.

The application may run multiple stages on the same thread, use its own executor, or choose another execution strategy.

For Core, what matters is maintaining Frontier ordering and the single-writer rule for each Frontier.

---

## 9. Backpressure

`RecordTape` has a fixed capacity.

Suppose:

```text
capacity = 64
```

The producer can advance only as far as the available Tape space allows.

If the final stages fall behind, Tail stops advancing.

The producer gradually fills the remaining slots and eventually receives:

```cpp
PublishStatus::Full
```

This is not an error.

It means:

> the next physical slot still contains a record that may be needed by the tract.

The application decides what to do in this situation:

* retry;
* call `yield`;
* wait for an external signal;
* use its own wait strategy.

Core does not impose such a policy.

---

## 10. ExecutionPolicy

By default, a Slider uses:

```cpp
ExecutionPolicy{1, 1}
```

A policy can be specified explicitly when needed:

```cpp
constexpr fexma::record_tract::ExecutionPolicy policy{
    8,
    4
};
```

The two values are:

```text
read_count
publish_count
```

`read_count` specifies the maximum number of records that a Slider accepts for processing in one batch.

`publish_count` specifies the number of successfully processed records after which the Slider publishes advancement of its Frontier.

For example:

```cpp
ExecutionPolicy{8, 4}
```

means:

> The Slider accepts no more than 8 records for processing in one batch and publishes its Frontier after every 4 successfully processed records.

If successfully processed records remain at the end of the batch but have not yet been reflected in the Frontier, the Slider publishes that remainder before completing the batch.

The policy allows batching behavior to be changed without changing the Module.

For initial use of the library, the default policy is usually sufficient.

---

## 11. What Happens When a Module Fails

A Module may return:

```cpp
false
```

In that case, the Slider returns:

```cpp
SliderStatus::ModuleFailed
```

It is important to distinguish successfully processed records from the record on which the failure occurred.

If the Slider successfully processed several records before the failure, but their progress has not yet been published because of the batching policy, that successfully processed prefix is published.

The position for which the Module returned `false` is **not** considered complete and is not included in the Frontier.

A Frontier therefore never claims that a stage successfully passed a record that the Module did not actually complete.

---

## 12. Slider Is Optional

`Slider<Module>` provides ready-made mechanics for the most common kind of sequential stage.

However, `RecordTape` does not require every consumer to be a Slider.

You can implement your own stage.

Such a stage needs:

* a `RecordTape`;
* the preceding stage's Frontier;
* its own Frontier.

In simplified form, such a stage works like this:

```cpp
const Position available =
    upstream_frontier.load(std::memory_order_acquire);

while (current < available) {
    const auto record = tape.try_view(current);

    // application-specific processing

    ++current;
}

own_frontier.store(current, std::memory_order_release);
```

The fundamental protocol is therefore simple:

```text
read upstream Frontier
        |
        v
process available records in order
        |
        v
publish own Frontier
```

Such a Frontier can be passed to the next stage in exactly the same way as the Frontier of a regular Slider.

If the custom stage is the final stage:

```cpp
tape.SetTailRef(own_frontier);
```

`Slider` is therefore not a mandatory framework around the Tape. It is a ready-made implementation of the standard sequential-consumer mechanics.

---

## 13. Basic Rules

Several rules must be followed for correct tract operation.

### One Producer per Tape

A `RecordTape` assumes a single producer.

### One Executor per Slider

A Slider must not be executed concurrently by multiple threads.

### One Writer per Frontier

Each Frontier has one owner that advances it.

Other participants only observe its value.

### Frontier Only Moves Forward

Published progress never moves backward.

### The Tract Is Linear

The normal topology is:

```text
Head -> Stage A -> Stage B -> Stage C -> Tail
```

Core is not intended for building DAGs with branching and later Frontier merging.

### Topology Is Configured Before Execution

Sliders and the connection between the final Frontier and Tail are created before `RecordTape::open()`.

The topology is not rebuilt during steady-state processing.

### Respect Object Lifetimes

`RecordTape`, the Module, and the upstream Frontier must remain alive for as long as a Slider using them may run.

A Frontier connected through:

```cpp
SetTailRef(...)
```

must remain alive for as long as the Tape may access it.

### RecordView Is a Borrowed View

`RecordView` refers to memory inside the Tape.

It is not an owning copy of a record.

A view must not be retained and used after Tail is allowed to advance past its position and the corresponding slot may have been reused.

---

## 14. Complete Example

A complete runnable example is located in:

```text
examples/basic_tract/
```

Its structure is:

```text
basic_tract/
├── CMakeLists.txt
├── demo_app.cpp
├── demo_modules.hpp
└── demo_payload.hpp
```

`demo_app.cpp` shows the complete composition:

```text
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

The producer and both stages run on separate threads.

The small capacity and large number of records force the Tape to reuse physical slots repeatedly and demonstrate backpressure when consumers fall behind.

A good place to start reading the example is `demo_app.cpp`.

---

## 15. Further Documentation

HOW_TO_USE describes practical use of Core.

Once the basic model is clear, more precise guarantees and restrictions are documented separately:

* `CONTRACT.md` — exact public contract;
* `INVARIANTS.md` — Tape, Frontier, and stage invariants;
* `DESIGN.md` — internal structure and architectural decisions;
* `BUILDING.md` — build and verification instructions.

A practical reading order is:

```text
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

For normal library use, README, HOW_TO_USE, and the public headers should usually be sufficient. The remaining documentation is intended for cases where exact guarantees, concurrency semantics, or Core internals need to be understood.
