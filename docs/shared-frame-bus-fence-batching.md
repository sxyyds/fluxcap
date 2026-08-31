# SharedFrameBus fence submission and deadline batching

## Status

The synchronous protocol continues to guarantee that a successful
`publish()` or `commit()` has submitted its ready-fence signal before the call
returns. Consumer `release()` likewise submits its done-fence signal before
returning. This guarantee is part of the current cross-process lifetime model.

The implementation now uses `ID3D11DeviceContext4::Flush1` with
`D3D11_CONTEXT_TYPE_ALL` instead of the older whole-context `Flush`. This is a
submission optimization, not batching, and does not change visibility or error
semantics.

## Measured result

One paired 320x320 run on the current RTX 5060 Laptop GPU used 2,000 measured
publishes per topology, 300 warmup publishes, and four AB/BA rounds:

| Submission | Staged publish/s | Direct publish/s | Direct event-query/s |
|---|---:|---:|---:|
| `Flush` | 25,158 | 23,631 | 23,600 |
| `Flush1(ALL)` | 24,862 | 24,453 | 24,421 |

The direct rate increased by about 3.5 percent in these two separate runs.
The staged difference is within normal run-to-run variance, so this is
directional evidence rather than a universal speedup claim.

The real WGC same-process A/B run remained source limited at about 60 distinct
presentations/s. Direct capture-to-consumer age changed from approximately
7.83/14.86 ms p50/p95 to 7.35/14.56 ms in two short samples. Longer runs are
required before attributing that difference to `Flush1`.

## Why synchronous commit cannot silently batch

A copy-publish producer currently performs these operations as one
transaction:

1. Queue the texture write.
2. Signal the ready fence.
3. Submit the context.
4. Publish metadata and slot/global sequences.
5. Clear the writer bit.
6. Return success.

For a direct write lease, step 1 is performed by the caller before
`commit()`. `commit()` owns steps 2 through 6 and consumes the active lease
once validation for that bus has begun.

Returning before step 3 would expose one of two invalid states. Publishing the
CPU sequence early lets a consumer wait on work that may remain indefinitely
buffered in another process. Publishing it late means `commit()` returned
success for a frame that is not yet acquirable. An asynchronous device removal
could then turn an already reported success into a failed publication.

Making synchronous `commit()` wait for a batch deadline does not help the WGC
producer. Frame-arrived callbacks are serialized in the common case, so the
first callback blocks the next frame that could have joined its batch. At 60
or 240 distinct presentations/s, a sub-millisecond deadline normally expires
with one frame and only adds latency.

## Explicit asynchronous design

Useful batching therefore needs a separate contract, not a flag that changes
the meaning of `commit()`:

```cpp
struct SharedFrameBusAsyncPublishConfig {
    std::uint32_t max_delay_us = 250;
    std::uint32_t max_batch_size = 4;
};

GpuError commit_async(
    SharedFrameBusWriteLease&&,
    const SharedFrameBusFrameMetadata&,
    SharedFrameBusPublishTicket&) noexcept;

GpuError flush_pending(std::uint32_t timeout_ms) noexcept;
GpuError wait_published(
    const SharedFrameBusPublishTicket&,
    std::uint32_t timeout_ms) noexcept;
```

`commit_async()` success would mean accepted, not cross-process visible. The
output parameter preserves the existing `GpuError` style and distinguishes an
invalid argument, allocation failure, or rejected lease from accepted pending
work. A ticket is complete only after the batch fence is submitted and its CPU
slot metadata is published. A wait timeout does not cancel the publication,
and destroying a ticket does not cancel it. Existing `commit()` would remain
synchronous and could internally perform `commit_async()` followed by a flush
and `wait_published()`.

`flush_pending()` snapshots the accepted sequence high-watermark on entry and
waits only through that point, so a concurrent producer cannot keep the call
alive indefinitely. The asynchronous configuration belongs in a new
`SharedFrameBusPublisher::create` overload rather than changing
`SharedFrameBusConfig` layout.

The producer-side batch state needs:

- a private pending record for each claimed slot;
- a reserved sequence for each record, not yet stored in the control page;
- the normalized metadata payload;
- a first-pending deadline and a maximum batch size;
- a completion result shared by every ticket in the batch;
- a submit worker or waitable timer that guarantees the final frame is flushed.

Sequence reservation must use one producer-private high-watermark shared by
synchronous and asynchronous paths; visible `global_sequence` is no longer a
safe allocator while pending records exist. Once a pending record owns a slot,
the producer may clear its private `writer_active` flag to claim another empty
slot, but the shared writer bit remains set. Slot-claim waits must either submit
pending work inline or release the publisher mutex before waiting so the submit
worker cannot be starved.

Pending slots keep the writer bit set. On batch submission the producer signals
the highest reserved sequence once, calls `Flush1`, checks device removal, then
publishes each metadata commit stamp and slot sequence, publishes the global
sequence, and clears the corresponding writer bits. If every usable slot is
pending, submission happens immediately rather than returning `no_slot`.

On signal or observed device-removal failure, all pending slots are cancelled,
every ticket receives the same terminal error, `shutting_down` is set, and no
pending metadata or sequence becomes visible. Producer destruction submits or
cancels pending work before closing shared handles. A process crash remains
covered by the existing publisher-process liveness handle.

`Flush1` returns no status, so submission failure cannot be reported directly;
the implementation can report `Signal` failure and device removal observed
after submission. Consumer registration, unregistration, and publisher
destruction must treat pending records as active protocol state, not only check
the current synchronous `writer_active` flag.

## Consumer done-fence batching

Done-fence batching is independently possible because a later monotonic signal
covers earlier released sequences. It still requires a deadline worker for the
last release. A queued signal is not completion: the shared reader bit must
remain set until the publisher observes that consumer's completed done-fence
value at or beyond the slot sequence. Slot claiming treats `ownership == 0` as
immediately reusable and does not check a done fence in that branch, so clearing
the bit when merely queuing the signal would permit an early texture overwrite.

An asynchronous release may clear only the consumer's private acquired state.
Its worker and publisher-side completion sweep retain responsibility for the
shared reader bit. Signal or device-removal failure must quarantine every slot
with a pending reader, and the API needs an explicit
`flush_releases()`/ticket contract. The current synchronous `release()`
therefore remains unchanged apart from `Flush1`.

## Acceptance gates

An asynchronous implementation should not become the WGC default until it
passes all of these checks:

- final-frame visibility within configured deadline plus 0.5 ms scheduler
  allowance at p99;
- no metadata/texture pairing failures under slot reuse and process churn;
- correct producer and consumer process-crash behavior;
- deterministic device-lost failure for every pending ticket;
- accepted-but-not-visible behavior before manual flush, followed by coherent
  cross-process texture and metadata visibility after flush;
- synchronous/asynchronous interleaving with strictly increasing reserved and
  visible sequences;
- ticket wait timeout without cancellation, dropped tickets, concurrent
  waiters, publisher destruction, and sequence exhaustion;
- registration/unregistration racing pending batches without deadlock or early
  slot reuse;
- deterministic `Signal` and device-removal fault injection, including
  quarantine of every pending-reader slot;
- no regression in fixed-ROI resize/unavailable recovery;
- lower CPU submission cost or at least 10 percent higher saturated publish
  rate on two adapters;
- no material p95/p99 real-WGC latency regression at 60, 120, and 240 Hz.

Until those gates are met, `Flush1` is the safe fixed-cost improvement and
deadline batching remains an explicit experimental protocol extension.
