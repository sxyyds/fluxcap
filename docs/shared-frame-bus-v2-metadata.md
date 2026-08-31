# SharedFrameBus v2 per-slot metadata design

Status: implementation proposal. This note does not change the current v1
implementation.

## Goals

SharedFrameBus v2 carries the provenance of every published texture in the
same slot as the texture sequence. The first producer is WGC direct-to-bus and
needs these fields:

- WGC `Direct3D11CaptureFrame::SystemRelativeTime` in 100 ns units;
- the FluxCap publication QPC value and its frequency;
- source width and height;
- ROI origin and size;
- WGC mailbox generation;
- the color-space interpretation of the pixels in the bus texture.

Metadata must remain paired with the texture during slot reuse, latest-wins
acquisition, consumer crashes, and failed or cancelled publishes. Adding it
must not add a GPU copy, another shared handle, a wait in the WGC callback, or
another `Signal`/`Flush`.

## Compatibility policy

`SharedFrameBusRegistration` keeps its existing field order and size. Its
default `protocol_version` becomes 2. The control mapping handle, process
handle, two fence handles, and legacy texture identifiers keep their existing
meaning.

The v2 control page has the complete v1 page as an offset-zero prefix and
appends a metadata record for each of the eight possible slots. A new consumer
accepts both protocol versions:

- v2: map and validate the v2 page, then expose per-slot metadata;
- v1: map and validate the exact v1 page, then return metadata with
  `valid_fields == 0`;
- any other version, or a registration/page version mismatch: reject before
  opening textures or fences.

An old v1 consumer rejects a v2 registration because its version is unknown.
It must not attempt to interpret the extended page. The publisher-side attach
workflow must unregister a registration if the remote open handshake fails,
as it already must for any remote initialization failure.

This is a new bus epoch, not an in-place upgrade. Existing v1 buses and leases
continue until closed. A v2 publisher never changes a live control page from
v1 to v2.

Keeping the registration layout unchanged avoids accidental IPC over-read and
preserves the public class layouts. Clean version rejection is preferable to
making a v1 peer silently accept metadata it cannot validate.

## Public API

Add public constants for the current bus and metadata versions and validity
bits. `valid_fields` is 64-bit so later schemas can add fields without
repurposing existing bits.

```cpp
inline constexpr std::uint32_t shared_frame_bus_protocol_version = 2;
inline constexpr std::uint32_t shared_frame_bus_metadata_version = 1;

inline constexpr std::uint64_t shared_frame_bus_metadata_source_timestamp = 1ull << 0;
inline constexpr std::uint64_t shared_frame_bus_metadata_qpc = 1ull << 1;
inline constexpr std::uint64_t shared_frame_bus_metadata_source_dimensions = 1ull << 2;
inline constexpr std::uint64_t shared_frame_bus_metadata_roi = 1ull << 3;
inline constexpr std::uint64_t shared_frame_bus_metadata_mailbox_generation = 1ull << 4;
inline constexpr std::uint64_t shared_frame_bus_metadata_color_space = 1ull << 5;

struct SharedFrameBusFrameMetadata final {
    std::uint32_t structure_size = sizeof(SharedFrameBusFrameMetadata);
    std::uint32_t metadata_version = shared_frame_bus_metadata_version;
    std::uint64_t valid_fields = 0;
    std::int64_t source_timestamp_100ns = 0;
    std::uint64_t timestamp_qpc = 0;
    std::uint64_t qpc_frequency = 0;
    std::uint64_t mailbox_generation = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t roi_x = 0;
    std::uint32_t roi_y = 0;
    std::uint32_t roi_width = 0;
    std::uint32_t roi_height = 0;
    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_CUSTOM;
    std::uint32_t reserved_0 = 0;
    std::array<std::uint64_t, 2> reserved{};
};
```

The intended size is 96 bytes on the supported Windows ABI. Add standard
layout, trivial-copy, size, alignment, and field-offset assertions. All
reserved fields must be zero when passed to a publisher.

`color_space` describes how to interpret the pixels in the bus texture. It is
not a claim about the monitor's native gamut. The current WGC BGRA8 frame-pool
path declares `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`; a future HDR/scRGB
frame-pool mode must publish its own explicit value.

Preserve every existing method and add overloads:

```cpp
GpuError SharedFrameBusPublisher::publish(
    ID3D11Texture2D*, const SharedFrameBusFrameMetadata&,
    std::uint32_t timeout_ms = 0) noexcept;

GpuError SharedFrameBusPublisher::commit(
    SharedFrameBusWriteLease&&,
    const SharedFrameBusFrameMetadata&) noexcept;

SharedFrameBusFrameMetadata
SharedFrameBusFrameLease::metadata() const noexcept;
```

The existing `publish(texture, timeout)` and `commit(lease)` publish a fresh
metadata object with `valid_fields == 0`. They must overwrite the slot's old
metadata so a generic publish cannot inherit WGC provenance from an earlier
use of that slot.

