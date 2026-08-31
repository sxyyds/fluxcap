#ifndef FLUXCAP_GPU_DESKTOP_DUPLICATION_CAPTURE_HPP
#define FLUXCAP_GPU_DESKTOP_DUPLICATION_CAPTURE_HPP
#include <fluxcap/gpu.hpp>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace fluxcap::gpu::internal {

// Resolves a desktop-attached monitor to its owning adapter/output. Returned
// interfaces carry one reference and may be null when the corresponding output
// pointer is null.
[[nodiscard]] HRESULT find_monitor_output(
    HMONITOR, IDXGIAdapter1**, IDXGIOutput1**,
    DXGI_OUTPUT_DESC* = nullptr) noexcept;

inline std::uint64_t desktop_duplication_cursor_shape_key(
    const WgcCursorShape& shape) noexcept {
    constexpr std::uint64_t offset = 1469598103934665603ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t value = offset;
    const auto append = [&](const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            value ^= bytes[index];
            value *= prime;
        }
    };
    const auto kind = static_cast<std::underlying_type_t<WgcCursorShapeKind>>(
        shape.kind);
    append(&kind, sizeof(kind));
    append(&shape.width, sizeof(shape.width));
    append(&shape.height, sizeof(shape.height));
    append(&shape.hotspot_x, sizeof(shape.hotspot_x));
    append(&shape.hotspot_y, sizeof(shape.hotspot_y));
    append(&shape.stride_bytes, sizeof(shape.stride_bytes));
    append(shape.data.data(), shape.data.size());
    return value == 0 ? 1 : value;
}

inline bool desktop_duplication_win32_shape_fallback_allowed(
    bool native_shape_seen,
    bool pending_native_shape) noexcept {
    return !native_shape_seen && !pending_native_shape;
}

// DXGI_OUTDUPL_POINTER_POSITION names the shape's top-left. FluxCap's public
// cursor contract names the hot spot, matching GetCursorInfo.
inline constexpr bool desktop_duplication_pointer_hotspot(
    std::int32_t top_left_x,
    std::int32_t top_left_y,
    std::uint32_t hotspot_x,
    std::uint32_t hotspot_y,
    std::int32_t& output_x,
    std::int32_t& output_y) noexcept {
    const std::int64_t x = static_cast<std::int64_t>(top_left_x) + hotspot_x;
    const std::int64_t y = static_cast<std::int64_t>(top_left_y) + hotspot_y;
    if (x < std::numeric_limits<std::int32_t>::min()
        || x > std::numeric_limits<std::int32_t>::max()
        || y < std::numeric_limits<std::int32_t>::min()
        || y > std::numeric_limits<std::int32_t>::max()) {
        output_x = 0;
        output_y = 0;
        return false;
    }
    output_x = static_cast<std::int32_t>(x);
    output_y = static_cast<std::int32_t>(y);
    return true;
}

inline constexpr bool desktop_duplication_planar_move_aligned(
    const WgcMoveRect& move) noexcept {
    if (move.source_x < 0 || move.source_y < 0
        || move.destination.x < 0 || move.destination.y < 0
        || move.destination.width == 0 || move.destination.height == 0) {
        return false;
    }
    return ((static_cast<std::uint32_t>(move.source_x)
                | static_cast<std::uint32_t>(move.source_y)
                | static_cast<std::uint32_t>(move.destination.x)
                | static_cast<std::uint32_t>(move.destination.y)
                | move.destination.width
        | move.destination.height) & 1u) == 0;
}

inline constexpr void desktop_duplication_full_damage(
    WgcFrameDamage& damage,
    std::uint32_t width,
    std::uint32_t height,
    bool overflow) noexcept {
    damage = {};
    damage.flags = wgc_damage_valid
        | wgc_damage_native
        | wgc_damage_native_move_available
        | wgc_damage_full_frame;
    if (overflow) damage.flags |= wgc_damage_overflow;
    damage.dirty_count = 1;
    damage.dirty_rects[0] = {0, 0, width, height};
}

inline constexpr bool desktop_duplication_rotation_dimensions(
    DXGI_MODE_ROTATION rotation,
    std::uint32_t surface_width,
    std::uint32_t surface_height,
    std::uint32_t& logical_width,
    std::uint32_t& logical_height) noexcept {
    if (surface_width == 0 || surface_height == 0
        || surface_width
            > static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())
        || surface_height
            > static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())) {
        logical_width = 0;
        logical_height = 0;
        return false;
    }
    switch (rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
    case DXGI_MODE_ROTATION_ROTATE180:
        logical_width = surface_width;
        logical_height = surface_height;
        return true;
    case DXGI_MODE_ROTATION_ROTATE90:
    case DXGI_MODE_ROTATION_ROTATE270:
        logical_width = surface_height;
        logical_height = surface_width;
        return true;
    default:
        logical_width = 0;
        logical_height = 0;
        return false;
    }
}

