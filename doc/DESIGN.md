# Record Tract Design

## Core topology

The current tract is a linear ordered chain:

```text
Head Frontier -> Slider A -> Slider B -> ... -> terminal Frontier == Tail
```

`RecordTape` owns a bounded preallocated ring and the producer publication
frontier. A Slider owns one processing Frontier. Downstream construction gets
only a read-only upstream Frontier:

```cpp
Slider a{tape, tape.GetFrontier(), module_a};
Slider b{tape, a.GetFrontier(), module_b};
```

Slider has no upstream object dependency or upstream accessor call. It reads
the upstream atomic directly in its processing path. The separately supplied
Tape provides record storage.

A Frontier value `N` means that its owner completed every position `< N` and
does not retain views of those positions. The terminal Frontier is the Tail
boundary observed by RecordTape. There is no second Tail progress state. With
no stages, the Tape observes its own Head and therefore `Tail == Head`.

Topology may be configured until the first successful `RecordTape::open()`.
The first successful open freezes topology permanently. Calling
`SetTailRef()` while the Tape is Open or after it has been Closed is a
lifecycle violation and terminates. A failed `open()` does not freeze topology.
The terminal Frontier must remain alive until the quiescent call to
`RecordTape::close()`. Closing the Tape clears the stored non-owning reference.
The terminal Frontier owner may be destroyed after `close()` returns. The tract
is linear; branching and DAG topology are outside this design.

## RecordTape

The nested `RecordTape::Buffer` owns one aligned allocation and maps a physical
slot to a byte span. It knows no ring policy. RecordTape owns Position-to-slot
mapping, Head/Tail observation, publication, capacity checks, and lifecycle.

Publication is ordered as follows:

1. The producer copies the payload into the Head slot.
2. It publishes the new Head with release semantics.
3. A reader acquires Head before exposing the payload.
4. The terminal stage publishes its Frontier with release semantics after
   processing and view retirement.
5. The producer acquires Tail before reusing a slot.

Absolute position validation precedes `position % capacity`, so a reclaimed
position cannot be mistaken for a replacement record after wraparound.

`try_view()` remains a direct borrowed-view API. It returns `Closed`,
`Reclaimed`, or `Unpublished` for direct callers. A correctly composed Slider
must not receive those results; a broken internal invariant fails fast.

## Slider

`Slider<Module>` is header-only and synchronously calls
`module.process(const RecordView&)` in position order. It publishes its own
Frontier only after successful module completion. `ModuleFailed` is a normal
module runtime outcome; broken frontier, view, and lifecycle conditions are not
modeled as statuses.

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

Any successfully processed remainder is published before `process()` returns.

Only one executor mutates a Slider. Its module and source Tape must remain
alive for the Slider lifetime.

## Persistence

Persistence is a specialized stage with this boundary:

```text
RecordTape / upstream Frontier -> Persistence -> PhysicalWalAdapter
```

It stores a read-only upstream Frontier and owns a durable Frontier. Its source
Tape must be open before `Persistence::open()`. The WAL payload size is derived
from `RecordTape::payload_size()`; physical WAL alignment, identity, and file
geometry remain the responsibility of the WAL layer.

For each non-empty batch Persistence:

1. reads records through the source Tape;
2. appends them to the physical WAL;
3. synchronizes the complete batch;
4. publishes the durable Frontier only after sync succeeds.

Append failure, sync failure, and physical sequence exhaustion set sticky
terminal `failed_`. Failed is absorbing: each later `process()` returns
`ModuleFailed`, performs no append or sync, and leaves the durable Frontier
unchanged. Lifecycle misuse and broken tract invariants do not set `failed_`.

`PersistenceCore` contains the non-template mechanics and
`PhysicalWalAdapter` owns the native file handle. Persistence close is terminal
for the writer lifetime; reopening is not part of the contract. Processing and
close require quiescent lifecycle roles.

## Lifecycle

RecordTape has a one-shot lifecycle:

```text
Constructed -> Open -> Closed
```

Failed initial configuration or allocation leaves Constructed. A successful
open starts the only lifetime. `open()` while Open returns `AlreadyOpen`; after
Close it terminates. `close()` is idempotent but terminal. The same quiescent
lifecycle rule applies to Persistence processing and close.
