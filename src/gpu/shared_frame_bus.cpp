#include "shared_frame_bus.hpp"

#include <d3d10.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t bus_magic = 0x53424346U; // FCBS
constexpr std::uint32_t bus_protocol_version_v1 = 1;
constexpr std::uint32_t bus_protocol_version_v2 = 2;
constexpr std::uint32_t bus_protocol_version_v3 = 3;
constexpr std::uint32_t bus_protocol_version_v4 = 4;
constexpr std::uint32_t bus_protocol_version_v5 = 5;
static_assert(shared_frame_bus_protocol_version == bus_protocol_version_v5);
constexpr LONG writer_bit = static_cast<LONG>(0x80000000UL);
constexpr LONG quarantine_bit = 0x40000000L;
constexpr std::uint64_t access_ref_mask = 0x000000000000ffffULL;
constexpr std::uint64_t access_open_bit = 0x0000000000010000ULL;
constexpr std::uint32_t access_tag_shift = 17;
constexpr std::uint64_t access_tag_value_mask =
    std::numeric_limits<std::uint64_t>::max() >> access_tag_shift;
constexpr std::uint64_t access_tag_mask =
    std::numeric_limits<std::uint64_t>::max() & ~((1ULL << access_tag_shift) - 1ULL);
constexpr std::uint32_t invalid_slot = std::numeric_limits<std::uint32_t>::max();

struct alignas(16) BusSlotControl final {
    volatile LONG64 sequence;
    volatile LONG ownership;
    LONG reserved;
};

struct alignas(8) BusConsumerControl final {
    volatile LONG64 token;
    volatile LONG64 access_state;
    volatile LONG process_id;
    LONG reserved;
};

struct alignas(64) BusControlPageV1 final {
    std::uint32_t magic;
    std::uint32_t protocol_version;
    std::uint32_t structure_size;
    std::uint32_t slot_count;
    std::uint32_t max_consumers;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
    std::uint32_t adapter_luid_low;
    std::int32_t adapter_luid_high;
    volatile LONG shutting_down;
    LONG reserved;
    alignas(8) volatile LONG64 sequence;
    std::array<BusSlotControl, shared_frame_bus_max_slots> slots;
    std::array<BusConsumerControl, shared_frame_bus_max_consumers> consumers;
};

struct alignas(64) BusSlotMetadataV2 final {
    volatile LONG64 sequence;
    SharedFrameBusFrameMetadata payload;
    std::array<std::uint8_t, 24> reserved;
};

struct alignas(64) BusControlPageV2 final {
    BusControlPageV1 common;
    std::array<BusSlotMetadataV2, shared_frame_bus_max_slots> metadata;
};

struct alignas(64) BusSlotSideDataV3 final {
    volatile LONG64 sequence;
    SharedFrameBusFrameSideData payload;
};

struct alignas(64) BusCursorShapeSlotV3 final {
    // Odd while the publisher writes; even and non-zero when committed.
    volatile LONG64 commit_sequence;
    std::uint64_t shape_sequence;
    std::uint32_t kind;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t hotspot_x;
    std::uint32_t hotspot_y;
    std::uint32_t stride_bytes;
    std::uint32_t data_size;
    std::uint32_t reserved;
    std::array<std::uint8_t, shared_frame_bus_max_cursor_shape_bytes> data;
};

struct alignas(64) BusControlPageV3 final {
    BusControlPageV2 v2;
    std::array<BusSlotSideDataV3, shared_frame_bus_max_slots> side_data;
    std::array<BusCursorShapeSlotV3, 2> cursor_shapes;
};

struct alignas(64) BusMoveResultSlotV4 final {
    // Odd while the publisher writes; even and non-zero when committed.
    volatile LONG64 commit_sequence;
    SharedFrameBusMoveResult payload;
    std::array<std::uint8_t, 56> reserved;
};

struct alignas(64) BusControlPageV4 final {
    BusControlPageV3 v3;
    std::array<BusMoveResultSlotV4,
        shared_frame_bus_move_result_slots> move_results;
};

static_assert(sizeof(BusSlotControl) == 16);
static_assert(sizeof(BusConsumerControl) == 24);
static_assert(sizeof(BusControlPageV1) == 384);
static_assert(sizeof(BusSlotMetadataV2) == 128);
static_assert(sizeof(BusControlPageV2) == 1408);
static_assert(offsetof(BusControlPageV2, common) == 0);
static_assert(offsetof(BusControlPageV2, metadata) == sizeof(BusControlPageV1));
static_assert(sizeof(BusControlPageV2) <= 4096);
static_assert(offsetof(BusControlPageV3, v2) == 0);
static_assert(offsetof(BusControlPageV3, side_data) == sizeof(BusControlPageV2));
static_assert(sizeof(BusSlotSideDataV3) % 64 == 0);
static_assert(sizeof(BusCursorShapeSlotV3) % 64 == 0);
static_assert(sizeof(BusControlPageV3) <= std::numeric_limits<DWORD>::max());
static_assert(offsetof(BusControlPageV4, v3) == 0);
static_assert(offsetof(BusControlPageV4, move_results)
    == sizeof(BusControlPageV3));
static_assert(sizeof(BusMoveResultSlotV4) % 64 == 0);
static_assert(sizeof(BusControlPageV4) <= std::numeric_limits<DWORD>::max());

GpuError make_error(GpuStatus status, HRESULT hr, const char* message) noexcept {
    GpuError result;
    result.status = status;
    result.hresult = hr;
    if (message != nullptr) {
        (void)strncpy_s(result.message.data(), result.message.size(), message, _TRUNCATE);
    }
    return result;
}

GpuError success() noexcept {
    return make_error(GpuStatus::ok, S_OK, "ok");
}

constexpr std::uint64_t known_metadata_fields =
    shared_frame_bus_metadata_source_timestamp
    | shared_frame_bus_metadata_qpc
    | shared_frame_bus_metadata_source_dimensions
    | shared_frame_bus_metadata_roi
    | shared_frame_bus_metadata_mailbox_generation
    | shared_frame_bus_metadata_color_space;

GpuError normalize_metadata(
    const SharedFrameBusFrameMetadata& input,
    std::uint32_t bus_width,
    std::uint32_t bus_height,
    SharedFrameBusFrameMetadata& output,
    DXGI_COLOR_SPACE_TYPE required_color_space =
        DXGI_COLOR_SPACE_CUSTOM,
    bool scaled_source_roi_allowed = false) noexcept {
    if (input.structure_size != sizeof(SharedFrameBusFrameMetadata)
        || input.metadata_version != shared_frame_bus_metadata_version
        || (input.valid_fields & ~known_metadata_fields) != 0
        || input.reserved_0 != 0
        || input.reserved[0] != 0
        || input.reserved[1] != 0) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "invalid shared frame bus metadata header or reserved fields");
    }

    output = {};
    output.valid_fields = input.valid_fields;
    if ((input.valid_fields & shared_frame_bus_metadata_source_timestamp) != 0) {
        output.source_timestamp_100ns = input.source_timestamp_100ns;
    }
    if ((input.valid_fields & shared_frame_bus_metadata_qpc) != 0) {
        if (input.timestamp_qpc == 0 || input.qpc_frequency == 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "shared frame bus QPC metadata is incomplete");
        }
        output.timestamp_qpc = input.timestamp_qpc;
        output.qpc_frequency = input.qpc_frequency;
    }
    if ((input.valid_fields & shared_frame_bus_metadata_source_dimensions) != 0) {
        if (input.source_width == 0 || input.source_height == 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "shared frame bus source dimensions are empty");
        }
        output.source_width = input.source_width;
        output.source_height = input.source_height;
    }
    if ((input.valid_fields & shared_frame_bus_metadata_roi) != 0) {
        if ((input.valid_fields & shared_frame_bus_metadata_source_dimensions) == 0
            || input.roi_width == 0
            || input.roi_height == 0
            || (!scaled_source_roi_allowed
                && (input.roi_width != bus_width
                    || input.roi_height != bus_height))
            || input.roi_width > input.source_width
            || input.roi_height > input.source_height
            || input.roi_x > input.source_width - input.roi_width
            || input.roi_y > input.source_height - input.roi_height) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "shared frame bus ROI metadata is out of bounds");
        }
        output.roi_x = input.roi_x;
        output.roi_y = input.roi_y;
        output.roi_width = input.roi_width;
        output.roi_height = input.roi_height;
    }
    if ((input.valid_fields & shared_frame_bus_metadata_mailbox_generation) != 0) {
        if (input.mailbox_generation == 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "shared frame bus mailbox generation is zero");
        }
        output.mailbox_generation = input.mailbox_generation;
    }
    if ((input.valid_fields & shared_frame_bus_metadata_color_space) != 0) {
        if (input.color_space == DXGI_COLOR_SPACE_CUSTOM
            || input.color_space == DXGI_COLOR_SPACE_RESERVED) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "shared frame bus color space is unspecified");
        }
        if (required_color_space != DXGI_COLOR_SPACE_CUSTOM
            && input.color_space != required_color_space) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "shared frame bus color space conflicts with its configured contract");
        }
        output.color_space = input.color_space;
    }
    return success();
}

constexpr std::uint32_t known_damage_flags_v4 =
    wgc_damage_valid
    | wgc_damage_native
    | wgc_damage_full_frame
    | wgc_damage_overflow
    | wgc_damage_discontinuity
    | wgc_damage_native_move_unavailable
    | wgc_damage_inferred_move
    | wgc_damage_move_pending;

constexpr std::uint32_t known_damage_flags_v5 =
    known_damage_flags_v4 | wgc_damage_native_move_available;

constexpr std::uint32_t known_cursor_flags =
    wgc_cursor_visible
    | wgc_cursor_position_valid
    | wgc_cursor_position_estimated
    | wgc_cursor_shape_pending;

bool valid_bus_rect(
    const WgcRect& rect,
    std::uint32_t bus_width,
    std::uint32_t bus_height) noexcept {
    if (rect.x < 0 || rect.y < 0 || rect.width == 0 || rect.height == 0) {
        return false;
    }
    const auto right = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(rect.x)) + rect.width;
    const auto bottom = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(rect.y)) + rect.height;
    return right <= bus_width && bottom <= bus_height;
}

GpuError normalize_side_data(
    const SharedFrameBusFrameSideData& input,
    std::uint32_t bus_width,
    std::uint32_t bus_height,
    std::uint32_t protocol_version,
    std::uint64_t frame_sequence,
    SharedFrameBusFrameSideData& output) noexcept {
    const bool v5_damage = protocol_version >= bus_protocol_version_v5;
    const std::uint32_t known_damage_flags = v5_damage
        ? known_damage_flags_v5
        : known_damage_flags_v4;
    if (input.structure_size != sizeof(SharedFrameBusFrameSideData)
        || input.side_data_version != shared_frame_bus_side_data_version
        || input.reserved[0] != 0
        || input.damage.reserved != 0
        || input.cursor.reserved != 0
        || ((input.epoch == 0) != (input.epoch_nonce == 0))
        || (input.damage.flags & ~known_damage_flags) != 0
        || (input.cursor.flags & ~known_cursor_flags) != 0
        || input.damage.dirty_count > wgc_max_dirty_rects
        || input.damage.move_count > wgc_max_move_rects) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "invalid shared frame bus side-data header or reserved fields");
    }

    const WgcFrameDamage& damage = input.damage;
    if ((damage.flags & wgc_damage_valid) == 0) {
        if (damage.base_sequence != 0
            || damage.dirty_count != 0
            || damage.move_count != 0
            || damage.flags != 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "invalid inactive shared frame damage payload");
        }
    } else {
        const bool native_move_available =
            (damage.flags & wgc_damage_native_move_available) != 0;
        const bool inferred_move =
            (damage.flags & wgc_damage_inferred_move) != 0;
        if ((damage.flags & wgc_damage_full_frame) != 0) {
            if (damage.dirty_count != 1
                || damage.dirty_rects[0].x != 0
                || damage.dirty_rects[0].y != 0
                || damage.dirty_rects[0].width != bus_width
                || damage.dirty_rects[0].height != bus_height) {
                return make_error(
                    GpuStatus::invalid_argument, E_INVALIDARG,
                    "full-frame damage does not cover the shared frame");
            }
        }
        if (v5_damage) {
            if ((native_move_available && inferred_move)
                || (native_move_available
                    && (((damage.flags & wgc_damage_native) == 0)
                        || ((damage.flags
                                & wgc_damage_native_move_unavailable) != 0)))
                || (damage.move_count != 0
                    && !native_move_available && !inferred_move)
                || (damage.move_count == 0 && inferred_move)) {
                return make_error(
                    GpuStatus::invalid_argument, E_INVALIDARG,
                    "shared frame move provenance is inconsistent");
            }
            if (frame_sequence != 0
                && (damage.base_sequence >= frame_sequence
                    || (damage.move_count != 0
                        && damage.base_sequence == 0))) {
                return make_error(
                    GpuStatus::invalid_argument, E_INVALIDARG,
                    "shared frame damage base sequence is invalid");
            }
        } else if (damage.move_count != 0 && !inferred_move) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "shared frame move rects are not marked as inferred");
        }
        for (std::uint32_t index = 0; index < damage.dirty_count; ++index) {
            if (!valid_bus_rect(
                    damage.dirty_rects[index], bus_width, bus_height)) {
                return make_error(
                    GpuStatus::invalid_argument, E_INVALIDARG,
                    "shared frame dirty rect is out of bounds");
            }
        }
        for (std::uint32_t index = 0; index < damage.move_count; ++index) {
            const WgcMoveRect& move = damage.move_rects[index];
            if (move.source_x < 0
                || move.source_y < 0
                || !valid_bus_rect(move.destination, bus_width, bus_height)) {
                return make_error(
                    GpuStatus::invalid_argument, E_INVALIDARG,
                    "shared frame move rect is out of bounds");
            }
            const auto source_right = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(move.source_x))
                + move.destination.width;
            const auto source_bottom = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(move.source_y))
                + move.destination.height;
            if (source_right > bus_width || source_bottom > bus_height) {
                return make_error(
                    GpuStatus::invalid_argument, E_INVALIDARG,
                    "shared frame move source is out of bounds");
            }
        }
    }

    const WgcCursorInfo& cursor = input.cursor;
    if ((cursor.flags & wgc_cursor_position_estimated) != 0
        && (cursor.flags & wgc_cursor_position_valid) == 0) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "estimated cursor position is not marked valid");
    }
    if (cursor.shape_sequence == 0) {
        if (cursor.width != 0
            || cursor.height != 0
            || cursor.hotspot_x != 0
            || cursor.hotspot_y != 0
            || (cursor.flags & wgc_cursor_shape_pending) != 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "cursor dimensions have no shape sequence");
        }
    } else if ((cursor.width == 0 || cursor.height == 0)
        && (cursor.flags & wgc_cursor_shape_pending) == 0) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "cursor shape dimensions are unavailable");
    } else if (cursor.width != 0
        && (cursor.hotspot_x >= cursor.width
            || cursor.hotspot_y >= cursor.height)) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "cursor hotspot is outside the shape");
    }

    output = {};
    output.epoch = input.epoch;
    output.epoch_nonce = input.epoch_nonce;
    output.damage.base_sequence = damage.base_sequence;
    output.damage.dirty_count = damage.dirty_count;
    output.damage.move_count = damage.move_count;
    output.damage.flags = damage.flags;
    std::copy_n(
        damage.dirty_rects.begin(), damage.dirty_count,
        output.damage.dirty_rects.begin());
    std::copy_n(
        damage.move_rects.begin(), damage.move_count,
        output.damage.move_rects.begin());
    output.cursor = cursor;
    return success();
}