inline constexpr bool desktop_duplication_rect_in_bounds(
    const WgcRect& rect,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    return rect.x >= 0 && rect.y >= 0 && rect.width != 0
        && rect.height != 0
        && rect.width <= width - std::min(
            width, static_cast<std::uint32_t>(rect.x))
        && rect.height <= height - std::min(
            height, static_cast<std::uint32_t>(rect.y))
        && static_cast<std::uint32_t>(rect.x) < width
        && static_cast<std::uint32_t>(rect.y) < height;
}

// Desktop Duplication reports dirty/move rectangles in the unrotated surface
// coordinate system. This maps them into the display's logical orientation.
inline constexpr bool desktop_duplication_surface_to_logical_rect(
    const WgcRect& surface,
    std::uint32_t surface_width,
    std::uint32_t surface_height,
    DXGI_MODE_ROTATION rotation,
    WgcRect& logical) noexcept {
    logical = {};
    if (!desktop_duplication_rect_in_bounds(
            surface, surface_width, surface_height)) {
        return false;
    }
    const std::uint32_t x = static_cast<std::uint32_t>(surface.x);
    const std::uint32_t y = static_cast<std::uint32_t>(surface.y);
    switch (rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
        logical = surface;
        return true;
    case DXGI_MODE_ROTATION_ROTATE90:
        logical = {
            static_cast<std::int32_t>(surface_height - y - surface.height),
            static_cast<std::int32_t>(x),
            surface.height,
            surface.width};
        return true;
    case DXGI_MODE_ROTATION_ROTATE180:
        logical = {
            static_cast<std::int32_t>(surface_width - x - surface.width),
            static_cast<std::int32_t>(surface_height - y - surface.height),
            surface.width,
            surface.height};
        return true;
    case DXGI_MODE_ROTATION_ROTATE270:
        logical = {
            static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(surface_width - x - surface.width),
            surface.height,
            surface.width};
        return true;
    default:
        return false;
    }
}

// The inverse mapping is used for the video processor's pre-rotation source
// rectangle. D3D11 specifies source rectangles in pre-rotation coordinates.
inline constexpr bool desktop_duplication_logical_to_surface_rect(
    const WgcRect& logical,
    std::uint32_t surface_width,
    std::uint32_t surface_height,
    DXGI_MODE_ROTATION rotation,
    WgcRect& surface) noexcept {
    surface = {};
    std::uint32_t logical_width = 0;
    std::uint32_t logical_height = 0;
    if (!desktop_duplication_rotation_dimensions(
            rotation,
            surface_width,
            surface_height,
            logical_width,
            logical_height)
        || !desktop_duplication_rect_in_bounds(
            logical, logical_width, logical_height)) {
        return false;
    }
    const std::uint32_t x = static_cast<std::uint32_t>(logical.x);
    const std::uint32_t y = static_cast<std::uint32_t>(logical.y);
    switch (rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
        surface = logical;
        return true;
    case DXGI_MODE_ROTATION_ROTATE90:
        surface = {
            static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(surface_height - x - logical.width),
            logical.height,
            logical.width};
        return true;
    case DXGI_MODE_ROTATION_ROTATE180:
        surface = {
            static_cast<std::int32_t>(surface_width - x - logical.width),
            static_cast<std::int32_t>(surface_height - y - logical.height),
            logical.width,
            logical.height};
        return true;
    case DXGI_MODE_ROTATION_ROTATE270:
        surface = {
            static_cast<std::int32_t>(surface_width - y - logical.height),
            static_cast<std::int32_t>(x),
            logical.height,
            logical.width};
        return true;
    default:
        return false;
    }
}

inline constexpr std::uint32_t desktop_duplication_scale_floor(
    std::uint32_t value,
    std::uint32_t source_extent,
    std::uint32_t destination_extent) noexcept {
    return source_extent == 0
        ? 0
        : static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(value) * destination_extent)
                / source_extent);
}

inline constexpr std::uint32_t desktop_duplication_scale_ceil(
    std::uint32_t value,
    std::uint32_t source_extent,
    std::uint32_t destination_extent) noexcept {
    if (source_extent == 0) return 0;
    const std::uint64_t product =
        static_cast<std::uint64_t>(value) * destination_extent;
    return static_cast<std::uint32_t>(
        product / source_extent + (product % source_extent != 0));
}

