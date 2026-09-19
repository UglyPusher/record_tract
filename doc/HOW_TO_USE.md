# Core HOW TO USE

This example uses only the public Core headers and implements one stage:

```text
producer thread -> RecordTape -> Slider<RecordTape, Module>
                                      |
                                      v
                              sole reclaimer
```

The complete compiling example is
[`examples/basic_tract/main.cpp`](../examples/basic_tract/main.cpp).

## Headers and namespace

Include the Core headers:

```cpp
#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>
```

The headers are physically under `record_tract`, but the current public types
remain in `fexma::wal`:

```cpp
namespace core = fexma::wal;
```

This example does not use WAL, Persistence, Reader, Recovery, `detail`, or
private headers.

## Open and publish

`RecordTape` has fixed-size payloads and bounded capacity. Open it before
starting runtime threads:

```cpp
core::RecordTape tape;
if (!tape.open({payload_size, capacity, core::default_alignment}).ok()) {
  return 1;
}
```

The producer is the sole caller of `try_publish()`. A successful result returns
the absolute published position. `PublishStatus::Full` is normal transient
backpressure and can be retried; other failures stop the composition.

## Module and Slider

A module synchronously processes one borrowed `RecordView` and returns `true`
only when that position is complete:

```cpp
class Module final {
public:
  bool process(const core::RecordView&) noexcept {
    ++processed_;
    return true;
  }

private:
  core::Position processed_{};
};
```

Construct the root Slider with the tape as both source and predecessor. The
default `ExecutionPolicy{1, 1}` is used:

```cpp
Module module;
core::Slider<core::RecordTape, Module> slider(tape, module);
```

The Slider stores references to the tape, predecessor, and module. Each of
those objects must outlive the Slider. Only one runtime executor may mutate a
given Slider by calling `process_available()`.

`process_available()` is synchronous. `SliderResult::ok()` is true for both
`SliderStatus::Processed` and `SliderStatus::Empty`; `Empty` means that the
predecessor frontier had no new records at the observation point. Other Slider
statuses are failures for this minimal example.

## Frontiers and reclaim

`RecordTape::GetFrontier()` is the published producer end and equals `head()`.
`Slider::GetFrontier()` is the exclusive end of records successfully processed
by the module. A position `p` is protected until the Slider has published at
least `p + 1`.

The Slider executor is also the sole reclaimer in this one-stage topology. It
calls `reclaim(slider.GetFrontier())` only after `process_available()` returns;
that return retires the Slider's borrowed views. Publishing the Slider frontier
alone does not release storage.

## Drain and shutdown

The producer publishes an atomic completion flag only after its final
successful publication. The executor continues processing after that flag until
both conditions hold:

```text
producer_done == true
Slider::GetFrontier() == RecordTape::GetFrontier()
```

At that point the producer and executor threads are joined. Only then may the
tape be closed. If either thread observes a non-transient error, it signals the
shared failure flag so the other thread can stop and the composition can join
both threads before closing the tape.

The example uses `std::this_thread::yield()` as a minimal retry policy for
`Full` and `Empty`. Core does not provide a scheduler, wait strategy, worker
thread, or drain abstraction.

## In-tree CMake

The repository's current build tree exposes the Core target directly:

```cmake
add_executable(example_basic_tract examples/basic_tract/main.cpp)
target_link_libraries(example_basic_tract PRIVATE fexma::record_tract)
```

There are no install rules or `find_package`/package-export claims for this
example.