constexpr std::uint32_t known_move_result_flags =
    shared_frame_bus_move_result_valid
    | shared_frame_bus_move_result_inferred
    | shared_frame_bus_move_result_fail_closed
    | shared_frame_bus_move_result_capacity_exceeded;

GpuError normalize_move_result(
    const SharedFrameBusMoveResult& input,
    std::uint32_t bus_width,
    std::uint32_t bus_height,
    SharedFrameBusMoveResult& output) noexcept {
    if (input.structure_size != sizeof(SharedFrameBusMoveResult)
        || input.result_version != shared_frame_bus_move_result_version
        || input.epoch == 0
        || input.epoch_nonce == 0
        || input.sequence == 0
        || input.base_sequence == 0
        || input.base_sequence >= input.sequence
        || input.move_count > wgc_max_move_rects
        || (input.flags & ~known_move_result_flags) != 0
        || (input.flags & shared_frame_bus_move_result_valid) == 0
        || input.reserved[0] != 0
        || input.reserved[1] != 0
        || ((input.flags & shared_frame_bus_move_result_inferred) != 0)
            != (input.move_count != 0)
        || ((input.flags & shared_frame_bus_move_result_capacity_exceeded) != 0
            && (input.flags & shared_frame_bus_move_result_fail_closed) == 0)
        || ((input.flags & shared_frame_bus_move_result_fail_closed) != 0
            && input.move_count != 0)) {
        return make_error(
            GpuStatus::invalid_argument,
            E_INVALIDARG,
            "invalid shared move-result header or reserved fields");
    }
    for (std::uint32_t index = 0; index < input.move_count; ++index) {
        const WgcMoveRect& move = input.move_rects[index];
        if (move.source_x < 0
            || move.source_y < 0
            || !valid_bus_rect(move.destination, bus_width, bus_height)) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "shared move-result rectangle is out of bounds");
        }
        const auto source_right = static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(move.source_x))
            + move.destination.width;
        const auto source_bottom = static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(move.source_y))
            + move.destination.height;
        if (source_right > bus_width || source_bottom > bus_height) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "shared move-result source is out of bounds");
        }
    }
    output = {};
    output.epoch = input.epoch;
    output.epoch_nonce = input.epoch_nonce;
    output.sequence = input.sequence;
    output.base_sequence = input.base_sequence;
    output.move_count = input.move_count;
    output.flags = input.flags;
    std::copy_n(
        input.move_rects.begin(), input.move_count, output.move_rects.begin());
    return success();
}

GpuError validate_cursor_shape(const WgcCursorShape& shape) noexcept {
    if (shape.sequence == 0
        || shape.kind == WgcCursorShapeKind::none
        || shape.width == 0
        || shape.height == 0
        || shape.width > 256
        || shape.height > 256
        || shape.hotspot_x >= shape.width
        || shape.hotspot_y >= shape.height
        || shape.stride_bytes == 0
        || shape.data.empty()
        || shape.data.size() > shared_frame_bus_max_cursor_shape_bytes) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "invalid shared cursor shape header");
    }

    std::uint64_t expected_size = 0;
    if (shape.kind == WgcCursorShapeKind::color_bgra8
        || shape.kind == WgcCursorShapeKind::masked_color_bgra8) {
        if (shape.stride_bytes != shape.width * 4u) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "invalid BGRA cursor shape stride");
        }
        expected_size = static_cast<std::uint64_t>(shape.stride_bytes)
            * shape.height;
    } else if (shape.kind == WgcCursorShapeKind::monochrome_and_xor) {
        const std::uint32_t minimum_stride = (shape.width + 7u) / 8u;
        if (shape.stride_bytes < minimum_stride
            || (shape.stride_bytes & 3u) != 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "invalid monochrome cursor shape stride");
        }
        expected_size = static_cast<std::uint64_t>(shape.stride_bytes)
            * shape.height * 2u;
    } else {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "unknown shared cursor shape kind");
    }
    if (expected_size != shape.data.size()) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "shared cursor shape byte count does not match its dimensions");
    }
    return success();
}

std::size_t control_page_size(std::uint32_t protocol_version) noexcept {
    if (protocol_version == bus_protocol_version_v1) {
        return sizeof(BusControlPageV1);
    }
    if (protocol_version == bus_protocol_version_v2) {
        return sizeof(BusControlPageV2);
    }
    if (protocol_version == bus_protocol_version_v3) {
        return sizeof(BusControlPageV3);
    }
    if (protocol_version == bus_protocol_version_v4
        || protocol_version == bus_protocol_version_v5) {
        return sizeof(BusControlPageV4);
    }
    return 0;
}

GpuError timeout_error(const char* message) noexcept {
    return make_error(
        GpuStatus::timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT), message);
}

LONG load_long(volatile LONG* value) noexcept {
    return InterlockedCompareExchange(value, 0, 0);
}

std::uint64_t load_u64(volatile LONG64* value) noexcept {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(value, 0, 0));
}

void store_u64(volatile LONG64* target, std::uint64_t value) noexcept {
    (void)InterlockedExchange64(target, static_cast<LONG64>(value));
}

bool slot_unavailable(LONG ownership) noexcept {
    constexpr ULONG unavailable = static_cast<ULONG>(writer_bit)
        | static_cast<ULONG>(quarantine_bit);
    return (static_cast<ULONG>(ownership) & unavailable) != 0;
}

std::uint64_t encode_access_tag(std::uint64_t generation) noexcept {
    return (generation & access_tag_value_mask) << access_tag_shift;
}

void close_consumer_access(
    BusConsumerControl& consumer, std::uint64_t expected_tag) noexcept {
    std::uint64_t state = load_u64(&consumer.access_state);
    while ((state & access_tag_mask) == expected_tag
        && (state & access_open_bit) != 0) {
        const std::uint64_t desired = state & ~access_open_bit;
        const std::uint64_t previous = static_cast<std::uint64_t>(
            InterlockedCompareExchange64(
                &consumer.access_state,
                static_cast<LONG64>(desired),
                static_cast<LONG64>(state)));
        if (previous == state) return;
        state = previous;
    }
}

HANDLE decode_handle(std::uint64_t value) noexcept {
    return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value));
}

std::uint64_t encode_handle(HANDLE value) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<ULONG_PTR>(value));
}

bool same_device(ID3D11Device* left, ID3D11Device* right) noexcept {
    ComPtr<IUnknown> a;
    ComPtr<IUnknown> b;
    return left != nullptr && right != nullptr
        && SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b)))
        && a.Get() == b.Get();
}

HRESULT query_adapter_luid(ID3D11Device* device, LUID& luid) noexcept {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&description);
    if (SUCCEEDED(hr)) luid = description.AdapterLuid;
    return hr;
}

bool timeout_elapsed(
    ULONGLONG start, std::uint32_t timeout_ms) noexcept {
    return timeout_ms != INFINITE && GetTickCount64() - start >= timeout_ms;
}

void wait_backoff(std::uint32_t& iteration) noexcept {
    if (iteration < 64) {
        YieldProcessor();
    } else if (iteration < 96) {
        (void)SwitchToThread();
    } else {
        Sleep(1);
    }
    ++iteration;
}

