#ifndef FLUXCAP_GPU_SIDE_DATA_GEOMETRY_HPP
#define FLUXCAP_GPU_SIDE_DATA_GEOMETRY_HPP

#include <fluxcap/gpu.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fluxcap::gpu::internal {

// Geometry shared by capture backends when side data names a source ROI but
// the committed bus texture has different dimensions. Dirty rectangles use
// conservative coverage. Move rectangles use a stricter, exact mapping
// because a consumer must be able to replay them byte-for-byte.
struct SideDataGeometry final {
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    bool planar_420 = false;
};

enum class SideDataMapStatus : std::uint8_t {
    empty,
    mapped,
    unrepresentable
};

inline bool valid_side_data_geometry(
    const SideDataGeometry& geometry) noexcept {
    return geometry.source_width != 0
        && geometry.source_height != 0
        && geometry.output_width != 0
        && geometry.output_height != 0
        && (!geometry.planar_420
            || ((geometry.output_width | geometry.output_height) & 1u) == 0);
}

inline bool scaled_side_data_geometry(
    const SideDataGeometry& geometry) noexcept {
    return geometry.source_width != geometry.output_width
        || geometry.source_height != geometry.output_height;
}

// Integer center-point tap shared with the deterministic planar shader. D3D11
// texture limits keep the shader's 32-bit product in range; the CPU version
// uses 64-bit arithmetic so geometry validation remains independent of that
// implementation limit.
inline std::uint32_t deterministic_point_tap(
    std::uint32_t output_coordinate,
    std::uint32_t source_extent,
    std::uint32_t output_extent) noexcept {
    if (source_extent == 0 || output_extent == 0
        || output_coordinate >= output_extent) {
        return 0;
    }
    const std::uint64_t numerator =
        (static_cast<std::uint64_t>(output_coordinate) * 2u + 1u)
        * source_extent;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        source_extent - 1u,
        numerator / (static_cast<std::uint64_t>(output_extent) * 2u)));
}

inline std::uint32_t deterministic_first_output_tap_at_least(
    std::uint32_t source_edge,
    std::uint32_t source_extent,
    std::uint32_t output_extent) noexcept {
    std::uint32_t first = 0;
    std::uint32_t last = output_extent;
    while (first < last) {
        const std::uint32_t middle = first + (last - first) / 2u;
        if (deterministic_point_tap(
                middle, source_extent, output_extent) < source_edge) {
            first = middle + 1u;
        } else {
            last = middle;
        }
    }
    return first;
}

// A cursor shape is cached independently from its frame position. With a
// fractional scale, the exact output footprint and hot-spot phase depend on
// that position, so one stable cached shape cannot describe every frame.
// Integer enlargement (including 1:1) is phase invariant; arbitrary dirty
// rectangles and individually validated moves do not have this restriction.
inline bool phase_invariant_cursor_geometry(
    const SideDataGeometry& geometry) noexcept {
    return valid_side_data_geometry(geometry)
        && geometry.output_width % geometry.source_width == 0
        && geometry.output_height % geometry.source_height == 0;
}

inline std::uint64_t scale_edge_floor(
    std::uint64_t edge,
    std::uint32_t source_extent,
    std::uint32_t output_extent) noexcept {
    return edge * output_extent / source_extent;
}

inline std::uint64_t scale_edge_ceil(
    std::uint64_t edge,
    std::uint32_t source_extent,
    std::uint32_t output_extent) noexcept {
    const std::uint64_t product = edge * output_extent;
    return product / source_extent
        + (product % source_extent != 0 ? 1u : 0u);
}

inline bool scale_edge_exact(
    std::uint64_t edge,
    std::uint32_t source_extent,
    std::uint32_t output_extent,
    std::uint64_t& output) noexcept {
    const std::uint64_t product = edge * output_extent;
    if (product % source_extent != 0) return false;
    output = product / source_extent;
    return true;
}

