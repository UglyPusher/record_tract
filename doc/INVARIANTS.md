# Record Tract Invariants

## RecordTape Boundary Order

`RecordTape` intrinsically owns only two monotonic absolute zero-based
boundaries:

```text
tail <= head
head - tail <= capacity
```

- `[tail, head)` contains published retained positions.
- `[head, tail + capacity)` is free capacity.
- Only the producer writes `head`.
- Only the composition/reclaimer writes `tail`.
- `reclaim(end)` cannot move backward or beyond observed `head`.
- The composition advances `tail` only after every mandatory reader has
  finished the reclaimed positions.

`RecordTape` contains no `durable` frontier, file writer, or persistence failure
state.

## Processing Frontier Order

A slider's own Frontier is its single authoritative current exclusive end:

```text
own frontier <= observed upstream frontier
```

- The upstream reference is const and therefore read-only.
- Only the slider holding its own non-const frontier reference publishes it.
- A range must begin at the own Frontier value acquired at entry and end no
  later than the acquired upstream frontier.
- A record view is obtained by its absolute zero-based position.
- The module must complete position `p` successfully before the own Frontier
  becomes `p + 1`.
- Own frontier publication occurs only after successful module completion.
- Release publication of exclusive end `p + 1` makes module output for
  position `p` visible to an acquire-reading downstream stage.
- The composition may reclaim only through progress that all mandatory stages
  protecting retention have completed and only after all borrowed views have
  been retired.

The slider mechanics allocate no storage, copy no payload, interpret no record
kind, and perform no persistence, snapshot I/O, waiting, or scheduling.

For the bare linear composition:

```text
tail <= NoOpF <= head
```

Publication of `NoOpF` certifies module completion but does not reclaim a
position. Only the composition writes `tail`, after the corresponding slider
call has returned and its borrowed views are retired. Stopping either slider
execution or composition reclamation therefore preserves bounded backpressure.

For a linear chain of processing stages, the ordering generalizes to:

```text
tail <= Fn <= ... <= F2 <= F1 <= head
```

The number of stages is not part of the tract contract. A composition may also
have multiple stages consuming from the same upstream boundary; each processing
frontier remains independently owned by its publishing stage.

## Core Ownership

- A free block is owned by the producer while it fills the payload.
- Publication of `head` makes the completed payload immutable and visible to
  coordinated readers.
- Processing stages borrow immutable `RecordView` values without owning the
  underlying slot.
- A processing frontier certifies completion by its stage; publishing that
  frontier does not reclaim the block.
- Publication of `tail` releases the block for producer reuse.

A block cannot be overwritten until `tail` passes its previous absolute
position.

Borrowed view readers do not own or advance `tail`. Before obtaining a view and
throughout its use, they must ensure reclamation cannot pass its position. The
view neither pins the slot nor survives close/destruction/reopen. Absolute range
validation precedes slot mapping, preventing a reclaimed position from being
interpreted as the new record in a reused slot under this contract.

## Core Publication Order

For one processing dependency:

```text
producer --head--> stage --frontier--> downstream stage
```

- Payload copy happens before `head.store(..., release)`.
- Retained view access acquires `head` before exposing payload bytes.
- A stage observes its upstream boundary before processing available records.
- Module completion for position `p` happens before publication of frontier
  `p + 1`.
- A downstream stage acquires its upstream frontier before relying on the
  corresponding module output.
- Reclamation happens only after the composition has established that all
  mandatory users of the reclaimed positions are finished.
- Producer acquires `tail` before reusing capacity.

No frontier operation uses `seq_cst`.

## Persistence Composition Invariants

Persistence is a specialized stage. In a composition where
`PersistenceSlider` consumes directly from `head`, its frontier may be named
`durable`:

```text
tail <= durable <= head
head - tail <= capacity
```

- `[durable, head)` is published to the tract but not yet certified durable.
- `[tail, head)` remains accessible through read-only `try_view()` under the
  caller-owned retention contract; accessibility does not prove durability.
- Position `p` maps to block `p % capacity` only after absolute range
  validation.
- Only `PersistenceSlider` publishes the `durable` frontier.
- Append and physical sync of the complete selected batch happen before
  publication of its batch-end durable frontier.
- A downstream stage that requires durability must acquire and obey the
  durable frontier before processing a position.

`durable` is the semantic name of this persistence stage's frontier. It is not
an intrinsic `RecordTape` boundary.

## Memory

- Storage contains exactly `capacity` fixed-stride blocks.
- Every block address satisfies configured `alignment`.
- The complete allocation is zeroed during `open()` to commit and touch every
  page before role threads start.
- No allocation occurs in publish, slider processing, persistence advance, or
  position view.

## Failure

For a generic slider:

- module failure at position `p` does not publish `p + 1`;
- the own frontier remains at the first uncompleted position;
- the composition decides whether and when to retry or stop.

For persistence:

- empty durability batches do not append or synchronize;
- a failed append or sync does not move `durable`;
- retained views remain independent of durability and do not grant downstream
  permission;
- a failed persistence module prevents further durable progress.

`PersistenceModule` owns its terminal failure state; `RecordTape` remains
unaware of it. A composition with mandatory persistence must stop production or
otherwise preserve bounded safety after observing that failure.

## Post-Crash WAL Prefix

- The maximal contiguous CRC-valid prefix is the authoritative recovered WAL.
- Every complete record in that prefix participates in replay and rebuild.
- Client acknowledgement state does not change the recovered prefix.
- Batches and the runtime `durable` frontier are not persisted as separate
  commit metadata.
- The physical format has no batch commit record or commit marker.