bool duplicate_to_process(
    HANDLE target_process, HANDLE source, HANDLE& target_value) noexcept {
    target_value = nullptr;
    return DuplicateHandle(
        GetCurrentProcess(), source, target_process, &target_value,
        0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
}

bool duplicate_to_process_with_access(
    HANDLE target_process, HANDLE source, DWORD access,
    HANDLE& target_value) noexcept {
    target_value = nullptr;
    return DuplicateHandle(
        GetCurrentProcess(), source, target_process, &target_value,
        access, FALSE, 0) != FALSE;
}

bool close_remote_handle(HANDLE process, HANDLE remote_handle) noexcept {
    if (process == nullptr || remote_handle == nullptr) return true;
    HANDLE local_copy = nullptr;
    if (DuplicateHandle(
            process, remote_handle, GetCurrentProcess(), &local_copy,
            0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
        if (local_copy != nullptr) CloseHandle(local_copy);
        return true;
    }
    return false;
}

GpuError fence_error(ID3D11Device* device, HRESULT hr, const char* message) noexcept {
    const HRESULT removed = device == nullptr ? S_OK : device->GetDeviceRemovedReason();
    return FAILED(removed)
        ? make_error(GpuStatus::device_lost, removed, message)
        : make_error(GpuStatus::system_error, hr, message);
}

} // namespace

struct SharedFrameBusPublisherState final {
    struct ConsumerRecord final {
        ComPtr<ID3D11Fence> done_fence;
        HANDLE process = nullptr;
        std::uint64_t token = 0;
        std::uint32_t process_id = 0;
        bool active = false;
        bool poisoned = false;
    };

    ~SharedFrameBusPublisherState() {
        if (control != nullptr) {
            (void)InterlockedExchange(&control->shutting_down, 1);
            for (std::uint32_t index = 0; index < config.max_consumers; ++index) {
                if (consumers[index].active) {
                    close_consumer_access(
                        control->consumers[index],
                        encode_access_tag(consumers[index].token));
                }
            }
        }
        for (auto& consumer : consumers) {
            if (consumer.process != nullptr) CloseHandle(consumer.process);
        }
        if (ready_fence_handle != nullptr) CloseHandle(ready_fence_handle);
        if (control != nullptr) UnmapViewOfFile(control);
        if (control_mapping != nullptr) CloseHandle(control_mapping);
    }

    GpuError initialize(ID3D11Device* source_device, const SharedFrameBusConfig& source_config) {
        if (source_device == nullptr
            || source_config.width == 0
            || source_config.height == 0
            || source_config.format == DXGI_FORMAT_UNKNOWN
            || source_config.color_space == DXGI_COLOR_SPACE_RESERVED
            || source_config.slot_count < 2
            || source_config.slot_count > shared_frame_bus_max_slots
            || source_config.max_consumers == 0
            || source_config.max_consumers > shared_frame_bus_max_consumers) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "invalid shared frame bus configuration");
        }

        config = source_config;
        device = source_device;
        HRESULT hr = device.As(&device5);
        if (FAILED(hr)) {
            return make_error(
                GpuStatus::unsupported, hr,
                "D3D11 shared fences require ID3D11Device5");
        }
        ComPtr<ID3D11DeviceContext> base_context;
        device->GetImmediateContext(&base_context);
        hr = base_context.As(&context4);
        if (FAILED(hr)) {
            return make_error(
                GpuStatus::unsupported, hr,
                "D3D11 shared fences require ID3D11DeviceContext4");
        }
        ComPtr<ID3D10Multithread> multithread;
        hr = base_context.As(&multithread);
        if (FAILED(hr)) {
            return make_error(
                GpuStatus::unsupported, hr,
                "D3D11 immediate-context synchronization is unavailable");
        }
        (void)multithread->SetMultithreadProtected(TRUE);
        hr = query_adapter_luid(device.Get(), adapter_luid);
        if (FAILED(hr)) {
            return make_error(GpuStatus::system_error, hr, "adapter LUID query failed");
        }

        D3D11_TEXTURE2D_DESC description{};
        description.Width = config.width;
        description.Height = config.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = config.format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = config.bind_flags;
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (std::uint32_t index = 0; index < config.slot_count; ++index) {
            hr = device->CreateTexture2D(&description, nullptr, &textures[index]);
            if (FAILED(hr)) {
                return make_error(
                    GpuStatus::unsupported, hr,
                    "shared frame bus texture creation failed");
            }
            ComPtr<IDXGIResource> resource;
            hr = textures[index].As(&resource);
            if (SUCCEEDED(hr)) {
                hr = resource->GetSharedHandle(&texture_shared_handles[index]);
            }
            if (FAILED(hr)) {
                return make_error(
                    GpuStatus::system_error, hr,
                    "shared frame bus texture identifier creation failed");
            }
        }

        hr = device5->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready_fence));
        if (FAILED(hr)) {
            return make_error(
                GpuStatus::unsupported, hr,
                "shared ready fence creation failed");
        }
        hr = ready_fence->CreateSharedHandle(
            nullptr, GENERIC_ALL, nullptr, &ready_fence_handle);
        if (FAILED(hr)) {
            return make_error(
                GpuStatus::system_error, hr,
                "shared ready fence handle creation failed");
        }

        control_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(BusControlPageV4)), nullptr);
        if (control_mapping == nullptr) {
            return make_error(
                GpuStatus::system_error, HRESULT_FROM_WIN32(GetLastError()),
                "shared frame bus control mapping creation failed");
        }
        control_v4 = static_cast<BusControlPageV4*>(MapViewOfFile(
            control_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
            sizeof(BusControlPageV4)));
        if (control_v4 == nullptr) {
            return make_error(
                GpuStatus::system_error, HRESULT_FROM_WIN32(GetLastError()),
                "shared frame bus control mapping failed");
        }

        std::memset(control_v4, 0, sizeof(*control_v4));
        control_v3 = &control_v4->v3;
        control_v2 = &control_v3->v2;
        control = &control_v2->common;
        control->magic = bus_magic;
        control->protocol_version = shared_frame_bus_protocol_version;
        control->structure_size = sizeof(BusControlPageV4);
        control->slot_count = config.slot_count;
        control->max_consumers = config.max_consumers;
        control->width = config.width;
        control->height = config.height;
        control->format = static_cast<std::uint32_t>(config.format);
        control->adapter_luid_low = adapter_luid.LowPart;
        control->adapter_luid_high = adapter_luid.HighPart;
        return success();
    }

    void finalize_consumer_locked(std::uint32_t index, bool dead) noexcept {
        ConsumerRecord& record = consumers[index];
        if (!record.active) return;
        const LONG bit = static_cast<LONG>(1UL << index);
        for (std::uint32_t slot = 0; slot < config.slot_count; ++slot) {
            (void)InterlockedAnd(&control->slots[slot].ownership, ~bit);
        }
        BusConsumerControl& shared = control->consumers[index];
        if (load_u64(&shared.token) == record.token) {
            store_u64(&shared.access_state, 0);
            (void)InterlockedCompareExchange64(
                &shared.token, 0, static_cast<LONG64>(record.token));
        }
        if (record.process != nullptr) CloseHandle(record.process);
        record = {};
        ++statistics.consumer_reclamations;
        if (dead) ++statistics.dead_consumer_reclamations;
        if (statistics.active_consumers != 0) --statistics.active_consumers;
    }

    bool consumer_has_readers_locked(std::uint32_t index) const noexcept {
        const LONG bit = static_cast<LONG>(1UL << index);
        for (std::uint32_t slot = 0; slot < config.slot_count; ++slot) {
            if ((load_long(&control->slots[slot].ownership) & bit) != 0) return true;
        }
        return false;
    }

    GpuError sweep_consumers_locked() noexcept {
        for (std::uint32_t index = 0; index < config.max_consumers; ++index) {
            ConsumerRecord& record = consumers[index];
            if (!record.active) continue;

            const DWORD process_wait = WaitForSingleObject(record.process, 0);
            if (process_wait == WAIT_OBJECT_0) {
                if (!record.poisoned) {
                    const std::uint64_t completed =
                        record.done_fence->GetCompletedValue();
                    const bool completion_valid = completed
                        != std::numeric_limits<std::uint64_t>::max();
                    const HRESULT removed = device->GetDeviceRemovedReason();
                    if (FAILED(removed)) {
                        terminal_failure = make_error(
                            GpuStatus::device_lost, removed,
                            "device removed while quarantining a dead consumer");
                        (void)InterlockedExchange(&control->shutting_down, 1);
                        return terminal_failure;
                    }
                    const LONG bit = static_cast<LONG>(1UL << index);
                    for (std::uint32_t slot = 0; slot < config.slot_count; ++slot) {
                        BusSlotControl& shared_slot = control->slots[slot];
                        if ((load_long(&shared_slot.ownership) & bit) == 0) continue;
                        if (completion_valid
                            && completed >= load_u64(&shared_slot.sequence)) {
                            (void)InterlockedAnd(&shared_slot.ownership, ~bit);
                            continue;
                        }
                        const LONG previous = InterlockedOr(
                            &shared_slot.ownership, quarantine_bit);
                        (void)previous;
                        (void)InterlockedAnd(&shared_slot.ownership, ~bit);
                    }
                }
                close_consumer_access(
                    control->consumers[index], encode_access_tag(record.token));
                finalize_consumer_locked(index, true);
                continue;
            }
            if (process_wait == WAIT_FAILED) {
                return make_error(
                    GpuStatus::system_error, HRESULT_FROM_WIN32(GetLastError()),
                    "consumer process liveness query failed");
            }

            if (record.poisoned) continue;

            const std::uint64_t completed = record.done_fence->GetCompletedValue();
            const bool completion_valid = completed
                != std::numeric_limits<std::uint64_t>::max();
            const HRESULT removed = device->GetDeviceRemovedReason();
            if (FAILED(removed)) {
                terminal_failure = make_error(
                    GpuStatus::device_lost, removed,
                    "device removed while reclaiming shared frame bus slots");
                (void)InterlockedExchange(&control->shutting_down, 1);
                return terminal_failure;
            }
            const LONG bit = static_cast<LONG>(1UL << index);
            if (!completion_valid) {
                close_consumer_access(
                    control->consumers[index], encode_access_tag(record.token));
            }
            for (std::uint32_t slot = 0; slot < config.slot_count; ++slot) {
                BusSlotControl& shared_slot = control->slots[slot];
                const LONG ownership = load_long(&shared_slot.ownership);
                if ((ownership & bit) == 0) continue;
                if (!completion_valid) {
                    (void)InterlockedOr(&shared_slot.ownership, quarantine_bit);
                    (void)InterlockedAnd(&shared_slot.ownership, ~bit);
                } else if (completed >= load_u64(&shared_slot.sequence)) {
                    (void)InterlockedAnd(&shared_slot.ownership, ~bit);
                }
            }

            BusConsumerControl& shared = control->consumers[index];
            const std::uint64_t access = load_u64(&shared.access_state);
            const bool requested_close = (access & access_open_bit) == 0
                || load_u64(&shared.token) != record.token;
            if (requested_close
                && (access & access_ref_mask) == 0
                && !consumer_has_readers_locked(index)) {
                finalize_consumer_locked(index, false);
            }
        }
        account_quarantined_slots_locked();
        return success();
    }

    void account_quarantined_slots_locked() noexcept {
        for (std::uint32_t slot = 0; slot < config.slot_count; ++slot) {
            const std::uint32_t bit = 1U << slot;
            if ((quarantine_accounted_mask & bit) == 0
                && (load_long(&control->slots[slot].ownership) & quarantine_bit) != 0) {
                quarantine_accounted_mask |= bit;
                ++statistics.quarantined_slots;
            }
        }
    }

    bool all_slots_quarantined_locked() const noexcept {
        for (std::uint32_t slot = 0; slot < config.slot_count; ++slot) {
            if ((load_long(&control->slots[slot].ownership) & quarantine_bit) == 0) {
                return false;
            }
        }
        return true;
    }

    GpuError claim_slot_locked(
        std::uint32_t timeout_ms, std::uint32_t& selected,
        std::uint64_t& lease_token) noexcept {
        if (writer_active) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "a shared frame bus write lease is already active");
        }

        const ULONGLONG start = GetTickCount64();
        std::uint32_t iteration = 0;
        for (;;) {
            const GpuError swept = sweep_consumers_locked();
            if (!swept) return swept;
            if (all_slots_quarantined_locked()) {
                return make_error(
                    GpuStatus::system_error, E_ABORT,
                    "all shared frame bus slots are quarantined; recreate the bus");
            }

            std::uint32_t candidate = invalid_slot;
            std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
            for (std::uint32_t slot = 0; slot < config.slot_count; ++slot) {
                BusSlotControl& shared_slot = control->slots[slot];
                if (load_long(&shared_slot.ownership) != 0) continue;
                const std::uint64_t sequence = load_u64(&shared_slot.sequence);
                if (candidate == invalid_slot || sequence < oldest) {
                    candidate = slot;
                    oldest = sequence;
                }
            }

            if (candidate != invalid_slot) {
                BusSlotControl& shared_slot = control->slots[candidate];
                if (InterlockedCompareExchange(
                        &shared_slot.ownership, writer_bit, 0) == 0) {
                    const std::uint64_t previous = load_u64(&shared_slot.sequence);
                    store_u64(&shared_slot.sequence, 0);
                    store_u64(&control_v2->metadata[candidate].sequence, 0);
                    store_u64(&control_v3->side_data[candidate].sequence, 0);
                    writer_active = true;
                    writer_slot = candidate;
                    ++writer_token;
                    if (writer_token == 0) ++writer_token;
                    selected = candidate;
                    lease_token = writer_token;
                    if (previous != 0) ++statistics.reused_ready_slots;
                    return success();
                }
            }

            if (timeout_ms == 0 || timeout_elapsed(start, timeout_ms)) {
                ++statistics.no_slot;
                return timeout_error("no reclaimable shared frame bus slot");
            }
            wait_backoff(iteration);
        }
    }

    void cancel_write_locked(std::uint32_t slot, std::uint64_t token) noexcept {
        if (!writer_active || writer_slot != slot || writer_token != token) return;
        store_u64(&control->slots[slot].sequence, 0);
        store_u64(&control_v2->metadata[slot].sequence, 0);
        store_u64(&control_v3->side_data[slot].sequence, 0);
        (void)InterlockedExchange(&control->slots[slot].ownership, 0);
        writer_active = false;
        writer_slot = invalid_slot;
    }

    void cancel_write(std::uint32_t slot, std::uint64_t token) noexcept {
        std::lock_guard lock(mutex);
        cancel_write_locked(slot, token);
    }

    GpuError commit_write_locked(
        std::uint32_t slot, std::uint64_t token, bool direct,
        const SharedFrameBusFrameMetadata& metadata,
        const SharedFrameBusFrameSideData& side_data) noexcept {
        if (!writer_active || writer_slot != slot || writer_token != token) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus write lease is stale");
        }
        SharedFrameBusFrameMetadata normalized;
        const GpuError metadata_validated = normalize_metadata(
            metadata, config.width, config.height, normalized,
            config.color_space, true);
        if (!metadata_validated) {
            cancel_write_locked(slot, token);
            return metadata_validated;
        }
        const std::uint64_t current = load_u64(&control->sequence);
        if (current >= std::numeric_limits<std::uint64_t>::max() - 1) {
            cancel_write_locked(slot, token);
            return make_error(
                GpuStatus::system_error, E_UNEXPECTED,
                "shared frame bus sequence is exhausted");
        }
        const std::uint64_t next = current + 1;
        SharedFrameBusFrameSideData normalized_side_data;
        const GpuError side_data_validated = normalize_side_data(
            side_data,
            config.width,
            config.height,
            shared_frame_bus_protocol_version,
            next,
            normalized_side_data);
        if (!side_data_validated) {
            cancel_write_locked(slot, token);
            return side_data_validated;
        }
        const HRESULT hr = context4->Signal(ready_fence.Get(), next);
        if (FAILED(hr)) {
            const HRESULT removed = device->GetDeviceRemovedReason();
            if (FAILED(removed)) {
                terminal_failure = make_error(
                    GpuStatus::device_lost, removed,
                    "device removed while signaling the ready fence");
                (void)InterlockedExchange(&control->shutting_down, 1);
            }
            cancel_write_locked(slot, token);
            return FAILED(removed)
                ? terminal_failure
                : make_error(
                    GpuStatus::system_error, hr,
                    "ready fence signal failed");
        }
        context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            terminal_failure = make_error(
                GpuStatus::device_lost, removed,
                "device removed while publishing a shared frame");
            (void)InterlockedExchange(&control->shutting_down, 1);
            cancel_write_locked(slot, token);
            return terminal_failure;
        }

        control_v2->metadata[slot].payload = normalized;
        control_v3->side_data[slot].payload = normalized_side_data;
        store_u64(&control_v2->metadata[slot].sequence, next);
        store_u64(&control_v3->side_data[slot].sequence, next);
        store_u64(&control->slots[slot].sequence, next);
        store_u64(&control->sequence, next);
        (void)InterlockedExchange(&control->slots[slot].ownership, 0);
        writer_active = false;
        writer_slot = invalid_slot;
        ++statistics.published_frames;
        if (direct) ++statistics.direct_publishes;
        else ++statistics.copied_publishes;
        return success();
    }

    GpuError validate_source(ID3D11Texture2D* source) const noexcept {
        if (source == nullptr) {
            return make_error(
                GpuStatus::invalid_argument, E_POINTER,
                "source texture is null");
        }
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        ComPtr<ID3D11Device> source_device;
        source->GetDevice(&source_device);
        if (!same_device(device.Get(), source_device.Get())
            || description.Width != config.width
            || description.Height != config.height
            || description.Format != config.format
            || description.MipLevels != 1
            || description.ArraySize != 1
            || description.SampleDesc.Count != 1) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "source texture does not match the shared frame bus");
        }
        return success();
    }

    GpuError closed_error_locked() const noexcept {
        return terminal_failure.status != GpuStatus::ok
            ? terminal_failure
            : make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus is closed");
    }

    GpuError publish(
        ID3D11Texture2D* source,
        const SharedFrameBusFrameMetadata& metadata,
        const SharedFrameBusFrameSideData& side_data,
        std::uint32_t timeout_ms) noexcept {
        std::lock_guard lock(mutex);
        if (load_long(&control->shutting_down) != 0) {
            return closed_error_locked();
        }
        if (reserved_producer_token != 0) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus producer is reserved by WGC");
        }
        const GpuError validated = validate_source(source);
        if (!validated) return validated;
        SharedFrameBusFrameMetadata normalized;
        const GpuError metadata_validated = normalize_metadata(
            metadata, config.width, config.height, normalized,
            config.color_space, true);
        if (!metadata_validated) return metadata_validated;
        const std::uint64_t current = load_u64(&control->sequence);
        if (current >= std::numeric_limits<std::uint64_t>::max() - 1) {
            return make_error(
                GpuStatus::system_error, E_UNEXPECTED,
                "shared frame bus sequence is exhausted");
        }
        SharedFrameBusFrameSideData normalized_side_data;
        const GpuError side_data_validated = normalize_side_data(
            side_data,
            config.width,
            config.height,
            shared_frame_bus_protocol_version,
            current + 1,
            normalized_side_data);
        if (!side_data_validated) return side_data_validated;
        ++statistics.publish_attempts;
        std::uint32_t slot = invalid_slot;
        std::uint64_t token = 0;
        const GpuError claimed = claim_slot_locked(timeout_ms, slot, token);
        if (!claimed) return claimed;
        context4->CopyResource(textures[slot].Get(), source);
        return commit_write_locked(
            slot, token, false, normalized, normalized_side_data);
    }

    GpuError begin_publish(
        std::uint32_t timeout_ms, SharedFrameBusWriteLease& output,
        const std::shared_ptr<SharedFrameBusPublisherState>& self,
        std::uint64_t producer_token = 0) noexcept {
        std::unique_lock lock(mutex, std::defer_lock);
        if (producer_token != 0 && timeout_ms == 0) {
            if (!lock.try_lock()) {
                return timeout_error(
                    "shared frame bus control path is busy");
            }
        } else {
            lock.lock();
        }
        if (output.state_ != nullptr) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "output write lease is not empty");
        }
        if (load_long(&control->shutting_down) != 0) {
            return closed_error_locked();
        }
        if ((producer_token == 0 && reserved_producer_token != 0)
            || (producer_token != 0
                && producer_token != reserved_producer_token)) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus producer reservation is not owned by this caller");
        }
        ++statistics.publish_attempts;
        std::uint32_t slot = invalid_slot;
        std::uint64_t token = 0;
        const GpuError claimed = claim_slot_locked(timeout_ms, slot, token);
        if (!claimed) return claimed;
        output.state_ = self;
        output.texture_ = textures[slot].Get();
        output.slot_ = slot;
        output.token_ = token;
        output.sequence_ = load_u64(&control->sequence) + 1;
        return success();
    }

    GpuError reserve_producer(std::uint64_t& output_token) noexcept {
        std::lock_guard lock(mutex);
        if (output_token != 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "producer reservation output token is not empty");
        }
        if (load_long(&control->shutting_down) != 0) {
            return closed_error_locked();
        }
        if (reserved_producer_token != 0 || writer_active) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus already has an active producer");
        }
        ++next_producer_token;
        if (next_producer_token == 0) ++next_producer_token;
        reserved_producer_token = next_producer_token;
        output_token = reserved_producer_token;
        return success();
    }

    void release_producer(std::uint64_t producer_token) noexcept {
        if (producer_token == 0) return;
        std::lock_guard lock(mutex);
        if (reserved_producer_token != producer_token) return;
        if (writer_active) {
            cancel_write_locked(writer_slot, writer_token);
        }
        reserved_producer_token = 0;
    }

    GpuError commit_reserved(
        std::uint64_t producer_token,
        SharedFrameBusWriteLease&& lease,
        const SharedFrameBusFrameMetadata& metadata,
        const SharedFrameBusFrameSideData& side_data) noexcept {
        if (lease.state_.get() != this) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "write lease does not belong to this shared frame bus");
        }

        GpuError result;
        {
            std::lock_guard lock(mutex);
            if (producer_token == 0
                || reserved_producer_token != producer_token) {
                cancel_write_locked(lease.slot_, lease.token_);
                result = make_error(
                    GpuStatus::invalid_argument, E_UNEXPECTED,
                    "shared frame bus producer reservation is stale");
            } else {
                result = commit_write_locked(
                    lease.slot_, lease.token_, true, metadata, side_data);
            }
        }
        lease.state_.reset();
        lease.texture_ = nullptr;
        lease.token_ = 0;
        lease.sequence_ = 0;
        lease.slot_ = 0;
        return result;
    }

    GpuError publish_cursor_shape(
        std::uint64_t producer_token,
        const WgcCursorShape& shape) noexcept {
        std::lock_guard lock(mutex);
        if (load_long(&control->shutting_down) != 0) {
            return closed_error_locked();
        }
        if ((producer_token == 0 && reserved_producer_token != 0)
            || (producer_token != 0
                && producer_token != reserved_producer_token)) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared cursor shape producer reservation is stale");
        }
        const GpuError validated = validate_cursor_shape(shape);
        if (!validated) return validated;
        if (cursor_commit_sequence
            >= std::numeric_limits<std::uint64_t>::max() - 2) {
            return make_error(
                GpuStatus::system_error, E_UNEXPECTED,
                "shared cursor shape commit sequence is exhausted");
        }

        BusCursorShapeSlotV3& slot =
            control_v3->cursor_shapes[cursor_next_slot];
        const std::uint64_t writing = cursor_commit_sequence + 1;
        const std::uint64_t committed = writing + 1;
        store_u64(&slot.commit_sequence, writing);
        MemoryBarrier();
        slot.shape_sequence = shape.sequence;
        slot.kind = static_cast<std::uint32_t>(shape.kind);
        slot.width = shape.width;
        slot.height = shape.height;
        slot.hotspot_x = shape.hotspot_x;
        slot.hotspot_y = shape.hotspot_y;
        slot.stride_bytes = shape.stride_bytes;
        slot.data_size = static_cast<std::uint32_t>(shape.data.size());
        slot.reserved = 0;
        std::memcpy(slot.data.data(), shape.data.data(), shape.data.size());
        MemoryBarrier();
        store_u64(&slot.commit_sequence, committed);
        cursor_commit_sequence = committed;
        cursor_next_slot ^= 1u;
        return success();
    }

    GpuError publish_move_result(
        std::uint64_t producer_token,
        const SharedFrameBusMoveResult& input) noexcept {
        std::lock_guard lock(mutex);
        if (load_long(&control->shutting_down) != 0) {
            return closed_error_locked();
        }
        if ((producer_token == 0 && reserved_producer_token != 0)
            || (producer_token != 0
                && producer_token != reserved_producer_token)) {
            return make_error(
                GpuStatus::invalid_argument,
                E_UNEXPECTED,
                "shared move-result producer reservation is stale");
        }
        SharedFrameBusMoveResult normalized;
        const GpuError validated = normalize_move_result(
            input, config.width, config.height, normalized);
        if (!validated) return validated;
        if (normalized.sequence > load_u64(&control->sequence)) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "shared move result refers to an unpublished frame");
        }
        if (normalized.sequence <= last_move_result_sequence) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "shared move-result sequences must increase");
        }
        if (move_result_commit_sequence
            >= std::numeric_limits<std::uint64_t>::max() - 2) {
            return make_error(
                GpuStatus::system_error,
                E_UNEXPECTED,
                "shared move-result commit sequence is exhausted");
        }

        BusMoveResultSlotV4& slot =
            control_v4->move_results[move_result_next_slot];
        const bool overwrite = load_u64(&slot.commit_sequence) != 0;
        const std::uint64_t writing = move_result_commit_sequence + 1;
        const std::uint64_t committed = writing + 1;
        store_u64(&slot.commit_sequence, writing);
        MemoryBarrier();
        slot.payload = normalized;
        slot.reserved.fill(0);
        MemoryBarrier();
        store_u64(&slot.commit_sequence, committed);
        move_result_commit_sequence = committed;
        last_move_result_sequence = normalized.sequence;
        move_result_next_slot =
            (move_result_next_slot + 1)
            % shared_frame_bus_move_result_slots;
        ++statistics.move_results_published;
        if (overwrite) ++statistics.move_result_overwrites;
        return success();
    }

    GpuError register_consumer(
        HANDLE target_process, SharedFrameBusRegistration& output) noexcept {
        if (target_process == nullptr) {
            return make_error(
                GpuStatus::invalid_argument, E_HANDLE,
                "target process handle is null");
        }
        std::lock_guard lock(mutex);
        if (load_long(&control->shutting_down) != 0) {
            return closed_error_locked();
        }
        if (writer_active) {
            return timeout_error(
                "shared frame bus producer commit is pending; retry consumer registration");
        }
        const GpuError swept = sweep_consumers_locked();
        if (!swept) return swept;

        std::uint32_t consumer_index = invalid_slot;
        for (std::uint32_t index = 0; index < config.max_consumers; ++index) {
            if (!consumers[index].active) {
                consumer_index = index;
                break;
            }
        }
        if (consumer_index == invalid_slot) {
            return timeout_error("shared frame bus consumer table is full");
        }

        const DWORD process_id = GetProcessId(target_process);
        if (process_id == 0) {
            return make_error(
                GpuStatus::system_error, HRESULT_FROM_WIN32(GetLastError()),
                "target process id query failed");
        }
        HANDLE watched_process = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(), target_process, GetCurrentProcess(),
                &watched_process, SYNCHRONIZE, FALSE, 0)) {
            return make_error(
                GpuStatus::system_error, HRESULT_FROM_WIN32(GetLastError()),
                "target process handle must permit synchronization");
        }

        ComPtr<ID3D11Fence> done_fence;
        HRESULT hr = device5->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&done_fence));
        if (FAILED(hr)) {
            CloseHandle(watched_process);
            return make_error(
                GpuStatus::unsupported, hr,
                "consumer done fence creation failed");
        }
        HANDLE done_fence_handle = nullptr;
        hr = done_fence->CreateSharedHandle(
            nullptr, GENERIC_ALL, nullptr, &done_fence_handle);
        if (FAILED(hr)) {
            CloseHandle(watched_process);
            return make_error(
                GpuStatus::system_error, hr,
                "consumer done fence handle creation failed");
        }

        SharedFrameBusRegistration registration{};
        registration.width = config.width;
        registration.height = config.height;
        registration.format = static_cast<std::uint32_t>(config.format);
        registration.slot_count = config.slot_count;
        registration.consumer_index = consumer_index;
        registration.target_process_id = process_id;
        registration.adapter_luid_low = adapter_luid.LowPart;
        registration.adapter_luid_high = adapter_luid.HighPart;
        if (next_consumer_generation == 0
            || next_consumer_generation > access_tag_value_mask) {
            CloseHandle(done_fence_handle);
            CloseHandle(watched_process);
            return make_error(
                GpuStatus::system_error, E_UNEXPECTED,
                "shared frame bus consumer generation is exhausted");
        }
        registration.consumer_token = next_consumer_generation++;

        std::array<HANDLE, 4> remote_handles{};
        std::size_t duplicated_count = 0;
        DWORD duplicate_error = ERROR_SUCCESS;
        const auto duplicate = [&](HANDLE local, std::uint64_t& encoded) {
            HANDLE remote = nullptr;
            if (!duplicate_to_process(target_process, local, remote)) {
                duplicate_error = GetLastError();
                return false;
            }
            remote_handles[duplicated_count++] = remote;
            encoded = encode_handle(remote);
            return true;
        };

        bool duplicated = duplicate(
            control_mapping, registration.control_mapping_handle);
        if (duplicated) {
            HANDLE remote = nullptr;
            duplicated = duplicate_to_process_with_access(
                target_process, GetCurrentProcess(), SYNCHRONIZE, remote);
            if (duplicated) {
                remote_handles[duplicated_count++] = remote;
                registration.publisher_process_handle = encode_handle(remote);
            } else {
                duplicate_error = GetLastError();
            }
        }
        if (duplicated) {
            duplicated = duplicate(
                ready_fence_handle, registration.ready_fence_handle);
        }
        if (duplicated) {
            duplicated = duplicate(
                done_fence_handle, registration.done_fence_handle);
        }
        for (std::uint32_t slot = 0; slot < config.slot_count; ++slot) {
            registration.texture_handles[slot] =
                encode_handle(texture_shared_handles[slot]);
        }
        CloseHandle(done_fence_handle);
        done_fence_handle = nullptr;

        const DWORD process_wait = WaitForSingleObject(watched_process, 0);
        const DWORD process_wait_error = process_wait == WAIT_FAILED
            ? GetLastError()
            : ERROR_SUCCESS;
        if (!duplicated || process_wait == WAIT_OBJECT_0 || process_wait == WAIT_FAILED) {
            const DWORD first_error = !duplicated
                ? duplicate_error
                : process_wait == WAIT_OBJECT_0
                    ? ERROR_PROCESS_ABORTED
                    : process_wait_error;
            bool rolled_back = true;
            for (std::size_t index = 0; index < duplicated_count; ++index) {
                rolled_back = close_remote_handle(
                    target_process, remote_handles[index]) && rolled_back;
            }
            if (!rolled_back && process_wait != WAIT_OBJECT_0) {
                ConsumerRecord& poisoned = consumers[consumer_index];
                poisoned.done_fence = std::move(done_fence);
                poisoned.process = watched_process;
                poisoned.token = registration.consumer_token;
                poisoned.process_id = process_id;
                poisoned.active = true;
                poisoned.poisoned = true;
                watched_process = nullptr;
                BusConsumerControl& shared = control->consumers[consumer_index];
                store_u64(&shared.access_state, 0);
                (void)InterlockedExchange(
                    &shared.process_id, static_cast<LONG>(process_id));
                store_u64(&shared.token, registration.consumer_token);
                ++statistics.active_consumers;
            }
            if (watched_process != nullptr) CloseHandle(watched_process);
            return make_error(
                GpuStatus::system_error, HRESULT_FROM_WIN32(first_error),
                !rolled_back
                    ? "consumer registration failed; leaked remote handles are quarantined"
                    : duplicated
                    ? "target process exited during consumer registration"
                    : "failed to duplicate shared frame bus handles");
        }

        ConsumerRecord& record = consumers[consumer_index];
        record.done_fence = std::move(done_fence);
        record.process = watched_process;
        record.token = registration.consumer_token;
        record.process_id = process_id;
        record.active = true;
        BusConsumerControl& shared = control->consumers[consumer_index];
        (void)InterlockedExchange(&shared.process_id, static_cast<LONG>(process_id));
        store_u64(&shared.token, registration.consumer_token);
        store_u64(
            &shared.access_state,
            encode_access_tag(registration.consumer_token) | access_open_bit);

        ++statistics.consumer_registrations;
        ++statistics.active_consumers;
        output = registration;
        return success();
    }

    GpuError unregister_consumer(
        const SharedFrameBusRegistration& registration,
        std::uint32_t timeout_ms) noexcept {
        if (registration.structure_size != sizeof(SharedFrameBusRegistration)
            || registration.protocol_version != shared_frame_bus_protocol_version
            || registration.consumer_index >= config.max_consumers
            || registration.consumer_token == 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "invalid shared frame bus registration");
        }

        {
            std::lock_guard lock(mutex);
            if (writer_active) {
                return timeout_error(
                    "shared frame bus producer commit is pending; retry consumer unregistration");
            }
            ConsumerRecord& record = consumers[registration.consumer_index];
            if (!record.active || record.token != registration.consumer_token) {
                return make_error(
                    GpuStatus::invalid_argument, E_UNEXPECTED,
                    "shared frame bus registration is stale");
            }
            BusConsumerControl& shared =
                control->consumers[registration.consumer_index];
            if (load_u64(&shared.token) != registration.consumer_token) {
                return make_error(
                    GpuStatus::invalid_argument, E_UNEXPECTED,
                    "shared frame bus registration token is stale");
            }
            close_consumer_access(
                shared, encode_access_tag(registration.consumer_token));
        }

        const ULONGLONG start = GetTickCount64();
        std::uint32_t iteration = 0;
        for (;;) {
            {
                std::lock_guard lock(mutex);
                const GpuError swept = sweep_consumers_locked();
                if (!swept) return swept;
                const ConsumerRecord& record =
                    consumers[registration.consumer_index];
                if (!record.active
                    || record.token != registration.consumer_token) {
                    return success();
                }
            }
            if (timeout_ms == 0 || timeout_elapsed(start, timeout_ms)) {
                return timeout_error(
                    "consumer done fence has not released every slot");
            }
            wait_backoff(iteration);
        }
    }

    SharedFrameBusStats stats() noexcept {
        std::lock_guard lock(mutex);
        account_quarantined_slots_locked();
        return statistics;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    std::array<ComPtr<ID3D11Texture2D>, shared_frame_bus_max_slots> textures;
    std::array<HANDLE, shared_frame_bus_max_slots> texture_shared_handles{};
    ComPtr<ID3D11Fence> ready_fence;
    HANDLE ready_fence_handle = nullptr;
    HANDLE control_mapping = nullptr;
    BusControlPageV1* control = nullptr;
    BusControlPageV2* control_v2 = nullptr;
    BusControlPageV3* control_v3 = nullptr;
    BusControlPageV4* control_v4 = nullptr;
    LUID adapter_luid{};
    SharedFrameBusConfig config{};
    std::array<ConsumerRecord, shared_frame_bus_max_consumers> consumers;
    mutable std::mutex mutex;
    SharedFrameBusStats statistics{};
    GpuError terminal_failure{};
    bool writer_active = false;
    std::uint32_t writer_slot = invalid_slot;
    std::uint64_t writer_token = 0;
    std::uint64_t reserved_producer_token = 0;
    std::uint64_t next_producer_token = 0;
    std::uint64_t next_consumer_generation = 1;
    std::uint64_t cursor_commit_sequence = 0;
    std::uint32_t cursor_next_slot = 0;
    std::uint64_t move_result_commit_sequence = 0;
    std::uint64_t last_move_result_sequence = 0;
    std::uint32_t move_result_next_slot = 0;
    std::uint32_t quarantine_accounted_mask = 0;
};