inline SideDataMapStatus clip_source_rect_to_region(
    const WgcRect& source,
    std::uint32_t region_x,
    std::uint32_t region_y,
    std::uint32_t region_width,
    std::uint32_t region_height,
    WgcRect& output) noexcept {
    output = {};
    if (source.width == 0 || source.height == 0
        || region_width == 0 || region_height == 0) {
        return SideDataMapStatus::empty;
    }

    const std::int64_t source_left = source.x;
    const std::int64_t source_top = source.y;
    const std::int64_t source_right = source_left + source.width;
    const std::int64_t source_bottom = source_top + source.height;
    const std::int64_t region_left = region_x;
    const std::int64_t region_top = region_y;
    const std::int64_t region_right = region_left + region_width;
    const std::int64_t region_bottom = region_top + region_height;
    const std::int64_t left = std::max(source_left, region_left);
    const std::int64_t top = std::max(source_top, region_top);
    const std::int64_t right = std::min(source_right, region_right);
    const std::int64_t bottom = std::min(source_bottom, region_bottom);
    if (left >= right || top >= bottom) {
        return SideDataMapStatus::empty;
    }

    const std::uint64_t local_left = static_cast<std::uint64_t>(
        left - region_left);
    const std::uint64_t local_top = static_cast<std::uint64_t>(
        top - region_top);
    const std::uint64_t local_width = static_cast<std::uint64_t>(right - left);
    const std::uint64_t local_height = static_cast<std::uint64_t>(bottom - top);
    if (local_left > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        || local_top > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        || local_width > std::numeric_limits<std::uint32_t>::max()
        || local_height > std::numeric_limits<std::uint32_t>::max()) {
        return SideDataMapStatus::unrepresentable;
    }

    output.x = static_cast<std::int32_t>(local_left);
    output.y = static_cast<std::int32_t>(local_top);
    output.width = static_cast<std::uint32_t>(local_width);
    output.height = static_cast<std::uint32_t>(local_height);
    return SideDataMapStatus::mapped;
}

inline SideDataMapStatus map_local_damage_rect(
    const WgcRect& source,
    const SideDataGeometry& geometry,
    WgcRect& output) noexcept {
    output = {};
    if (!valid_side_data_geometry(geometry)) {
        return SideDataMapStatus::unrepresentable;
    }
    if (source.width == 0 || source.height == 0) {
        return SideDataMapStatus::empty;
    }

    const std::int64_t source_left_signed = source.x;
    const std::int64_t source_top_signed = source.y;
    const std::int64_t source_right_signed =
        source_left_signed + source.width;
    const std::int64_t source_bottom_signed =
        source_top_signed + source.height;
    const std::int64_t clipped_left = std::max<std::int64_t>(
        0, source_left_signed);
    const std::int64_t clipped_top = std::max<std::int64_t>(
        0, source_top_signed);
    const std::int64_t clipped_right = std::min<std::int64_t>(
        geometry.source_width, source_right_signed);
    const std::int64_t clipped_bottom = std::min<std::int64_t>(
        geometry.source_height, source_bottom_signed);
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
        return SideDataMapStatus::empty;
    }

    std::uint64_t left = scale_edge_floor(
        static_cast<std::uint64_t>(clipped_left),
        geometry.source_width,
        geometry.output_width);
    std::uint64_t top = scale_edge_floor(
        static_cast<std::uint64_t>(clipped_top),
        geometry.source_height,
        geometry.output_height);
    std::uint64_t right = scale_edge_ceil(
        static_cast<std::uint64_t>(clipped_right),
        geometry.source_width,
        geometry.output_width);
    std::uint64_t bottom = scale_edge_ceil(
        static_cast<std::uint64_t>(clipped_bottom),
        geometry.source_height,
        geometry.output_height);

    if (geometry.planar_420) {
        left &= ~std::uint64_t{1};
        top &= ~std::uint64_t{1};
        right = std::min<std::uint64_t>(
            geometry.output_width, (right + 1u) & ~std::uint64_t{1});
        bottom = std::min<std::uint64_t>(
            geometry.output_height, (bottom + 1u) & ~std::uint64_t{1});
    }
    if (left >= right || top >= bottom) {
        return SideDataMapStatus::empty;
    }
    if (left > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        || top > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        || right - left > std::numeric_limits<std::uint32_t>::max()
        || bottom - top > std::numeric_limits<std::uint32_t>::max()) {
        return SideDataMapStatus::unrepresentable;
    }

    output.x = static_cast<std::int32_t>(left);
    output.y = static_cast<std::int32_t>(top);
    output.width = static_cast<std::uint32_t>(right - left);
    output.height = static_cast<std::uint32_t>(bottom - top);
    return SideDataMapStatus::mapped;
}