// Conservatively maps every source pixel touched by a rectangle. For 4:2:0
// outputs it additionally expands to complete 2x2 chroma samples.
inline constexpr bool desktop_duplication_scale_rect(
    const WgcRect& source,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t destination_width,
    std::uint32_t destination_height,
    bool planar_420,
    WgcRect& destination) noexcept {
    destination = {};
    if (!desktop_duplication_rect_in_bounds(
            source, source_width, source_height)
        || destination_width == 0 || destination_height == 0) {
        return false;
    }
    if (destination_width
            > static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())
        || destination_height
            > static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    const std::uint32_t source_left = static_cast<std::uint32_t>(source.x);
    const std::uint32_t source_top = static_cast<std::uint32_t>(source.y);
    std::uint32_t left = desktop_duplication_scale_floor(
        source_left, source_width, destination_width);
    std::uint32_t top = desktop_duplication_scale_floor(
        source_top, source_height, destination_height);
    std::uint32_t right = desktop_duplication_scale_ceil(
        source_left + source.width, source_width, destination_width);
    std::uint32_t bottom = desktop_duplication_scale_ceil(
        source_top + source.height, source_height, destination_height);
    if (planar_420) {
        left &= ~1u;
        top &= ~1u;
        right = std::min(destination_width, (right + 1u) & ~1u);
        bottom = std::min(destination_height, (bottom + 1u) & ~1u);
    }
    if (left >= right || top >= bottom) return false;
    destination = {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        right - left,
        bottom - top};
    return true;
}