namespace internal {

GpuError reserve_shared_frame_bus_producer(
    const std::shared_ptr<SharedFrameBusPublisherState>& state,
    std::uint64_t& output_token) noexcept {
    if (state == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state->reserve_producer(output_token);
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared frame bus producer reservation failure");
    }
}

void release_shared_frame_bus_producer(
    const std::shared_ptr<SharedFrameBusPublisherState>& state,
    std::uint64_t producer_token) noexcept {
    if (state == nullptr) return;
    try {
        state->release_producer(producer_token);
    } catch (...) {
    }
}

GpuError begin_shared_frame_bus_publish(
    const std::shared_ptr<SharedFrameBusPublisherState>& state,
    std::uint64_t producer_token,
    SharedFrameBusWriteLease& output) noexcept {
    if (state == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state->begin_publish(0, output, state, producer_token);
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown direct WGC publish begin failure");
    }
}

GpuError commit_shared_frame_bus_publish(
    const std::shared_ptr<SharedFrameBusPublisherState>& state,
    std::uint64_t producer_token,
    SharedFrameBusWriteLease&& lease,
    const SharedFrameBusFrameMetadata& metadata) noexcept {
    return commit_shared_frame_bus_publish(
        state,
        producer_token,
        std::move(lease),
        metadata,
        SharedFrameBusFrameSideData{});
}

GpuError commit_shared_frame_bus_publish(
    const std::shared_ptr<SharedFrameBusPublisherState>& state,
    std::uint64_t producer_token,
    SharedFrameBusWriteLease&& lease,
    const SharedFrameBusFrameMetadata& metadata,
    const SharedFrameBusFrameSideData& side_data) noexcept {
    if (state == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state->commit_reserved(
            producer_token, std::move(lease), metadata, side_data);
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown direct WGC publish commit failure");
    }
}

GpuError publish_shared_frame_bus_cursor_shape(
    const std::shared_ptr<SharedFrameBusPublisherState>& state,
    std::uint64_t producer_token,
    const WgcCursorShape& shape) noexcept {
    if (state == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state->publish_cursor_shape(producer_token, shape);
    } catch (const std::bad_alloc&) {
        return make_error(
            GpuStatus::out_of_memory, E_OUTOFMEMORY,
            "shared cursor shape allocation failed");
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared cursor shape publish failure");
    }
}

GpuError publish_shared_frame_bus_move_result(
    const std::shared_ptr<SharedFrameBusPublisherState>& state,
    std::uint64_t producer_token,
    const SharedFrameBusMoveResult& result) noexcept {
    if (state == nullptr) {
        return make_error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state->publish_move_result(producer_token, result);
    } catch (...) {
        return make_error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown shared move-result publish failure");
    }
}

ID3D11Device* shared_frame_bus_device(
    const std::shared_ptr<SharedFrameBusPublisherState>& state) noexcept {
    return state != nullptr ? state->device.Get() : nullptr;
}

SharedFrameBusConfig shared_frame_bus_config(
    const std::shared_ptr<SharedFrameBusPublisherState>& state) noexcept {
    return state != nullptr ? state->config : SharedFrameBusConfig{};
}

std::uint64_t shared_frame_bus_sequence(
    const std::shared_ptr<SharedFrameBusPublisherState>& state) noexcept {
    return state != nullptr && state->control != nullptr
        ? load_u64(&state->control->sequence)
        : 0;
}

} // namespace internal