// Maps the exact support of the deterministic point/2x2-box planar kernel.
// A dirty source texel affects every luma output whose integer center tap
// selects it. Chroma consumes pairs of those luma taps, so the public frame
// rectangle is expanded only to the containing 2x2 output blocks.
inline SideDataMapStatus map_local_deterministic_planar_damage_rect(
    const WgcRect& source,
    const SideDataGeometry& geometry,
    WgcRect& output) noexcept {
    output = {};
    if (!valid_side_data_geometry(geometry) || !geometry.planar_420) {
        return SideDataMapStatus::unrepresentable;
    }
    if (source.width == 0 || source.height == 0) {
        return SideDataMapStatus::empty;
    }

    const std::int64_t source_right_signed =
        static_cast<std::int64_t>(source.x) + source.width;
    const std::int64_t source_bottom_signed =
        static_cast<std::int64_t>(source.y) + source.height;
    const std::int64_t clipped_left = std::max<std::int64_t>(0, source.x);
    const std::int64_t clipped_top = std::max<std::int64_t>(0, source.y);
    const std::int64_t clipped_right = std::min<std::int64_t>(
        geometry.source_width, source_right_signed);
    const std::int64_t clipped_bottom = std::min<std::int64_t>(
        geometry.source_height, source_bottom_signed);
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
        return SideDataMapStatus::empty;
    }

    std::uint32_t left = deterministic_first_output_tap_at_least(
        static_cast<std::uint32_t>(clipped_left),
        geometry.source_width,
        geometry.output_width);
    std::uint32_t right = deterministic_first_output_tap_at_least(
        static_cast<std::uint32_t>(clipped_right),
        geometry.source_width,
        geometry.output_width);
    std::uint32_t top = deterministic_first_output_tap_at_least(
        static_cast<std::uint32_t>(clipped_top),
        geometry.source_height,
        geometry.output_height);
    std::uint32_t bottom = deterministic_first_output_tap_at_least(
        static_cast<std::uint32_t>(clipped_bottom),
        geometry.source_height,
        geometry.output_height);
    if (left >= right || top >= bottom) {
        return SideDataMapStatus::empty;
    }

    left &= ~1u;
    top &= ~1u;
    right = std::min(geometry.output_width, (right + 1u) & ~1u);
    bottom = std::min(geometry.output_height, (bottom + 1u) & ~1u);
    if (left > static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max())
        || top > static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max())) {
        return SideDataMapStatus::unrepresentable;
    }
    output = {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        right - left,
        bottom - top};
    return SideDataMapStatus::mapped;
}

struct DeterministicPlanarMoveMapping final {
    WgcMoveRect move{};
    std::uint32_t dirty_count = 0;
    std::array<WgcRect, 4> dirty_rects{};
};