inline constexpr std::int32_t desktop_duplication_scale_position(
    std::int32_t value,
    std::uint32_t source_extent,
    std::uint32_t destination_extent) noexcept {
    if (source_extent == 0) return 0;
    const std::int64_t signed_value = value;
    const std::uint64_t magnitude = static_cast<std::uint64_t>(
        signed_value < 0 ? -signed_value : signed_value);
    const std::uint64_t rounded =
        (magnitude * destination_extent + source_extent / 2u)
            / source_extent;
    const std::int64_t result = signed_value < 0
        ? -static_cast<std::int64_t>(rounded)
        : static_cast<std::int64_t>(rounded);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        result,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

// Move rectangles can only be replayed after scaling when every edge lands on
// an exact destination pixel boundary. Otherwise callers must publish the
// conservatively mapped destination as dirty.
inline constexpr bool desktop_duplication_scale_move_exact(
    const WgcRect& source,
    const WgcRect& destination_source_space,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t destination_width,
    std::uint32_t destination_height,
    WgcMoveRect& move) noexcept {
    move = {};
    if (!desktop_duplication_rect_in_bounds(
            source, source_width, source_height)
        || !desktop_duplication_rect_in_bounds(
            destination_source_space, source_width, source_height)
        || source.width != destination_source_space.width
        || source.height != destination_source_space.height
        || destination_width == 0 || destination_height == 0
        || destination_width
            > static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())
        || destination_height
            > static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    const auto exact_axis = [](
        std::uint32_t begin,
        std::uint32_t length,
        std::uint32_t source_extent,
        std::uint32_t destination_extent,
        std::uint32_t& output_begin,
        std::uint32_t& output_end) noexcept {
        const std::uint64_t begin_product =
            static_cast<std::uint64_t>(begin) * destination_extent;
        const std::uint64_t end_product =
            static_cast<std::uint64_t>(begin + length)
                * destination_extent;
        if (begin_product % source_extent != 0
            || end_product % source_extent != 0) {
            return false;
        }
        output_begin = static_cast<std::uint32_t>(
            begin_product / source_extent);
        output_end = static_cast<std::uint32_t>(
            end_product / source_extent);
        return output_begin < output_end;
    };
    std::uint32_t source_left = 0;
    std::uint32_t source_right = 0;
    std::uint32_t source_top = 0;
    std::uint32_t source_bottom = 0;
    std::uint32_t destination_left = 0;
    std::uint32_t destination_right = 0;
    std::uint32_t destination_top = 0;
    std::uint32_t destination_bottom = 0;
    if (!exact_axis(
            static_cast<std::uint32_t>(source.x), source.width,
            source_width, destination_width,
            source_left, source_right)
        || !exact_axis(
            static_cast<std::uint32_t>(source.y), source.height,
            source_height, destination_height,
            source_top, source_bottom)
        || !exact_axis(
            static_cast<std::uint32_t>(destination_source_space.x),
            destination_source_space.width,
            source_width, destination_width,
            destination_left, destination_right)
        || !exact_axis(
            static_cast<std::uint32_t>(destination_source_space.y),
            destination_source_space.height,
            source_height, destination_height,
            destination_top, destination_bottom)
        || source_right - source_left != destination_right - destination_left
        || source_bottom - source_top
            != destination_bottom - destination_top) {
        return false;
    }
    move.source_x = static_cast<std::int32_t>(source_left);
    move.source_y = static_cast<std::int32_t>(source_top);
    move.destination = {
        static_cast<std::int32_t>(destination_left),
        static_cast<std::int32_t>(destination_top),
        destination_right - destination_left,
        destination_bottom - destination_top};
    return true;
}

inline constexpr bool desktop_duplication_geometry_contract() noexcept {
    std::uint32_t logical_width = 0;
    std::uint32_t logical_height = 0;
    if (!desktop_duplication_rotation_dimensions(
            DXGI_MODE_ROTATION_ROTATE90,
            1920,
            1080,
            logical_width,
            logical_height)
        || logical_width != 1080 || logical_height != 1920) {
        return false;
    }
    const WgcRect surface{100, 200, 300, 400};
    WgcRect logical;
    WgcRect round_trip;
    if (!desktop_duplication_surface_to_logical_rect(
            surface,
            1920,
            1080,
            DXGI_MODE_ROTATION_ROTATE90,
            logical)
        || logical.x != 480 || logical.y != 100
        || logical.width != 400 || logical.height != 300
        || !desktop_duplication_logical_to_surface_rect(
            logical,
            1920,
            1080,
            DXGI_MODE_ROTATION_ROTATE90,
            round_trip)
        || round_trip.x != surface.x || round_trip.y != surface.y
        || round_trip.width != surface.width
        || round_trip.height != surface.height) {
        return false;
    }
    if (!desktop_duplication_surface_to_logical_rect(
            surface,
            1920,
            1080,
            DXGI_MODE_ROTATION_ROTATE180,
            logical)
        || logical.x != 1520 || logical.y != 480
        || logical.width != 300 || logical.height != 400
        || !desktop_duplication_surface_to_logical_rect(
            surface,
            1920,
            1080,
            DXGI_MODE_ROTATION_ROTATE270,
            logical)
        || logical.x != 200 || logical.y != 1520
        || logical.width != 400 || logical.height != 300
        || desktop_duplication_rotation_dimensions(
            static_cast<DXGI_MODE_ROTATION>(99),
            1920,
            1080,
            logical_width,
            logical_height)
        || desktop_duplication_rotation_dimensions(
            DXGI_MODE_ROTATION_IDENTITY,
            static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max()) + 1u,
            1080,
            logical_width,
            logical_height)) {
        return false;
    }
    WgcRect planar;
    if (!desktop_duplication_scale_rect(
            {1, 1, 1, 1}, 4, 4, 6, 6, true, planar)
        || planar.x != 0 || planar.y != 0
        || planar.width != 4 || planar.height != 4
        || !desktop_duplication_scale_rect(
            {3, 3, 1, 1}, 4, 4, 6, 6, true, planar)
        || planar.x != 4 || planar.y != 4
        || planar.width != 2 || planar.height != 2) {
        return false;
    }
    WgcMoveRect move;
    std::int32_t hotspot_x = 0;
    std::int32_t hotspot_y = 0;
    if (!desktop_duplication_scale_move_exact(
            {0, 0, 4, 4},
            {4, 4, 4, 4},
            8,
            8,
            4,
            4,
            move)
        || move.source_x != 0 || move.source_y != 0
        || move.destination.x != 2 || move.destination.y != 2
        || move.destination.width != 2 || move.destination.height != 2
        || !desktop_duplication_planar_move_aligned(move)
        || desktop_duplication_scale_move_exact(
            {1, 0, 3, 4},
            {5, 4, 3, 4},
            8,
            8,
            4,
            4,
            move)
        || desktop_duplication_scale_position(-1, 4, 6) != -2
        || !desktop_duplication_pointer_hotspot(
            -10, 20, 3, 4, hotspot_x, hotspot_y)
        || hotspot_x != -7 || hotspot_y != 24
        || desktop_duplication_pointer_hotspot(
            std::numeric_limits<std::int32_t>::max(),
            0,
            1,
            0,
            hotspot_x,
            hotspot_y)) {
        return false;
    }
    return true;
}

static_assert(desktop_duplication_geometry_contract());

} // namespace fluxcap::gpu::internal
#endif