namespace {

struct TransferredHandles final {
    explicit TransferredHandles(
        const SharedFrameBusRegistration& registration,
        bool take_ownership) noexcept
        : owns(take_ownership) {
        handles[count++] = decode_handle(registration.control_mapping_handle);
        handles[count++] = decode_handle(registration.publisher_process_handle);
        handles[count++] = decode_handle(registration.ready_fence_handle);
        handles[count++] = decode_handle(registration.done_fence_handle);
    }

    ~TransferredHandles() {
        if (!owns) return;
        for (std::size_t index = 0; index < count; ++index) {
            const HANDLE handle = handles[index];
            if (handle == nullptr) continue;
            bool duplicate = false;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (handles[previous] == handle) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) CloseHandle(handle);
        }
    }

    std::array<HANDLE, 4> handles{};
    std::size_t count = 0;
    bool owns = false;
};

bool registration_handles_valid(
    const SharedFrameBusRegistration& registration) noexcept {
    if (registration.control_mapping_handle == 0
        || registration.publisher_process_handle == 0
        || registration.ready_fence_handle == 0
        || registration.done_fence_handle == 0) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < registration.slot_count; ++slot) {
        if (registration.texture_handles[slot] == 0) return false;
    }
    return true;
}

void best_effort_deactivate(
    const SharedFrameBusRegistration& registration) noexcept {
    if (registration.control_mapping_handle == 0
        || registration.consumer_index >= shared_frame_bus_max_consumers
        || registration.consumer_token == 0) {
        return;
    }
    const std::size_t mapping_size = control_page_size(
        registration.protocol_version);
    if (mapping_size == 0) return;
    auto* page = static_cast<BusControlPageV1*>(MapViewOfFile(
        decode_handle(registration.control_mapping_handle),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, mapping_size));
    if (page == nullptr) return;
    if (page->magic == bus_magic
        && page->protocol_version == registration.protocol_version
        && page->structure_size == mapping_size
        && registration.consumer_index < page->max_consumers) {
        BusConsumerControl& shared = page->consumers[registration.consumer_index];
        if (load_u64(&shared.token) == registration.consumer_token) {
            close_consumer_access(
                shared, encode_access_tag(registration.consumer_token));
        }
    }
    UnmapViewOfFile(page);
}

} // namespace

struct SharedFrameBusConsumerState final {
    struct Acquisition final {
        std::uint64_t sequence = 0;
        SharedFrameBusFrameMetadata metadata{};
        SharedFrameBusFrameSideData side_data{};
        bool active = false;
        bool released = false;
    };

    struct AcquisitionOrder final {
        std::uint32_t slot = invalid_slot;
        std::uint64_t sequence = 0;
    };

    ~SharedFrameBusConsumerState() {
        {
            std::lock_guard lock(mutex);
            if (registered) {
                if (active_acquisitions != 0) {
                    std::uint64_t last_sequence = 0;
                    for (std::uint32_t index = 0;
                         index < acquisition_order_count;
                         ++index) {
                        last_sequence = acquisition_order[index].sequence;
                    }
                    const HRESULT hr = last_sequence == 0
                        ? E_UNEXPECTED
                        : context4->Signal(done_fence.Get(), last_sequence);
                    if (SUCCEEDED(hr)) {
                        context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
                        if (FAILED(device->GetDeviceRemovedReason())) {
                            quarantine_all_readers_locked();
                        } else {
                            clear_acquisitions_locked();
                        }
                    } else {
                        quarantine_all_readers_locked();
                    }
                }
                deactivate_locked();
            }
        }
        if (control != nullptr) UnmapViewOfFile(control);
        if (publisher_process != nullptr) CloseHandle(publisher_process);
    }