// Finds the largest 2-pixel-aligned output interval whose deterministic taps
// can be replayed from the previous output after a native source-space move.
// Exact output displacement is necessary but not sufficient: the interval is
// also trimmed inward so every byte of its 4:2:0 chroma samples has moved.
inline bool map_deterministic_planar_move_axis(
    std::uint32_t source_begin,
    std::uint32_t destination_begin,
    std::uint32_t length,
    std::uint32_t source_extent,
    std::uint32_t output_extent,
    std::uint32_t& output_source_begin,
    std::uint32_t& output_destination_begin,
    std::uint32_t& output_length) noexcept {
    output_source_begin = 0;
    output_destination_begin = 0;
    output_length = 0;
    if (source_extent == 0 || output_extent == 0
        || (output_extent & 1u) != 0
        || output_extent > static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max())
        || length == 0 || source_begin >= source_extent
        || destination_begin >= source_extent
        || length > source_extent - source_begin
        || length > source_extent - destination_begin) {
        return false;
    }

    const std::int64_t source_delta =
        static_cast<std::int64_t>(destination_begin) - source_begin;
    const std::int64_t scaled_product = source_delta * output_extent;
    if (scaled_product % source_extent != 0) return false;
    const std::int64_t output_delta = scaled_product / source_extent;
    if ((output_delta % 2) != 0) return false;

    const std::uint32_t first_tap =
        deterministic_first_output_tap_at_least(
            destination_begin, source_extent, output_extent);
    const std::uint32_t end_tap =
        deterministic_first_output_tap_at_least(
            destination_begin + length, source_extent, output_extent);
    const std::uint32_t destination_output_begin =
        first_tap + (first_tap & 1u);
    const std::uint32_t destination_output_end = end_tap & ~1u;
    if (destination_output_begin >= destination_output_end) return false;

    const std::int64_t source_output_begin_signed =
        static_cast<std::int64_t>(destination_output_begin) - output_delta;
    const std::int64_t source_output_end_signed =
        static_cast<std::int64_t>(destination_output_end) - output_delta;
    if (source_output_begin_signed < 0
        || source_output_end_signed <= source_output_begin_signed
        || source_output_end_signed > output_extent) {
        return false;
    }
    const auto source_output_begin_value = static_cast<std::uint32_t>(
        source_output_begin_signed);
    const auto source_output_end = static_cast<std::uint32_t>(
        source_output_end_signed);
    if (((source_output_begin_value | source_output_end) & 1u) != 0) {
        return false;
    }

    // Translation by an exact scaled displacement preserves the center-tap
    // phase. Validate both interval endpoints as a fail-closed guard around
    // that identity and around any future change to the shader tap formula.
    const std::int64_t first_source_tap = deterministic_point_tap(
        source_output_begin_value, source_extent, output_extent);
    const std::int64_t first_destination_tap = deterministic_point_tap(
        destination_output_begin, source_extent, output_extent);
    const std::int64_t last_source_tap = deterministic_point_tap(
        source_output_end - 1u, source_extent, output_extent);
    const std::int64_t last_destination_tap = deterministic_point_tap(
        destination_output_end - 1u, source_extent, output_extent);
    const std::int64_t source_end =
        static_cast<std::int64_t>(source_begin) + length;
    const std::int64_t destination_end =
        static_cast<std::int64_t>(destination_begin) + length;
    if (first_source_tap < source_begin
        || last_source_tap >= source_end
        || first_destination_tap < destination_begin
        || last_destination_tap >= destination_end
        || first_destination_tap - first_source_tap != source_delta
        || last_destination_tap - last_source_tap != source_delta) {
        return false;
    }

    output_source_begin = source_output_begin_value;
    output_destination_begin = destination_output_begin;
    output_length = destination_output_end - destination_output_begin;
    return true;
}