Return metadata by value and keep the acquired copy in
`SharedFrameBusConsumerState`. This adds no field to the public frame-lease
class and therefore does not change its object layout. A default object is
returned for an empty or stale lease.

The copy-publish overload validates metadata before claiming a slot. The
direct-publish overload receives metadata at `commit()`, after
`begin_publish()` has necessarily claimed a slot, but validates it before a
ready-fence signal is queued:

- exact known `structure_size` and `metadata_version`;
- no unknown validity bits and all reserved values zero;
- QPC validity requires nonzero `timestamp_qpc` and `qpc_frequency`;
- source-dimension validity requires nonzero dimensions;
- ROI validity requires source dimensions, nonzero ROI dimensions, ROI bounds
  inside the source, and ROI width/height equal to the fixed bus texture size;
- mailbox-generation validity requires a nonzero generation;
- color-space validity rejects `DXGI_COLOR_SPACE_RESERVED` and
  `DXGI_COLOR_SPACE_CUSTOM`.

Invalid fields do not advance sequence and do not increment
`published_frames`. Copy-publish rejects them before `publish_attempts` and
before claiming a slot, matching the existing invalid-source rule. A direct
commit with invalid metadata cancels and consumes its already active write
lease; its earlier successful `begin_publish()` remains a `publish_attempts`
entry, just as other failures after begin do. The slot must immediately be
claimable again.

## Shared control layout

Keep the current page definition verbatim as `BusControlPageV1`. Define v2 as
an exact prefix extension:

```cpp
struct alignas(64) BusSlotMetadataV2 final {
    volatile LONG64 sequence;
    SharedFrameBusFrameMetadata payload;
};

struct alignas(64) BusControlPageV2 final {
    BusControlPageV1 common;
    std::array<BusSlotMetadataV2, shared_frame_bus_max_slots> metadata;
};
```

`BusControlPageV1` is currently 384 bytes. Each metadata record rounds to 128
bytes, and `BusControlPageV2` is 1408 bytes, remaining well below one 4 KiB
page. Lock these values and the offset-zero prefix with static assertions.

The duplicate `BusSlotMetadataV2::sequence` is a commit stamp, not a second
global sequence. It lets a consumer reject malformed or partially initialized
metadata instead of returning a valid texture with unrelated provenance.

The registration's `structure_size` remains unchanged. Its version selects
the mapped page size. The page header's `structure_size` must be exactly
`sizeof(BusControlPageV1)` or `sizeof(BusControlPageV2)` for the selected
version. Never map `sizeof(BusControlPageV2)` merely to probe a v1 mapping.

## Publication and memory ordering

The existing writer bit and reader bits are the CPU ownership protocol. No
new lock or GPU primitive is required.

When a writer claims a slot it sets both the existing slot sequence and the v2
metadata commit stamp to zero while holding the writer bit. On commit:

1. Validate and normalize metadata in publisher-private memory.
2. Queue the texture-producing GPU commands as today.
3. Queue `ready_fence->Signal(next)` and flush as today; abort on failure.
4. Copy the complete metadata payload into the writer-owned slot.
5. Publish the metadata commit stamp with `InterlockedExchange64(next)`.
6. Publish the existing slot sequence, then the global sequence, with the
   existing interlocked stores.
7. Clear the writer bit with `InterlockedExchange`, making the slot acquirable.

Windows interlocked operations are the cross-process full barriers around the
plain metadata copy. The payload and commit stamp become visible before the
writer bit is cleared. A cancelled or failed commit leaves slot sequence zero
and never exposes its metadata.

A consumer first sets its reader bit with the existing CAS, rechecks the slot
sequence, then verifies that the metadata commit stamp equals that stable
sequence. While its reader bit is set, the publisher cannot overwrite either
the texture or metadata. The consumer copies and validates metadata before
queueing the ready-fence wait. A stamp mismatch or malformed payload is a
protocol error; clear the reader bit, quarantine that slot, and require bus
recreation rather than retrying potentially corrupt data.

The ready fence still orders GPU texture writes. It does not order the CPU
mapping; CPU visibility comes from the ownership interlocked operations. The
shared sequence pairs the two domains.

## WGC integration

In the bus-only branch, build metadata after resolving the mailbox and before
the single bus commit:

```cpp
SharedFrameBusFrameMetadata metadata;
metadata.valid_fields =
    shared_frame_bus_metadata_source_timestamp |
    shared_frame_bus_metadata_qpc |
    shared_frame_bus_metadata_source_dimensions |
    shared_frame_bus_metadata_roi |
    shared_frame_bus_metadata_mailbox_generation |
    shared_frame_bus_metadata_color_space;
metadata.source_timestamp_100ns = newest.SystemRelativeTime().count();
metadata.timestamp_qpc = publication_qpc;
metadata.qpc_frequency = qpc_frequency;
metadata.mailbox_generation = mailbox.generation;
metadata.source_width = mailbox.source_width;
metadata.source_height = mailbox.source_height;
metadata.roi_x = mailbox.x;
metadata.roi_y = mailbox.y;
metadata.roi_width = mailbox.width;
metadata.roi_height = mailbox.height;
metadata.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
```