    void deactivate_locked() noexcept {
        if (!registered || control == nullptr
            || consumer_index >= shared_frame_bus_max_consumers) {
            closed = true;
            (void)InterlockedExchange(&closed_state, 1);
            return;
        }
        BusConsumerControl& shared = control->consumers[consumer_index];
        if (load_u64(&shared.token) == consumer_token) {
            close_consumer_access(shared, expected_access_tag);
        }
        closed = true;
        (void)InterlockedExchange(&closed_state, 1);
    }

    void clear_acquisitions_locked() noexcept {
        for (Acquisition& acquisition : acquisitions) acquisition = {};
        acquisition_order.fill({});
        acquisition_order_count = 0;
        active_acquisitions = 0;
    }

    void quarantine_all_readers_locked() noexcept {
        const LONG reader_bit = static_cast<LONG>(1UL << consumer_index);
        for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
            BusSlotControl& shared_slot = control->slots[slot];
            if ((load_long(&shared_slot.ownership) & reader_bit) == 0) continue;
            (void)InterlockedOr(&shared_slot.ownership, quarantine_bit);
            (void)InterlockedAnd(&shared_slot.ownership, ~reader_bit);
        }
        clear_acquisitions_locked();
    }

    GpuError initialize(
        ID3D11Device* source_device,
        const SharedFrameBusRegistration& registration,
        bool take_handle_ownership) noexcept {
        if (source_device == nullptr
            || registration.structure_size != sizeof(SharedFrameBusRegistration)
            || (registration.protocol_version != bus_protocol_version_v1
                && registration.protocol_version != bus_protocol_version_v2
                && registration.protocol_version != bus_protocol_version_v3
                && registration.protocol_version != bus_protocol_version_v4
                && registration.protocol_version != shared_frame_bus_protocol_version)
            || registration.width == 0
            || registration.height == 0
            || registration.format == DXGI_FORMAT_UNKNOWN
            || registration.slot_count < 2
            || registration.slot_count > shared_frame_bus_max_slots
            || registration.consumer_index >= shared_frame_bus_max_consumers
            || registration.consumer_token == 0
            || registration.target_process_id != GetCurrentProcessId()
            || !registration_handles_valid(registration)) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "invalid shared frame bus registration");
        }

        protocol_version = registration.protocol_version;
        const std::size_t mapping_size = control_page_size(protocol_version);
        control = static_cast<BusControlPageV1*>(MapViewOfFile(
            decode_handle(registration.control_mapping_handle),
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, mapping_size));
        if (control == nullptr) {
            return make_error(
                GpuStatus::system_error, HRESULT_FROM_WIN32(GetLastError()),
                "shared frame bus control mapping failed");
        }

        const auto fail_after_mapping = [&](GpuError error) noexcept {
            if (take_handle_ownership
                && registration.consumer_index < shared_frame_bus_max_consumers) {
                BusConsumerControl& shared =
                    control->consumers[registration.consumer_index];
                if (load_u64(&shared.token) == registration.consumer_token) {
                    close_consumer_access(
                        shared, encode_access_tag(registration.consumer_token));
                }
            }
            return error;
        };

        if (control->magic != bus_magic
            || control->protocol_version != protocol_version
            || control->structure_size != mapping_size
            || control->slot_count != registration.slot_count
            || control->width != registration.width
            || control->height != registration.height
            || control->format != registration.format
            || control->adapter_luid_low != registration.adapter_luid_low
            || control->adapter_luid_high != registration.adapter_luid_high
            || control->max_consumers == 0
            || control->max_consumers > shared_frame_bus_max_consumers
            || registration.consumer_index >= control->max_consumers) {
            return fail_after_mapping(make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "shared frame bus control page does not match registration"));
        }
        if (protocol_version >= bus_protocol_version_v2) {
            control_v2 = reinterpret_cast<BusControlPageV2*>(control);
        }
        if (protocol_version >= bus_protocol_version_v3) {
            control_v3 = reinterpret_cast<BusControlPageV3*>(control);
        }
        if (protocol_version >= bus_protocol_version_v4) {
            control_v4 = reinterpret_cast<BusControlPageV4*>(control);
        }
        if (load_long(&control->shutting_down) != 0) {
            return fail_after_mapping(make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus publisher is closed"));
        }
        BusConsumerControl& shared_consumer =
            control->consumers[registration.consumer_index];
        const std::uint64_t registered_access =
            load_u64(&shared_consumer.access_state);
        if ((registered_access & access_open_bit) == 0
            || (registered_access & access_tag_mask)
                != encode_access_tag(registration.consumer_token)
            || load_u64(&shared_consumer.token) != registration.consumer_token
            || static_cast<DWORD>(load_long(&shared_consumer.process_id))
                != registration.target_process_id) {
            return fail_after_mapping(make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus consumer registration is inactive"));
        }

        if (!DuplicateHandle(
                GetCurrentProcess(),
                decode_handle(registration.publisher_process_handle),
                GetCurrentProcess(), &publisher_process,
                SYNCHRONIZE, FALSE, 0)) {
            return fail_after_mapping(make_error(
                GpuStatus::system_error, HRESULT_FROM_WIN32(GetLastError()),
                "publisher process liveness handle duplication failed"));
        }
        const DWORD publisher_wait = WaitForSingleObject(publisher_process, 0);
        if (publisher_wait == WAIT_OBJECT_0 || publisher_wait == WAIT_FAILED) {
            const DWORD error = publisher_wait == WAIT_OBJECT_0
                ? ERROR_BROKEN_PIPE
                : GetLastError();
            return fail_after_mapping(make_error(
                GpuStatus::system_error, HRESULT_FROM_WIN32(error),
                "shared frame bus publisher is unavailable"));
        }

        device = source_device;
        HRESULT hr = device.As(&device5);
        if (FAILED(hr)) {
            return fail_after_mapping(make_error(
                GpuStatus::unsupported, hr,
                "D3D11 shared fences require ID3D11Device5"));
        }
        ComPtr<ID3D11DeviceContext> base_context;
        device->GetImmediateContext(&base_context);
        hr = base_context.As(&context4);
        if (FAILED(hr)) {
            return fail_after_mapping(make_error(
                GpuStatus::unsupported, hr,
                "D3D11 shared fences require ID3D11DeviceContext4"));
        }
        ComPtr<ID3D10Multithread> multithread;
        hr = base_context.As(&multithread);
        if (FAILED(hr)) {
            return fail_after_mapping(make_error(
                GpuStatus::unsupported, hr,
                "D3D11 immediate-context synchronization is unavailable"));
        }
        (void)multithread->SetMultithreadProtected(TRUE);

        LUID local_luid{};
        hr = query_adapter_luid(device.Get(), local_luid);
        if (FAILED(hr)) {
            return fail_after_mapping(make_error(
                GpuStatus::system_error, hr,
                "consumer adapter LUID query failed"));
        }
        if (local_luid.LowPart != registration.adapter_luid_low
            || local_luid.HighPart != registration.adapter_luid_high) {
            return fail_after_mapping(make_error(
                GpuStatus::unsupported, E_INVALIDARG,
                "shared frame bus belongs to a different GPU adapter"));
        }

        hr = device5->OpenSharedFence(
            decode_handle(registration.ready_fence_handle),
            IID_PPV_ARGS(&ready_fence));
        if (FAILED(hr)) {
            return fail_after_mapping(make_error(
                GpuStatus::system_error, hr,
                "shared ready fence open failed"));
        }
        hr = device5->OpenSharedFence(
            decode_handle(registration.done_fence_handle),
            IID_PPV_ARGS(&done_fence));
        if (FAILED(hr)) {
            return fail_after_mapping(make_error(
                GpuStatus::system_error, hr,
                "consumer done fence open failed"));
        }

        for (std::uint32_t slot = 0; slot < registration.slot_count; ++slot) {
            hr = device->OpenSharedResource(
                decode_handle(registration.texture_handles[slot]),
                IID_PPV_ARGS(&textures[slot]));
            if (FAILED(hr)) {
                return fail_after_mapping(make_error(
                    GpuStatus::system_error, hr,
                    "shared frame bus texture open failed"));
            }
            D3D11_TEXTURE2D_DESC description{};
            textures[slot]->GetDesc(&description);
            if (description.Width != registration.width
                || description.Height != registration.height
                || description.Format != static_cast<DXGI_FORMAT>(registration.format)
                || description.MipLevels != 1
                || description.ArraySize != 1
                || description.SampleDesc.Count != 1) {
                return fail_after_mapping(make_error(
                    GpuStatus::invalid_argument, E_INVALIDARG,
                    "opened shared frame bus texture does not match registration"));
            }
        }

        width = registration.width;
        height = registration.height;
        format = static_cast<DXGI_FORMAT>(registration.format);
        slot_count = registration.slot_count;
        consumer_index = registration.consumer_index;
        consumer_token = registration.consumer_token;
        expected_access_tag = encode_access_tag(registration.consumer_token);
        registered = true;
        return success();
    }

    bool registration_active_locked() const noexcept {
        if (!registered || closed || control == nullptr) return false;
        const BusConsumerControl& shared = control->consumers[consumer_index];
        const std::uint64_t access = load_u64(
            const_cast<volatile LONG64*>(&shared.access_state));
        return (access & access_open_bit) != 0
            && (access & access_tag_mask) == expected_access_tag
            && load_u64(const_cast<volatile LONG64*>(&shared.token)) == consumer_token;
    }

    bool enter_consumer_access() noexcept {
        BusConsumerControl& shared = control->consumers[consumer_index];
        if (load_u64(&shared.token) != consumer_token) return false;
        std::uint64_t state = load_u64(&shared.access_state);
        for (;;) {
            if ((state & access_tag_mask) != expected_access_tag
                || (state & access_open_bit) == 0
                || (state & access_ref_mask) == access_ref_mask) {
                return false;
            }
            const std::uint64_t previous = static_cast<std::uint64_t>(
                InterlockedCompareExchange64(
                    &shared.access_state,
                    static_cast<LONG64>(state + 1),
                    static_cast<LONG64>(state)));
            if (previous == state) {
                if (load_u64(&shared.token) == consumer_token) return true;
                (void)InterlockedDecrement64(&shared.access_state);
                return false;
            }
            state = previous;
        }
    }

    void leave_consumer_access() noexcept {
        (void)InterlockedDecrement64(
            &control->consumers[consumer_index].access_state);
    }

    bool all_slots_quarantined() const noexcept {
        for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
            if ((load_long(&control->slots[slot].ownership) & quarantine_bit) == 0) {
                return false;
            }
        }
        return true;
    }

    GpuError acquire_latest(
        std::uint32_t timeout_ms, SharedFrameBusFrameLease& output,
        const std::shared_ptr<SharedFrameBusConsumerState>& self) noexcept {
        std::lock_guard lock(mutex);
        if (output.state_ != nullptr) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "output frame lease is not empty");
        }
        if (!registration_active_locked()) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus consumer is closed or stale");
        }

        const LONG bit = static_cast<LONG>(1UL << consumer_index);
        const ULONGLONG start = GetTickCount64();
        std::uint32_t iteration = 0;
        for (;;) {
            if (!enter_consumer_access()) {
                return make_error(
                    GpuStatus::invalid_argument, E_UNEXPECTED,
                    "shared frame bus consumer is closed or stale");
            }
            if (load_long(&close_requested) != 0) {
                leave_consumer_access();
                return make_error(
                    GpuStatus::invalid_argument, E_UNEXPECTED,
                    "shared frame bus consumer close was requested");
            }
            const DWORD publisher_wait = WaitForSingleObject(publisher_process, 0);
            if (publisher_wait == WAIT_OBJECT_0 || publisher_wait == WAIT_FAILED) {
                const DWORD error = publisher_wait == WAIT_OBJECT_0
                    ? ERROR_BROKEN_PIPE
                    : GetLastError();
                leave_consumer_access();
                return make_error(
                    GpuStatus::system_error, HRESULT_FROM_WIN32(error),
                    "shared frame bus publisher process exited");
            }
            if (load_long(&control->shutting_down) != 0) {
                leave_consumer_access();
                return make_error(
                    GpuStatus::invalid_argument, E_UNEXPECTED,
                    "shared frame bus publisher is closed");
            }

            std::uint32_t candidate = invalid_slot;
            std::uint64_t newest = observed_sequence;
            for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
                BusSlotControl& shared_slot = control->slots[slot];
                const LONG ownership = load_long(&shared_slot.ownership);
                if (slot_unavailable(ownership)) continue;
                const std::uint64_t sequence = load_u64(&shared_slot.sequence);
                if (sequence > newest) {
                    newest = sequence;
                    candidate = slot;
                }
            }

            if (candidate != invalid_slot) {
                BusSlotControl& shared_slot = control->slots[candidate];
                LONG ownership = load_long(&shared_slot.ownership);
                while (!slot_unavailable(ownership)) {
                    if ((ownership & bit) != 0) break;
                    const LONG desired = ownership | bit;
                    const LONG previous = InterlockedCompareExchange(
                        &shared_slot.ownership, desired, ownership);
                    if (previous == ownership) {
                        const std::uint64_t stable_sequence =
                            load_u64(&shared_slot.sequence);
                        if (stable_sequence != newest || stable_sequence <= observed_sequence) {
                            (void)InterlockedAnd(&shared_slot.ownership, ~bit);
                            break;
                        }
                        if (!registration_active_locked()) {
                            (void)InterlockedAnd(&shared_slot.ownership, ~bit);
                            leave_consumer_access();
                            return make_error(
                                GpuStatus::invalid_argument, E_UNEXPECTED,
                                "shared frame bus consumer was unregistered");
                        }
                        SharedFrameBusFrameMetadata metadata;
                        if (control_v2 != nullptr) {
                            BusSlotMetadataV2& shared_metadata =
                                control_v2->metadata[candidate];
                            if (load_u64(&shared_metadata.sequence)
                                    != stable_sequence
                                || !normalize_metadata(
                                    shared_metadata.payload,
                                     width,
                                     height,
                                    metadata,
                                    DXGI_COLOR_SPACE_CUSTOM,
                                    protocol_version
                                        >= shared_frame_bus_protocol_version)) {
                                (void)InterlockedOr(
                                    &shared_slot.ownership, quarantine_bit);
                                (void)InterlockedAnd(
                                    &shared_slot.ownership, ~bit);
                                leave_consumer_access();
                                deactivate_locked();
                                return make_error(
                                    GpuStatus::system_error, E_ABORT,
                                    "shared frame bus slot metadata is corrupt; recreate the bus");
                            }
                        }
                        SharedFrameBusFrameSideData side_data;
                        if (control_v3 != nullptr) {
                            BusSlotSideDataV3& shared_side_data =
                                control_v3->side_data[candidate];
                            const std::uint64_t before =
                                load_u64(&shared_side_data.sequence);
                            const SharedFrameBusFrameSideData payload =
                                shared_side_data.payload;
                            MemoryBarrier();
                            const std::uint64_t after =
                                load_u64(&shared_side_data.sequence);
                            if (before != stable_sequence
                                || after != stable_sequence
                                || !normalize_side_data(
                                    payload,
                                    width,
                                    height,
                                    protocol_version,
                                    stable_sequence,
                                    side_data)) {
                                (void)InterlockedOr(
                                    &shared_slot.ownership, quarantine_bit);
                                (void)InterlockedAnd(
                                    &shared_slot.ownership, ~bit);
                                leave_consumer_access();
                                deactivate_locked();
                                return make_error(
                                    GpuStatus::system_error, E_ABORT,
                                    "shared frame bus slot side data is corrupt; recreate the bus");
                            }
                        }
                        const HRESULT hr = context4->Wait(
                            ready_fence.Get(), stable_sequence);
                        if (FAILED(hr)) {
                            (void)InterlockedAnd(&shared_slot.ownership, ~bit);
                            leave_consumer_access();
                            deactivate_locked();
                            return fence_error(
                                device.Get(), hr, "ready fence wait failed");
                        }
                        Acquisition& acquisition = acquisitions[candidate];
                        if (acquisition.active) {
                            (void)InterlockedOr(
                                &shared_slot.ownership, quarantine_bit);
                            (void)InterlockedAnd(
                                &shared_slot.ownership, ~bit);
                            leave_consumer_access();
                            deactivate_locked();
                            return make_error(
                                GpuStatus::system_error, E_ABORT,
                                "shared frame bus slot was acquired twice; recreate the bus");
                        }
                        acquisition.sequence = stable_sequence;
                        acquisition.metadata = metadata;
                        acquisition.side_data = side_data;
                        acquisition.active = true;
                        acquisition.released = false;
                        if (acquisition_order_count >= acquisition_order.size()) {
                            acquisition = {};
                            (void)InterlockedOr(
                                &shared_slot.ownership, quarantine_bit);
                            (void)InterlockedAnd(
                                &shared_slot.ownership, ~bit);
                            leave_consumer_access();
                            deactivate_locked();
                            return make_error(
                                GpuStatus::system_error, E_ABORT,
                                "shared frame bus acquisition order overflowed");
                        }
                        acquisition_order[acquisition_order_count++] = {
                            candidate, stable_sequence};
                        ++active_acquisitions;
                        observed_sequence = stable_sequence;
                        output.state_ = self;
                        output.texture_ = textures[candidate].Get();
                        output.info_.slot_index = candidate;
                        output.info_.width = width;
                        output.info_.height = height;
                        output.info_.format = format;
                        output.info_.sequence = stable_sequence;
                        leave_consumer_access();
                        return success();
                    }
                    ownership = previous;
                }
            }

            leave_consumer_access();
            if (all_slots_quarantined()) {
                return make_error(
                    GpuStatus::system_error, E_ABORT,
                    "all shared frame bus slots are quarantined; recreate the bus");
            }
            if (timeout_ms == 0 || timeout_elapsed(start, timeout_ms)) {
                return timeout_error("no newer shared frame is available");
            }
            wait_backoff(iteration);
        }
    }

    GpuError release(std::uint32_t slot, std::uint64_t sequence) noexcept {
        std::lock_guard lock(mutex);
        if (slot >= slot_count) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus frame lease is stale");
        }
        Acquisition& released = acquisitions[slot];
        if (!released.active
            || released.released
            || released.sequence != sequence) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus frame lease is stale");
        }
        released.released = true;

        std::uint32_t retired_count = 0;
        std::uint64_t retired_sequence = 0;
        while (retired_count < acquisition_order_count) {
            const AcquisitionOrder entry = acquisition_order[retired_count];
            if (entry.slot >= slot_count) {
                quarantine_all_readers_locked();
                deactivate_locked();
                return make_error(
                    GpuStatus::system_error, E_ABORT,
                    "shared frame bus acquisition order is corrupt");
            }
            Acquisition& candidate = acquisitions[entry.slot];
            if (!candidate.active
                || candidate.sequence != entry.sequence) {
                quarantine_all_readers_locked();
                deactivate_locked();
                return make_error(
                    GpuStatus::system_error, E_ABORT,
                    "shared frame bus acquisition record is corrupt");
            }
            if (!candidate.released) break;
            retired_sequence = candidate.sequence;
            candidate = {};
            --active_acquisitions;
            ++retired_count;
        }

        if (retired_count == 0) return success();
        for (std::uint32_t index = retired_count;
             index < acquisition_order_count;
             ++index) {
            acquisition_order[index - retired_count] =
                acquisition_order[index];
        }
        for (std::uint32_t index = acquisition_order_count - retired_count;
             index < acquisition_order_count;
             ++index) {
            acquisition_order[index] = {};
        }
        acquisition_order_count -= retired_count;

        const HRESULT hr = context4->Signal(
            done_fence.Get(), retired_sequence);
        if (FAILED(hr)) {
            quarantine_all_readers_locked();
            deactivate_locked();
            return fence_error(device.Get(), hr, "consumer done fence signal failed");
        }
        context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            quarantine_all_readers_locked();
            deactivate_locked();
            return make_error(
                GpuStatus::device_lost, removed,
                "consumer device was removed while signaling the done fence");
        }
        return success();
    }

    SharedFrameBusFrameMetadata metadata(
        std::uint32_t slot, std::uint64_t sequence) noexcept {
        std::lock_guard lock(mutex);
        return slot < slot_count
                && acquisitions[slot].active
                && acquisitions[slot].sequence == sequence
            ? acquisitions[slot].metadata
            : SharedFrameBusFrameMetadata{};
    }

    SharedFrameBusFrameSideData side_data(
        std::uint32_t slot, std::uint64_t sequence) noexcept {
        std::lock_guard lock(mutex);
        return slot < slot_count
                && acquisitions[slot].active
                && acquisitions[slot].sequence == sequence
            ? acquisitions[slot].side_data
            : SharedFrameBusFrameSideData{};
    }

    GpuError cursor_shape(
        std::uint64_t shape_sequence,
        WgcCursorShape& output) noexcept {
        output = {};
        if (shape_sequence == 0) {
            return make_error(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "cursor shape sequence is zero");
        }
        std::lock_guard lock(mutex);
        if (!registration_active_locked()) {
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "shared frame bus consumer is closed or stale");
        }
        if (control_v3 == nullptr) {
            return make_error(
                GpuStatus::timeout, HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
                "shared cursor shapes require bus protocol v3");
        }

        for (BusCursorShapeSlotV3& slot : control_v3->cursor_shapes) {
            for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
                const std::uint64_t before = load_u64(&slot.commit_sequence);
                if (before == 0 || (before & 1u) != 0) break;
                const std::uint64_t candidate_sequence = slot.shape_sequence;
                if (candidate_sequence != shape_sequence) break;

                WgcCursorShape candidate;
                candidate.sequence = candidate_sequence;
                candidate.kind = static_cast<WgcCursorShapeKind>(slot.kind);
                candidate.width = slot.width;
                candidate.height = slot.height;
                candidate.hotspot_x = slot.hotspot_x;
                candidate.hotspot_y = slot.hotspot_y;
                candidate.stride_bytes = slot.stride_bytes;
                const std::uint32_t data_size = slot.data_size;
                const std::uint32_t reserved = slot.reserved;
                if (data_size <= shared_frame_bus_max_cursor_shape_bytes) {
                    candidate.data.assign(
                        slot.data.begin(), slot.data.begin() + data_size);
                }
                MemoryBarrier();
                const std::uint64_t after = load_u64(&slot.commit_sequence);
                if (before != after || (after & 1u) != 0) continue;
                if (reserved != 0
                    || data_size > shared_frame_bus_max_cursor_shape_bytes
                    || !validate_cursor_shape(candidate)) {
                    return make_error(
                        GpuStatus::system_error, E_ABORT,
                        "shared cursor shape cache entry is corrupt");
                }
                output = std::move(candidate);
                return success();
            }
        }
        return make_error(
            GpuStatus::timeout, HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
            "shared cursor shape is no longer cached");
    }

    GpuError move_result(
        std::uint64_t epoch,
        std::uint64_t epoch_nonce,
        std::uint64_t sequence,
        SharedFrameBusMoveResult& output) noexcept {
        output = {};
        if (epoch == 0 || epoch_nonce == 0 || sequence == 0) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "shared move-result key is incomplete");
        }
        std::lock_guard lock(mutex);
        if (!registration_active_locked()) {
            return make_error(
                GpuStatus::invalid_argument,
                E_UNEXPECTED,
                "shared frame bus consumer is closed or stale");
        }
        if (control_v4 == nullptr) {
            return make_error(
                GpuStatus::unsupported,
                E_NOTIMPL,
                "shared move results require bus protocol v4 or newer");
        }

        for (BusMoveResultSlotV4& slot : control_v4->move_results) {
            for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
                const std::uint64_t before = load_u64(&slot.commit_sequence);
                if (before == 0 || (before & 1u) != 0) break;
                const SharedFrameBusMoveResult candidate = slot.payload;
                const auto reserved = slot.reserved;
                MemoryBarrier();
                const std::uint64_t after = load_u64(&slot.commit_sequence);
                if (before != after || (after & 1u) != 0) continue;
                if (candidate.epoch != epoch
                    || candidate.epoch_nonce != epoch_nonce
                    || candidate.sequence != sequence) {
                    break;
                }
                SharedFrameBusMoveResult normalized;
                if (std::any_of(
                        reserved.begin(), reserved.end(),
                        [](std::uint8_t value) { return value != 0; })
                    || !normalize_move_result(
                        candidate, width, height, normalized)) {
                    return make_error(
                        GpuStatus::system_error,
                        E_ABORT,
                        "shared move-result cache entry is corrupt");
                }
                output = normalized;
                return success();
            }
        }
        return make_error(
            GpuStatus::timeout,
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
            "shared move result is pending or no longer cached");
    }

    GpuError close_explicit() noexcept {
        (void)InterlockedExchange(&close_requested, 1);
        std::lock_guard lock(mutex);
        if (closed) return success();
        if (active_acquisitions != 0) {
            (void)InterlockedExchange(&close_requested, 0);
            return make_error(
                GpuStatus::invalid_argument, E_UNEXPECTED,
                "release every acquired frame before closing the consumer");
        }
        deactivate_locked();
        return success();
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> ready_fence;
    ComPtr<ID3D11Fence> done_fence;
    std::array<ComPtr<ID3D11Texture2D>, shared_frame_bus_max_slots> textures;
    BusControlPageV1* control = nullptr;
    BusControlPageV2* control_v2 = nullptr;
    BusControlPageV3* control_v3 = nullptr;
    BusControlPageV4* control_v4 = nullptr;
    HANDLE publisher_process = nullptr;
    std::mutex mutex;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t slot_count = 0;
    std::uint32_t consumer_index = 0;
    std::uint64_t consumer_token = 0;
    std::uint64_t expected_access_tag = 0;
    std::uint32_t protocol_version = 0;
    std::uint64_t observed_sequence = 0;
    std::array<Acquisition, shared_frame_bus_max_slots> acquisitions{};
    std::array<AcquisitionOrder, shared_frame_bus_max_slots>
        acquisition_order{};
    std::uint32_t acquisition_order_count = 0;
    std::uint32_t active_acquisitions = 0;
    volatile LONG close_requested = 0;
    volatile LONG closed_state = 0;
    bool registered = false;
    bool closed = false;
};