// Maps a deterministic scaled 4:2:0 native move to one replayable interior
// move. The exact affected support outside that interior is represented by at
// most four non-overlapping dirty strips (top, bottom, left, right).
inline SideDataMapStatus map_local_deterministic_planar_move(
    const WgcRect& source,
    const WgcRect& destination,
    const SideDataGeometry& geometry,
    DeterministicPlanarMoveMapping& output) noexcept {
    output = {};
    if (!valid_side_data_geometry(geometry) || !geometry.planar_420
        || geometry.output_width > static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max())
        || geometry.output_height > static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max())
        || source.x < 0 || source.y < 0
        || destination.x < 0 || destination.y < 0
        || source.width == 0 || source.height == 0
        || source.width != destination.width
        || source.height != destination.height) {
        return SideDataMapStatus::unrepresentable;
    }
    const auto rect_in_bounds = [](
        const WgcRect& rect,
        std::uint32_t width,
        std::uint32_t height) noexcept {
        const auto x = static_cast<std::uint32_t>(rect.x);
        const auto y = static_cast<std::uint32_t>(rect.y);
        return x < width && y < height
            && rect.width <= width - x && rect.height <= height - y;
    };
    if (!rect_in_bounds(source, geometry.source_width, geometry.source_height)
        || !rect_in_bounds(
            destination, geometry.source_width, geometry.source_height)) {
        return SideDataMapStatus::unrepresentable;
    }

    WgcRect affected;
    const SideDataMapStatus affected_status =
        map_local_deterministic_planar_damage_rect(
            destination, geometry, affected);
    if (affected_status != SideDataMapStatus::mapped) {
        return affected_status;
    }

    std::uint32_t source_x = 0;
    std::uint32_t destination_x = 0;
    std::uint32_t width = 0;
    std::uint32_t source_y = 0;
    std::uint32_t destination_y = 0;
    std::uint32_t height = 0;
    if (!map_deterministic_planar_move_axis(
            static_cast<std::uint32_t>(source.x),
            static_cast<std::uint32_t>(destination.x),
            destination.width,
            geometry.source_width,
            geometry.output_width,
            source_x,
            destination_x,
            width)
        || !map_deterministic_planar_move_axis(
            static_cast<std::uint32_t>(source.y),
            static_cast<std::uint32_t>(destination.y),
            destination.height,
            geometry.source_height,
            geometry.output_height,
            source_y,
            destination_y,
            height)) {
        return SideDataMapStatus::unrepresentable;
    }

    const std::uint32_t affected_left = static_cast<std::uint32_t>(affected.x);
    const std::uint32_t affected_top = static_cast<std::uint32_t>(affected.y);
    const std::uint32_t affected_right = affected_left + affected.width;
    const std::uint32_t affected_bottom = affected_top + affected.height;
    const std::uint32_t interior_right = destination_x + width;
    const std::uint32_t interior_bottom = destination_y + height;
    if (destination_x < affected_left || destination_y < affected_top
        || interior_right > affected_right
        || interior_bottom > affected_bottom) {
        return SideDataMapStatus::unrepresentable;
    }

    DeterministicPlanarMoveMapping mapped;
    mapped.move.source_x = static_cast<std::int32_t>(source_x);
    mapped.move.source_y = static_cast<std::int32_t>(source_y);
    mapped.move.destination = {
        static_cast<std::int32_t>(destination_x),
        static_cast<std::int32_t>(destination_y),
        width,
        height};
    const auto append_strip = [&](WgcRect strip) noexcept {
        if (strip.width != 0 && strip.height != 0) {
            mapped.dirty_rects[mapped.dirty_count++] = strip;
        }
    };
    append_strip({
        affected.x,
        affected.y,
        affected.width,
        destination_y - affected_top});
    append_strip({
        affected.x,
        static_cast<std::int32_t>(interior_bottom),
        affected.width,
        affected_bottom - interior_bottom});
    append_strip({
        affected.x,
        static_cast<std::int32_t>(destination_y),
        destination_x - affected_left,
        height});
    append_strip({
        static_cast<std::int32_t>(interior_right),
        static_cast<std::int32_t>(destination_y),
        affected_right - interior_right,
        height});
    output = mapped;
    return SideDataMapStatus::mapped;
}

// Appends the move and all of its edge strips atomically. A false result lets
// the caller replace the entire frame with overflow/full damage without ever
// exposing a partial replay contract.
inline bool append_deterministic_planar_move_mapping(
    const DeterministicPlanarMoveMapping& mapping,
    WgcFrameDamage& damage) noexcept {
    if (mapping.dirty_count > mapping.dirty_rects.size()
        || damage.move_count >= wgc_max_move_rects
        || damage.dirty_count > wgc_max_dirty_rects
        || mapping.dirty_count > wgc_max_dirty_rects - damage.dirty_count) {
        return false;
    }
    damage.move_rects[damage.move_count++] = mapping.move;
    for (std::uint32_t index = 0; index < mapping.dirty_count; ++index) {
        damage.dirty_rects[damage.dirty_count++] = mapping.dirty_rects[index];
    }
    return true;
}

inline SideDataMapStatus map_source_damage_rect(
    const WgcRect& source,
    std::uint32_t region_x,
    std::uint32_t region_y,
    const SideDataGeometry& geometry,
    WgcRect& output) noexcept {
    WgcRect local;
    const SideDataMapStatus clipped = clip_source_rect_to_region(
        source,
        region_x,
        region_y,
        geometry.source_width,
        geometry.source_height,
        local);
    if (clipped != SideDataMapStatus::mapped) {
        output = {};
        return clipped;
    }
    return map_local_damage_rect(local, geometry, output);
}