`timestamp_qpc` should retain the current private-ring meaning: sample QPC
after `CopySubresourceRegion` has been queued and immediately before commit.
Use the same sampling helper for private-ring and bus-only publication so the
two paths remain comparable.

Change the internal reserved-producer commit helper to accept the metadata by
const reference. The WGC callback still calls `begin_publish(0, ...)`; a busy
bus mutex or unavailable slot remains an immediate drop. No metadata is
published on `no_slot`, resize-transition, `region_unavailable`, copy failure,
commit failure, or device loss.

Same-size mailbox reconfiguration publishes the incremented generation and
new ROI origin on the first committed frame from that configuration. A
320-to-640 change still requires a new bus epoch. Source shrink publishes
nothing; recovery on the same fixed ROI resumes with the same generation and
new current source dimensions.

## Test plan

### Protocol and API tests

- Assert the registration's existing size and offsets do not change, and
  assert both page layouts and the 96-byte public metadata layout.
- Verify a new consumer opens a v1 fixture and returns `valid_fields == 0`.
- Verify v1/v2 registration-page mismatches, unknown versions, wrong page
  sizes, bad metadata versions, unknown flags, nonzero reserved fields,
  invalid ROI bounds, invalid QPC pairs, and invalid color spaces fail cleanly.
- Verify failed copy-publish validation leaves sequence, stats, and slot
  availability unchanged. Verify failed direct-commit validation leaves
  sequence and published counters unchanged, consumes the lease, and makes its
  slot immediately available; the begin attempt remains counted.
- Verify both metadata overloads, plus the two legacy overloads that must
  clear metadata.
- Cancel a direct write lease with metadata prepared by the caller, reuse the
  slot, and prove no cancelled metadata becomes visible.

### Pairing and concurrency tests

- Extend the real child-process consumer response to return metadata. Publish
  alternating sentinel pixel patterns and metadata through copy and direct
  paths; every acquired texture marker, metadata sentinel, and bus sequence
  must agree despite latest-wins skips.
- Run a high-count slot-reuse loop and reject any mixed-field sentinel. This
  catches missing barriers and torn payload copies.
- Hold a slow consumer lease while a fast consumer advances. Revalidate both
  the held pixels and held metadata after many slot reuses.
- Repeat consumer crash/quarantine and consumer-index reuse tests with
  metadata validation so a quarantined slot cannot leak stale provenance.

### WGC tests

- For the existing centered 320 test, validate source `801x799`, ROI
  `(240,239,320,320)`, nonzero source timestamp/QPC/frequency, mailbox
  generation, and the declared BGRA8 color space in the real child process.
- After the existing same-size absolute reconfiguration, validate ROI
  `(17,23,320,320)` and generation increment on the first newer sequence.
- For the existing absolute 640 shrink/recovery test, prove the unavailable
  period advances neither sequence nor metadata and recovery preserves the
  generation while publishing restored source dimensions.
- In the two-slot no-buffer test, prove dropped WGC callbacks do not alter the
  last committed metadata and that publication resumes with a coherent newer
  record after release.
- Check source timestamps and QPC values are monotonic across deliberately
  generated presentations, without equating repeated processing rate to WGC
  source FPS.

Run all protocol tests against static and shared Release builds. The WGC tests
require an interactive desktop and should be rerun unrestricted for both 320
and 640 modes.

## Implementation map in the current tree

- Public registration/frame info and bus classes:
  `include/fluxcap/gpu.hpp`, around lines 189-348.
- v1 page and slot controls: `src/gpu/shared_frame_bus.cpp`, around lines
  21-65.
- slot claim and current commit publication order: the same file, around
  lines 492-613.
- registration production/validation: the same file, around lines 770-958 and
  1118-1428.
- consumer latest acquisition and reader-bit stabilization: the same file,
  around lines 1479-1603.
- internal WGC reserved-producer helpers: `src/gpu/shared_frame_bus.hpp`, lines
  7-22, and `src/gpu/shared_frame_bus.cpp`, around lines 1041-1116.
- WGC bus-only callback branch: `src/gpu/wgc_capture.cpp`, around lines
  685-783. The private-ring timestamp/mailbox assignment is around lines
  804-839.
- protocol tests: `tests/shared_frame_bus_tests.cpp`, especially around lines
  979-1041 and 1210-1341.
- WGC direct-bus tests: `tests/gpu_capture_tests.cpp`, around lines 1284-1714.
