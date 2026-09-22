# Record Tract Invariants

## Topology

The supported topology is a linear ordered chain:

```text
Head Frontier -> Stage Frontier -> ... -> terminal Frontier == Tail
```

There is no branching, DAG, topology registry, or separate Tail publication
atomic.

Topology wiring is completed before `RecordTape::open()` and is immutable during
runtime. `SetTailRef()` after open is a programmer/lifecycle error. Every
stored Frontier reference must outlive the object observing it.

## RecordTape boundaries

`RecordTape` owns a bounded preallocated ring with these boundaries:

```text
tail <= head
head - tail <= capacity
```

- Head is published only by the single producer.
- `[tail, head)` contains published retained positions.
- `tail` is an acquire observation of the terminal stage Frontier.
- Producer reuse is allowed only after the terminal Frontier has passed the
  old position.
- A zero-stage Tape observes its own Head, so `Tail == Head`.

`RecordTape` maps an absolute Position to `position % capacity` only after
checking the absolute range. The nested `Buffer` owns aligned allocation and
slot layout; it does not own ring progress.

## Frontier ownership

Each stage owns exactly one mutable Frontier and exposes only a read-only view
to downstream composition. A Frontier value `N` means:

```text
all positions < N are complete for that owner
the owner retains no views/references to those positions
```

The Slider owns its own Frontier and receives only `const Frontier&` upstream.
Persistence owns its durable Frontier and receives only `const Frontier&`
upstream. No stage obtains upstream progress through an upstream object.

For a stage:

```text
current_frontier <= upstream_frontier <= head
```

It may process only `[current_frontier, upstream_frontier)`. A regression or a
failed view in a correctly composed tract is a broken invariant and fails fast.

## Publication ordering

For each producer/stage handoff:

1. producer writes the payload;
2. producer publishes Head with release semantics;
3. stage acquires the upstream Frontier before using the record;
4. stage completes its work and publishes its Frontier with release semantics;
5. downstream stage acquires that Frontier;
6. terminal Frontier publication is acquired by the producer before slot reuse.

No `seq_cst` operation is required. These guarantees assume one producer per
Tape, one runtime executor per Slider/Persistence, and quiescent lifecycle
operations.

## Borrowed views

`RecordTape::try_view()` is a direct non-owning access API. `ViewStatus` remains
valid for direct callers:

- `Closed` — Tape is not open;
- `Reclaimed` — position is below Tail;
- `Unpublished` — position is at or beyond Head;
- `Ok` — position is retained and published.

The returned span does not pin a slot or register a reader. A caller must keep
the terminal Frontier from passing the position while the view is in use. A
Slider or Persistence receiving `Closed`, `Reclaimed`, or `Unpublished` means
the tract invariant was broken; those stages terminate instead of returning a
runtime status.

## Slider policy and status

`ExecutionPolicy::read_count` and `publish_count` must be greater than zero.
They are bootstrap preconditions, not normalization inputs.

`read_count` specifies the maximum number of consecutive records in one
processing portion. A single `process()` call processes as many portions as
needed to cover the range observed at the beginning of the call. Therefore,
`read_count` does not limit the total number of records processed by one call.

`publish_count` specifies how many successfully processed records may
accumulate before the Slider publishes advancement of its Frontier.
Processing-portion boundaries do not cause Frontier publication. Publication is
controlled solely by `publish_count`. Any successfully processed remainder is
published before `process()` returns.

A module must complete a position before the Slider publishes the next
exclusive Frontier value.

Valid Slider outcomes are:

- `Processed` — progress was published;
- `Empty` — no new upstream progress was observed;
- `ModuleFailed` — the module returned failure.

Broken frontier, view, and lifecycle invariants are not runtime outcomes.

## Persistence durability

```text
RecordTape / upstream Frontier -> Persistence -> PhysicalWalAdapter
```

The source Tape is open before Persistence bootstrap. Payload size comes from
the Tape. Physical WAL geometry remains owned by the WAL configuration and
adapter.

For every non-empty batch:

```text
append all records -> sync -> publish durable Frontier
```

Append failure, sync failure, and physical sequence exhaustion set sticky
terminal `failed_`. While Failed:

- every `process()` returns `ModuleFailed`;
- no append or sync occurs;
- durable Frontier does not advance;
- pending upstream work is irrelevant.

Lifecycle misuse and broken tract invariants do not set `failed_`.

## Lifecycle

RecordTape lifecycle:

```text
Constructed -> Open -> Closed
```

- failed initial configuration/allocation leaves Constructed;
- successful `open()` starts the only lifetime;
- `open()` while Open returns `AlreadyOpen`;
- `close()` is terminal and idempotent;
- `open()` after Close terminates;
- `open()` and `close()` are quiescent operations.

Persistence processing and close likewise require quiescent roles. Persistence
reopen is not part of the current contract.