inline SideDataMapStatus map_source_deterministic_planar_damage_rect(
    const WgcRect& source,
    std::uint32_t region_x,
    std::uint32_t region_y,
    const SideDataGeometry& geometry,
    WgcRect& output) noexcept {
    WgcRect local;
    const SideDataMapStatus clipped = clip_source_rect_to_region(
        source,
        region_x,
        region_y,
        geometry.source_width,
        geometry.source_height,
        local);
    if (clipped != SideDataMapStatus::mapped) {
        output = {};
        return clipped;
    }
    return map_local_deterministic_planar_damage_rect(
        local, geometry, output);
}

inline bool map_move_rect_exact(
    const WgcMoveRect& source,
    const SideDataGeometry& geometry,
    WgcMoveRect& output) noexcept {
    output = {};
    if (!valid_side_data_geometry(geometry)
        || source.source_x < 0 || source.source_y < 0
        || source.destination.x < 0 || source.destination.y < 0
        || source.destination.width == 0
        || source.destination.height == 0) {
        return false;
    }

    const std::uint64_t source_left = static_cast<std::uint32_t>(
        source.source_x);
    const std::uint64_t source_top = static_cast<std::uint32_t>(
        source.source_y);
    const std::uint64_t destination_left = static_cast<std::uint32_t>(
        source.destination.x);
    const std::uint64_t destination_top = static_cast<std::uint32_t>(
        source.destination.y);
    const std::uint64_t source_right =
        source_left + source.destination.width;
    const std::uint64_t source_bottom =
        source_top + source.destination.height;
    const std::uint64_t destination_right =
        destination_left + source.destination.width;
    const std::uint64_t destination_bottom =
        destination_top + source.destination.height;
    if (source_right > geometry.source_width
        || destination_right > geometry.source_width
        || source_bottom > geometry.source_height
        || destination_bottom > geometry.source_height) {
        return false;
    }

    std::uint64_t mapped_source_left = 0;
    std::uint64_t mapped_source_top = 0;
    std::uint64_t mapped_source_right = 0;
    std::uint64_t mapped_source_bottom = 0;
    std::uint64_t mapped_destination_left = 0;
    std::uint64_t mapped_destination_top = 0;
    std::uint64_t mapped_destination_right = 0;
    std::uint64_t mapped_destination_bottom = 0;
    if (!scale_edge_exact(
            source_left, geometry.source_width, geometry.output_width,
            mapped_source_left)
        || !scale_edge_exact(
            source_right, geometry.source_width, geometry.output_width,
            mapped_source_right)
        || !scale_edge_exact(
            destination_left, geometry.source_width, geometry.output_width,
            mapped_destination_left)
        || !scale_edge_exact(
            destination_right, geometry.source_width, geometry.output_width,
            mapped_destination_right)
        || !scale_edge_exact(
            source_top, geometry.source_height, geometry.output_height,
            mapped_source_top)
        || !scale_edge_exact(
            source_bottom, geometry.source_height, geometry.output_height,
            mapped_source_bottom)
        || !scale_edge_exact(
            destination_top, geometry.source_height, geometry.output_height,
            mapped_destination_top)
        || !scale_edge_exact(
            destination_bottom, geometry.source_height, geometry.output_height,
            mapped_destination_bottom)) {
        return false;
    }

    const std::uint64_t source_width =
        mapped_source_right - mapped_source_left;
    const std::uint64_t source_height =
        mapped_source_bottom - mapped_source_top;
    const std::uint64_t destination_width =
        mapped_destination_right - mapped_destination_left;
    const std::uint64_t destination_height =
        mapped_destination_bottom - mapped_destination_top;
    if (source_width == 0 || source_height == 0
        || source_width != destination_width
        || source_height != destination_height
        || mapped_source_left > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        || mapped_source_top > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        || mapped_destination_left > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        || mapped_destination_top > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        || source_width > std::numeric_limits<std::uint32_t>::max()
        || source_height > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    if (geometry.planar_420
        && ((mapped_source_left | mapped_source_top
                | mapped_destination_left | mapped_destination_top
                | source_width | source_height) & 1u) != 0) {
        return false;
    }

    output.source_x = static_cast<std::int32_t>(mapped_source_left);
    output.source_y = static_cast<std::int32_t>(mapped_source_top);
    output.destination.x = static_cast<std::int32_t>(
        mapped_destination_left);
    output.destination.y = static_cast<std::int32_t>(
        mapped_destination_top);
    output.destination.width = static_cast<std::uint32_t>(source_width);
    output.destination.height = static_cast<std::uint32_t>(source_height);
    return true;
}

inline bool scale_signed_coordinate_nearest(
    std::int64_t coordinate,
    std::uint32_t source_extent,
    std::uint32_t output_extent,
    std::int32_t& output) noexcept {
    output = 0;
    if (source_extent == 0 || output_extent == 0) return false;

    const bool negative = coordinate < 0;
    const std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-(coordinate + 1)) + 1u
        : static_cast<std::uint64_t>(coordinate);
    const std::uint64_t quotient = magnitude / source_extent;
    const std::uint64_t remainder = magnitude % source_extent;
    const std::uint64_t limit = negative
        ? std::uint64_t{1} << 31u
        : static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max());
    if (quotient > limit / output_extent) return false;
    std::uint64_t scaled = quotient * output_extent;
    const std::uint64_t fractional = remainder * output_extent;
    const std::uint64_t rounded = fractional / source_extent
        + ((fractional % source_extent) * 2u >= source_extent ? 1u : 0u);
    if (rounded > limit || scaled > limit - rounded) return false;
    scaled += rounded;

    if (negative) {
        if (scaled == (std::uint64_t{1} << 31u)) {
            output = std::numeric_limits<std::int32_t>::min();
        } else {
            output = -static_cast<std::int32_t>(scaled);
        }
    } else {
        output = static_cast<std::int32_t>(scaled);
    }
    return true;
}