SharedFrameBusWriteLease::SharedFrameBusWriteLease() noexcept = default;
SharedFrameBusWriteLease::~SharedFrameBusWriteLease() { reset(); }

SharedFrameBusWriteLease::SharedFrameBusWriteLease(
    SharedFrameBusWriteLease&& other) noexcept
    : state_(std::move(other.state_)),
      texture_(std::exchange(other.texture_, nullptr)),
      token_(std::exchange(other.token_, 0)),
      sequence_(std::exchange(other.sequence_, 0)),
      slot_(std::exchange(other.slot_, 0)) {}

SharedFrameBusWriteLease& SharedFrameBusWriteLease::operator=(
    SharedFrameBusWriteLease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    state_ = std::move(other.state_);
    texture_ = std::exchange(other.texture_, nullptr);
    token_ = std::exchange(other.token_, 0);
    sequence_ = std::exchange(other.sequence_, 0);
    slot_ = std::exchange(other.slot_, 0);
    return *this;
}

SharedFrameBusWriteLease::operator bool() const noexcept {
    return state_ != nullptr && texture_ != nullptr && token_ != 0;
}

ID3D11Texture2D* SharedFrameBusWriteLease::texture() const noexcept {
    return texture_;
}

std::uint32_t SharedFrameBusWriteLease::slot_index() const noexcept {
    return slot_;
}

