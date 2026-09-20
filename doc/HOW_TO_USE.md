# Core HOW TO USE

This example uses only the public Core headers and builds a linear tract with
one Slider:

```text
producer -> Head Frontier -> Slider Frontier == Tail
```

The complete compiling example is
[`examples/basic_tract/main.cpp`](../examples/basic_tract/main.cpp).

## Setup and bootstrap

```cpp
#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

namespace core = fexma::record_tract;

core::RecordTape tape;
Module module;
core::Slider slider{tape, tape.GetFrontier(), module};
tape.SetTailRef(slider.GetFrontier());

if (tape.open({payload_size, capacity, core::default_alignment}) !=
    core::RecordTapeOpenStatus::Ok) {
  return 1;
}
```

All topology wiring must happen before `open()`. The terminal Frontier owner
must outlive the Tape. `SetTailRef()` after open is a lifecycle violation and
terminates.

`RecordTape` is a bounded preallocated ring. The producer is the sole caller
of `try_publish()`. A successful result contains an absolute position;
`PublishStatus::Full` is normal transient backpressure.

## Module and processing

```cpp
class Module final {
public:
  bool process(const core::RecordView& record) noexcept {
    // Complete this position before returning true.
    return consume(record);
  }
};
```

`Slider` receives its upstream Frontier explicitly:

```cpp
core::Slider first{tape, tape.GetFrontier(), module_a};
core::Slider second{tape, first.GetFrontier(), module_b};
```

It does not depend on an upstream object. A Slider reads only
`[current, upstream_frontier)`. A Frontier publication certifies completion of
all positions below it and retirement of their borrowed views.

```cpp
const core::SliderStatus result = slider.process();
```

`Processed` means the Slider advanced its Frontier, `Empty` means no new
upstream positions were observed, and `ModuleFailed` is the module runtime
failure outcome. Broken frontier, view, and lifecycle invariants fail fast.

## Tail and lifecycle

The terminal Slider Frontier is the Tape Tail/reclamation boundary. The Tape
loads that Frontier directly when checking capacity and when serving views.
Tail is not maintained as a second progress state.

For a tract without stages, omit `SetTailRef()`: the Tape defaults to observing
its own Head, so `Tail == Head`.

RecordTape lifecycle is one-shot:

```text
Constructed -> Open -> Closed
```

Failed initial configuration or allocation leaves Constructed. `open()` while
Open returns `AlreadyOpen`; `close()` is terminal and idempotent. Reopening
after a successful lifetime is a lifecycle violation and terminates. Open,
close, and all runtime roles must be coordinated; concurrent close with
publishing or processing is not supported.

## Drain and shutdown

The producer and stage executor must be joined before closing the Tape. All
borrowed views must be retired before the terminal Frontier is allowed to pass
their positions and before close. Core supplies no scheduler, wait strategy,
worker, or drain abstraction.