inline std::uint64_t cursor_shape_hash_bytes(
    std::uint64_t value,
    const void* data,
    std::size_t size) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= prime;
    }
    return value;
}

inline std::uint64_t cursor_shape_content_sequence(
    const WgcCursorShape& shape) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    hash = cursor_shape_hash_bytes(hash, &shape.kind, sizeof(shape.kind));
    hash = cursor_shape_hash_bytes(hash, &shape.width, sizeof(shape.width));
    hash = cursor_shape_hash_bytes(hash, &shape.height, sizeof(shape.height));
    hash = cursor_shape_hash_bytes(
        hash, &shape.hotspot_x, sizeof(shape.hotspot_x));
    hash = cursor_shape_hash_bytes(
        hash, &shape.hotspot_y, sizeof(shape.hotspot_y));
    hash = cursor_shape_hash_bytes(hash, shape.data.data(), shape.data.size());
    return hash == 0 ? 1 : hash;
}

inline bool valid_cursor_shape_storage(const WgcCursorShape& shape) noexcept {
    if (shape.kind == WgcCursorShapeKind::none
        || shape.width == 0 || shape.height == 0
        || shape.hotspot_x >= shape.width || shape.hotspot_y >= shape.height
        || shape.width > 256 || shape.height > 256
        || shape.stride_bytes == 0 || shape.data.empty()) {
        return false;
    }
    std::uint64_t expected = 0;
    if (shape.kind == WgcCursorShapeKind::color_bgra8
        || shape.kind == WgcCursorShapeKind::masked_color_bgra8) {
        if (shape.stride_bytes != shape.width * 4u) return false;
        expected = static_cast<std::uint64_t>(shape.stride_bytes)
            * shape.height;
    } else if (shape.kind == WgcCursorShapeKind::monochrome_and_xor) {
        if (shape.stride_bytes != ((shape.width + 31u) / 32u) * 4u) {
            return false;
        }
        expected = static_cast<std::uint64_t>(shape.stride_bytes)
            * shape.height * 2u;
    } else {
        return false;
    }
    return expected == shape.data.size()
        && expected <= shared_frame_bus_max_cursor_shape_bytes;
}

inline bool monochrome_cursor_bit(
    const WgcCursorShape& shape,
    std::uint32_t plane,
    std::uint32_t x,
    std::uint32_t y) noexcept {
    const std::size_t row = static_cast<std::size_t>(
        plane * shape.height + y) * shape.stride_bytes;
    return (shape.data[row + x / 8u] & (0x80u >> (x & 7u))) != 0;
}

inline void set_monochrome_cursor_bit(
    WgcCursorShape& shape,
    std::uint32_t plane,
    std::uint32_t x,
    std::uint32_t y) noexcept {
    const std::size_t row = static_cast<std::size_t>(
        plane * shape.height + y) * shape.stride_bytes;
    shape.data[row + x / 8u] |= static_cast<std::uint8_t>(
        0x80u >> (x & 7u));
}