std::uint64_t SharedFrameBusWriteLease::sequence() const noexcept {
    return sequence_;
}

void SharedFrameBusWriteLease::reset() noexcept {
    if (state_ != nullptr && token_ != 0) {
        state_->cancel_write(slot_, token_);
    }
    state_.reset();
    texture_ = nullptr;
    token_ = 0;
    sequence_ = 0;
    slot_ = 0;
}

SharedFrameBusFrameLease::SharedFrameBusFrameLease() noexcept = default;
SharedFrameBusFrameLease::~SharedFrameBusFrameLease() { reset(); }

SharedFrameBusFrameLease::SharedFrameBusFrameLease(
    SharedFrameBusFrameLease&& other) noexcept
    : state_(std::move(other.state_)),
      texture_(std::exchange(other.texture_, nullptr)),
      info_(std::exchange(other.info_, {})) {}

SharedFrameBusFrameLease& SharedFrameBusFrameLease::operator=(
    SharedFrameBusFrameLease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    state_ = std::move(other.state_);
    texture_ = std::exchange(other.texture_, nullptr);
    info_ = std::exchange(other.info_, {});
    return *this;
}

SharedFrameBusFrameLease::operator bool() const noexcept {
    return state_ != nullptr && texture_ != nullptr && info_.sequence != 0;
}

ID3D11Texture2D* SharedFrameBusFrameLease::texture() const noexcept {
    return texture_;
}

const SharedFrameBusFrameInfo& SharedFrameBusFrameLease::info() const noexcept {
    return info_;
}

SharedFrameBusFrameMetadata SharedFrameBusFrameLease::metadata() const noexcept {
    if (state_ == nullptr || info_.sequence == 0) return {};
    try {
        return state_->metadata(info_.slot_index, info_.sequence);
    } catch (...) {
        return {};
    }
}

SharedFrameBusFrameSideData SharedFrameBusFrameLease::side_data() const noexcept {
    if (state_ == nullptr || info_.sequence == 0) return {};
    try {
        return state_->side_data(info_.slot_index, info_.sequence);
    } catch (...) {
        return {};
    }
}

GpuError SharedFrameBusFrameLease::release() noexcept {
    if (state_ == nullptr || info_.sequence == 0) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus frame lease is empty");
    }
    GpuError result;
    try {
        result = state_->release(info_.slot_index, info_.sequence);
    } catch (...) {
        result = make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared frame lease release failure");
    }
    state_.reset();
    texture_ = nullptr;
    info_ = {};
    return result;
}

void SharedFrameBusFrameLease::reset() noexcept {
    if (state_ != nullptr && info_.sequence != 0) {
        (void)release();
        return;
    }
    state_.reset();
    texture_ = nullptr;
    info_ = {};
}

SharedFrameBusPublisher::SharedFrameBusPublisher() noexcept = default;
SharedFrameBusPublisher::~SharedFrameBusPublisher() = default;
SharedFrameBusPublisher::SharedFrameBusPublisher(
    SharedFrameBusPublisher&&) noexcept = default;
SharedFrameBusPublisher& SharedFrameBusPublisher::operator=(
    SharedFrameBusPublisher&&) noexcept = default;

GpuError SharedFrameBusPublisher::create(
    ID3D11Device* device,
    const SharedFrameBusConfig& config,
    SharedFrameBusPublisher& output) noexcept {
    try {
        auto state = std::make_shared<SharedFrameBusPublisherState>();
        const GpuError initialized = state->initialize(device, config);
        if (!initialized) return initialized;
        output.state_ = std::move(state);
        return success();
    } catch (const std::bad_alloc&) {
        return make_error(
            GpuStatus::out_of_memory, E_OUTOFMEMORY,
            "shared frame bus allocation failed");
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared frame bus creation failure");
    }
}

GpuError SharedFrameBusPublisher::register_consumer(
    HANDLE target_process,
    SharedFrameBusRegistration& output) noexcept {
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state_->register_consumer(target_process, output);
    } catch (const std::bad_alloc&) {
        return make_error(
            GpuStatus::out_of_memory, E_OUTOFMEMORY,
            "consumer registration allocation failed");
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown consumer registration failure");
    }
}

GpuError SharedFrameBusPublisher::unregister_consumer(
    const SharedFrameBusRegistration& registration,
    std::uint32_t timeout_ms) noexcept {
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state_->unregister_consumer(registration, timeout_ms);
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown consumer unregistration failure");
    }
}

GpuError SharedFrameBusPublisher::publish(
    ID3D11Texture2D* source,
    std::uint32_t timeout_ms) noexcept {
    return publish(
        source,
        SharedFrameBusFrameMetadata{},
        SharedFrameBusFrameSideData{},
        timeout_ms);
}

GpuError SharedFrameBusPublisher::publish(
    ID3D11Texture2D* source,
    const SharedFrameBusFrameMetadata& metadata,
    std::uint32_t timeout_ms) noexcept {
    return publish(
        source, metadata, SharedFrameBusFrameSideData{}, timeout_ms);
}

GpuError SharedFrameBusPublisher::publish(
    ID3D11Texture2D* source,
    const SharedFrameBusFrameMetadata& metadata,
    const SharedFrameBusFrameSideData& side_data,
    std::uint32_t timeout_ms) noexcept {
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state_->publish(source, metadata, side_data, timeout_ms);
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared frame publish failure");
    }
}

GpuError SharedFrameBusPublisher::begin_publish(
    std::uint32_t timeout_ms,
    SharedFrameBusWriteLease& output) noexcept {
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state_->begin_publish(timeout_ms, output, state_);
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown direct publish begin failure");
    }
}

GpuError SharedFrameBusPublisher::commit(
    SharedFrameBusWriteLease&& lease) noexcept {
    return commit(
        std::move(lease),
        SharedFrameBusFrameMetadata{},
        SharedFrameBusFrameSideData{});
}

GpuError SharedFrameBusPublisher::commit(
    SharedFrameBusWriteLease&& lease,
    const SharedFrameBusFrameMetadata& metadata) noexcept {
    return commit(
        std::move(lease), metadata, SharedFrameBusFrameSideData{});
}

GpuError SharedFrameBusPublisher::commit(
    SharedFrameBusWriteLease&& lease,
    const SharedFrameBusFrameMetadata& metadata,
    const SharedFrameBusFrameSideData& side_data) noexcept {
    if (state_ == nullptr || lease.state_.get() != state_.get()) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "write lease does not belong to this shared frame bus");
    }
    GpuError result;
    try {
        std::lock_guard lock(state_->mutex);
        result = state_->commit_write_locked(
            lease.slot_, lease.token_, true, metadata, side_data);
    } catch (...) {
        result = make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown direct publish commit failure");
    }
    lease.state_.reset();
    lease.texture_ = nullptr;
    lease.token_ = 0;
    lease.sequence_ = 0;
    lease.slot_ = 0;
    return result;
}

GpuError SharedFrameBusPublisher::publish_cursor_shape(
    const WgcCursorShape& shape) noexcept {
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state_->publish_cursor_shape(0, shape);
    } catch (const std::bad_alloc&) {
        return make_error(
            GpuStatus::out_of_memory, E_OUTOFMEMORY,
            "shared cursor shape allocation failed");
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared cursor shape publish failure");
    }
}

GpuError SharedFrameBusPublisher::publish_move_result(
    const SharedFrameBusMoveResult& result) noexcept {
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "shared frame bus publisher is uninitialized");
    }
    try {
        return state_->publish_move_result(0, result);
    } catch (...) {
        return make_error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown shared move-result publish failure");
    }
}

SharedFrameBusConfig SharedFrameBusPublisher::config() const noexcept {
    return state_ != nullptr ? state_->config : SharedFrameBusConfig{};
}

std::uint64_t SharedFrameBusPublisher::sequence() const noexcept {
    return state_ != nullptr && state_->control != nullptr
        ? load_u64(&state_->control->sequence)
        : 0;
}

SharedFrameBusStats SharedFrameBusPublisher::stats() const noexcept {
    if (state_ == nullptr) return {};
    try {
        return state_->stats();
    } catch (...) {
        return {};
    }
}

bool SharedFrameBusPublisher::initialized() const noexcept {
    return state_ != nullptr;
}

SharedFrameBusConsumer::SharedFrameBusConsumer() noexcept = default;
SharedFrameBusConsumer::~SharedFrameBusConsumer() = default;
SharedFrameBusConsumer::SharedFrameBusConsumer(
    SharedFrameBusConsumer&&) noexcept = default;
SharedFrameBusConsumer& SharedFrameBusConsumer::operator=(
    SharedFrameBusConsumer&&) noexcept = default;

GpuError SharedFrameBusConsumer::open(
    ID3D11Device* device,
    const SharedFrameBusRegistration& registration,
    bool take_handle_ownership,
    SharedFrameBusConsumer& output) noexcept {
    TransferredHandles transferred(registration, take_handle_ownership);
    try {
        auto state = std::make_shared<SharedFrameBusConsumerState>();
        const GpuError opened = state->initialize(
            device, registration, take_handle_ownership);
        if (!opened) {
            if (take_handle_ownership) best_effort_deactivate(registration);
            return opened;
        }
        output.state_ = std::move(state);
        return success();
    } catch (const std::bad_alloc&) {
        if (take_handle_ownership) best_effort_deactivate(registration);
        return make_error(
            GpuStatus::out_of_memory, E_OUTOFMEMORY,
            "shared frame bus consumer allocation failed");
    } catch (...) {
        if (take_handle_ownership) best_effort_deactivate(registration);
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared frame bus consumer open failure");
    }
}

GpuError SharedFrameBusConsumer::acquire_latest(
    std::uint32_t timeout_ms,
    SharedFrameBusFrameLease& output) noexcept {
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus consumer is uninitialized");
    }
    try {
        return state_->acquire_latest(timeout_ms, output, state_);
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared frame acquire failure");
    }
}

GpuError SharedFrameBusConsumer::release(
    SharedFrameBusFrameLease& lease) noexcept {
    if (state_ == nullptr || lease.state_.get() != state_.get()) {
        return make_error(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "frame lease does not belong to this shared frame bus consumer");
    }
    return lease.release();
}

GpuError SharedFrameBusConsumer::cursor_shape(
    std::uint64_t shape_sequence,
    WgcCursorShape& output) const noexcept {
    output = {};
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument, E_UNEXPECTED,
            "shared frame bus consumer is uninitialized");
    }
    try {
        return state_->cursor_shape(shape_sequence, output);
    } catch (const std::bad_alloc&) {
        return make_error(
            GpuStatus::out_of_memory, E_OUTOFMEMORY,
            "shared cursor shape allocation failed");
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared cursor shape read failure");
    }
}

GpuError SharedFrameBusConsumer::try_get_move_result(
    std::uint64_t epoch,
    std::uint64_t epoch_nonce,
    std::uint64_t sequence,
    SharedFrameBusMoveResult& output) const noexcept {
    output = {};
    if (state_ == nullptr) {
        return make_error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "shared frame bus consumer is uninitialized");
    }
    try {
        return state_->move_result(epoch, epoch_nonce, sequence, output);
    } catch (...) {
        return make_error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown shared move-result read failure");
    }
}

GpuError SharedFrameBusConsumer::close() noexcept {
    if (state_ == nullptr) return success();
    try {
        const GpuError result = state_->close_explicit();
        return result;
    } catch (...) {
        return make_error(
            GpuStatus::system_error, E_FAIL,
            "unknown shared frame bus consumer close failure");
    }
}

std::uint32_t SharedFrameBusConsumer::consumer_index() const noexcept {
    return state_ != nullptr ? state_->consumer_index : 0;
}

bool SharedFrameBusConsumer::initialized() const noexcept {
    return state_ != nullptr && load_long(&state_->closed_state) == 0;
}

} // namespace fluxcap::gpu