inline bool scale_cursor_shape(
    const WgcCursorShape& source,
    const SideDataGeometry& geometry,
    WgcCursorShape& output) noexcept {
    output = {};
    if (!valid_side_data_geometry(geometry)
        || !valid_cursor_shape_storage(source)) {
        return false;
    }
    try {
        const std::uint64_t width = scale_edge_ceil(
            source.width, geometry.source_width, geometry.output_width);
        const std::uint64_t height = scale_edge_ceil(
            source.height, geometry.source_height, geometry.output_height);
        if (width == 0 || height == 0 || width > 256 || height > 256) {
            return false;
        }

        output.kind = source.kind;
        output.width = static_cast<std::uint32_t>(width);
        output.height = static_cast<std::uint32_t>(height);
        std::int32_t hotspot_x = 0;
        std::int32_t hotspot_y = 0;
        if (!scale_signed_coordinate_nearest(
                source.hotspot_x,
                geometry.source_width,
                geometry.output_width,
                hotspot_x)
            || !scale_signed_coordinate_nearest(
                source.hotspot_y,
                geometry.source_height,
                geometry.output_height,
                hotspot_y)
            || hotspot_x < 0 || hotspot_y < 0) {
            return false;
        }
        output.hotspot_x = std::min(
            output.width - 1u, static_cast<std::uint32_t>(hotspot_x));
        output.hotspot_y = std::min(
            output.height - 1u, static_cast<std::uint32_t>(hotspot_y));

        if (output.kind == WgcCursorShapeKind::color_bgra8
            || output.kind == WgcCursorShapeKind::masked_color_bgra8) {
            output.stride_bytes = output.width * 4u;
            const std::uint64_t byte_count =
                static_cast<std::uint64_t>(output.stride_bytes)
                * output.height;
            if (byte_count > shared_frame_bus_max_cursor_shape_bytes) {
                return false;
            }
            output.data.resize(static_cast<std::size_t>(byte_count));
            for (std::uint32_t y = 0; y < output.height; ++y) {
                const std::uint32_t source_y = std::min(
                    source.height - 1u,
                    static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(y)
                        * geometry.source_height
                        / geometry.output_height));
                for (std::uint32_t x = 0; x < output.width; ++x) {
                    const std::uint32_t source_x = std::min(
                        source.width - 1u,
                        static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(x)
                            * geometry.source_width
                            / geometry.output_width));
                    const std::size_t source_offset =
                        static_cast<std::size_t>(source_y)
                            * source.stride_bytes
                        + source_x * 4u;
                    const std::size_t output_offset =
                        static_cast<std::size_t>(y) * output.stride_bytes
                        + x * 4u;
                    std::copy_n(
                        source.data.data() + source_offset,
                        4u,
                        output.data.data() + output_offset);
                }
            }
        } else {
            output.stride_bytes = ((output.width + 31u) / 32u) * 4u;
            const std::uint64_t byte_count =
                static_cast<std::uint64_t>(output.stride_bytes)
                * output.height * 2u;
            if (byte_count > shared_frame_bus_max_cursor_shape_bytes) {
                return false;
            }
            output.data.assign(static_cast<std::size_t>(byte_count), 0);
            for (std::uint32_t plane = 0; plane < 2; ++plane) {
                for (std::uint32_t y = 0; y < output.height; ++y) {
                    const std::uint32_t source_y = std::min(
                        source.height - 1u,
                        static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(y)
                            * geometry.source_height
                            / geometry.output_height));
                    for (std::uint32_t x = 0; x < output.width; ++x) {
                        const std::uint32_t source_x = std::min(
                            source.width - 1u,
                            static_cast<std::uint32_t>(
                                static_cast<std::uint64_t>(x)
                                * geometry.source_width
                                / geometry.output_width));
                        if (monochrome_cursor_bit(
                                source, plane, source_x, source_y)) {
                            set_monochrome_cursor_bit(
                                output, plane, x, y);
                        }
                    }
                }
            }
        }
        output.sequence = cursor_shape_content_sequence(output);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace fluxcap::gpu::internal

#endif
