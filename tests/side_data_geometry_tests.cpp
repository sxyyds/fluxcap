#include "side_data_geometry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

namespace gpu = fluxcap::gpu;
namespace internal = fluxcap::gpu::internal;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_conservative_roi_damage_mapping() {
    const internal::SideDataGeometry geometry{5, 5, 8, 6, true};
    gpu::WgcRect mapped;
    const auto status = internal::map_source_damage_rect(
        {11, 21, 2, 2}, 10, 20, geometry, mapped);
    require(status == internal::SideDataMapStatus::mapped,
        "ROI damage did not map");
    require(mapped.x == 0 && mapped.y == 0
            && mapped.width == 6 && mapped.height == 4,
        "ROI damage was not conservatively scaled and 4:2:0 aligned");

    const auto edge_status = internal::map_source_damage_rect(
        {14, 24, 1, 1}, 10, 20, geometry, mapped);
    require(edge_status == internal::SideDataMapStatus::mapped,
        "edge damage did not map");
    require(mapped.x == 6 && mapped.y == 4
            && mapped.width == 2 && mapped.height == 2,
        "edge damage escaped or under-covered the planar surface");

    const auto outside = internal::map_source_damage_rect(
        {-20, -20, 2, 2}, 10, 20, geometry, mapped);
    require(outside == internal::SideDataMapStatus::empty,
        "out-of-ROI damage was not discarded");

    const internal::SideDataGeometry fractional{3, 3, 2, 2, false};
    require(internal::map_local_damage_rect(
                {1, 1, 1, 1}, fractional, mapped)
            == internal::SideDataMapStatus::mapped,
        "fractional damage did not map");
    require(mapped.x == 0 && mapped.y == 0
            && mapped.width == 2 && mapped.height == 2,
        "fractional damage did not conservatively cover both output pixels");

    const internal::SideDataGeometry invalid{0, 3, 2, 2, false};
    require(internal::map_local_damage_rect(
                {0, 0, 1, 1}, invalid, mapped)
            == internal::SideDataMapStatus::unrepresentable,
        "zero-sized geometry was accepted");
}

void test_exact_move_mapping() {
    const internal::SideDataGeometry geometry{8, 8, 4, 4, true};
    gpu::WgcMoveRect source;
    source.source_x = 0;
    source.source_y = 0;
    source.destination = {4, 4, 4, 4};
    gpu::WgcMoveRect mapped;
    require(internal::map_move_rect_exact(source, geometry, mapped),
        "exact planar move did not map");
    require(mapped.source_x == 0 && mapped.source_y == 0
            && mapped.destination.x == 2 && mapped.destination.y == 2
            && mapped.destination.width == 2
            && mapped.destination.height == 2,
        "exact planar move mapped to the wrong rectangle");

    source.source_x = 2;
    source.source_y = 2;
    source.destination = {4, 4, 2, 2};
    require(!internal::map_move_rect_exact(source, geometry, mapped),
        "chroma-misaligned move was accepted");

    const internal::SideDataGeometry fractional{3, 3, 2, 2, false};
    source.source_x = 0;
    source.source_y = 0;
    source.destination = {1, 1, 1, 1};
    require(!internal::map_move_rect_exact(source, fractional, mapped),
        "fractional-boundary move was accepted as exactly replayable");
}

void test_deterministic_planar_damage_mapping() {
    for (std::uint32_t source_extent = 1; source_extent <= 64;
         ++source_extent) {
        for (std::uint32_t output_extent = 2; output_extent <= 64;
             output_extent += 2) {
            for (std::uint32_t edge = 0; edge <= source_extent; ++edge) {
                std::uint32_t expected = 0;
                while (expected < output_extent
                    && internal::deterministic_point_tap(
                        expected, source_extent, output_extent) < edge) {
                    ++expected;
                }
                require(
                    internal::deterministic_first_output_tap_at_least(
                        edge, source_extent, output_extent) == expected,
                    "deterministic tap lower-bound disagrees with shader taps");
            }
        }
    }

    for (std::uint32_t source_width = 1; source_width <= 8;
         ++source_width) {
        for (std::uint32_t source_height = 1; source_height <= 8;
             ++source_height) {
            for (std::uint32_t output_width = 2; output_width <= 10;
                 output_width += 2) {
                for (std::uint32_t output_height = 2;
                     output_height <= 10;
                     output_height += 2) {
                    const internal::SideDataGeometry geometry{
                        source_width,
                        source_height,
                        output_width,
                        output_height,
                        true};
                    for (std::uint32_t source_y = 0;
                         source_y < source_height;
                         ++source_y) {
                        for (std::uint32_t source_x = 0;
                             source_x < source_width;
                             ++source_x) {
                            std::uint32_t left = output_width;
                            std::uint32_t top = output_height;
                            std::uint32_t right = 0;
                            std::uint32_t bottom = 0;
                            bool affected = false;
                            for (std::uint32_t y = 0; y < output_height; ++y) {
                                for (std::uint32_t x = 0; x < output_width;
                                     ++x) {
                                    if (internal::deterministic_point_tap(
                                            x, source_width, output_width)
                                            != source_x
                                        || internal::deterministic_point_tap(
                                            y, source_height, output_height)
                                            != source_y) {
                                        continue;
                                    }
                                    affected = true;
                                    left = std::min(left, x & ~1u);
                                    top = std::min(top, y & ~1u);
                                    right = std::max(
                                        right, std::min(output_width,
                                            (x + 2u) & ~1u));
                                    bottom = std::max(
                                        bottom, std::min(output_height,
                                            (y + 2u) & ~1u));
                                }
                            }

                            gpu::WgcRect mapped;
                            const auto status =
                                internal::map_local_deterministic_planar_damage_rect(
                                    {static_cast<std::int32_t>(source_x),
                                     static_cast<std::int32_t>(source_y),
                                     1,
                                     1},
                                    geometry,
                                    mapped);
                            if (!affected) {
                                require(
                                    status
                                        == internal::SideDataMapStatus::empty,
                                    "unsampled source texel produced damage");
                            } else {
                                require(
                                    status
                                        == internal::SideDataMapStatus::mapped,
                                    "sampled source texel lost damage");
                                require(
                                    mapped.x == static_cast<std::int32_t>(left)
                                        && mapped.y
                                            == static_cast<std::int32_t>(top)
                                        && mapped.width == right - left
                                        && mapped.height == bottom - top,
                                    "deterministic planar damage was not the exact 2x2-aligned support");
                            }
                        }
                    }
                }
            }
        }
    }

    const internal::SideDataGeometry roi_geometry{5, 3, 8, 6, true};
    gpu::WgcRect roi_mapped;
    require(
        internal::map_source_deterministic_planar_damage_rect(
            {12, 21, 1, 1}, 10, 20, roi_geometry, roi_mapped)
            == internal::SideDataMapStatus::mapped,
        "deterministic ROI damage did not map");
    require(
        internal::map_source_deterministic_planar_damage_rect(
            {1, 1, 1, 1}, 10, 20, roi_geometry, roi_mapped)
            == internal::SideDataMapStatus::empty,
        "deterministic out-of-ROI damage was not discarded");
}

struct BruteMoveAxis final {
    bool replayable = false;
    std::uint32_t source_begin = 0;
    std::uint32_t destination_begin = 0;
    std::uint32_t length = 0;
};

BruteMoveAxis brute_deterministic_planar_move_axis(
    std::uint32_t source_begin,
    std::uint32_t destination_begin,
    std::uint32_t length,
    std::uint32_t source_extent,
    std::uint32_t output_extent) {
    BruteMoveAxis result;
    if (source_extent == 0 || output_extent == 0
        || (output_extent & 1u) != 0 || length == 0
        || source_begin >= source_extent
        || destination_begin >= source_extent
        || length > source_extent - source_begin
        || length > source_extent - destination_begin) {
        return result;
    }
    const std::int64_t source_delta =
        static_cast<std::int64_t>(destination_begin) - source_begin;
    const std::int64_t product = source_delta * output_extent;
    if (product % source_extent != 0) return result;
    const std::int64_t output_delta = product / source_extent;
    if ((output_delta % 2) != 0) return result;

    bool ended = false;
    for (std::uint32_t block = 0; block < output_extent; block += 2) {
        const std::int64_t source_block =
            static_cast<std::int64_t>(block) - output_delta;
        bool good = source_block >= 0
            && source_block + 1 < output_extent;
        for (std::uint32_t offset = 0; good && offset != 2; ++offset) {
            const auto destination_tap = internal::deterministic_point_tap(
                block + offset, source_extent, output_extent);
            const auto source_tap = internal::deterministic_point_tap(
                static_cast<std::uint32_t>(source_block) + offset,
                source_extent,
                output_extent);
            good = destination_tap >= destination_begin
                && destination_tap - destination_begin < length
                && source_tap >= source_begin
                && source_tap - source_begin < length
                && static_cast<std::int64_t>(destination_tap) - source_tap
                    == source_delta;
        }
        if (good) {
            require(!ended,
                "brute deterministic move produced disjoint interiors");
            if (!result.replayable) {
                result.replayable = true;
                result.destination_begin = block;
                result.source_begin = static_cast<std::uint32_t>(source_block);
            }
            result.length += 2;
        } else if (result.replayable) {
            ended = true;
        }
    }
    return result;
}

void test_deterministic_planar_move_axis_exhaustive() {
    std::uint64_t replayable_count = 0;
    std::uint64_t rejected_count = 0;
    for (std::uint32_t source_extent = 1; source_extent <= 32;
         ++source_extent) {
        for (std::uint32_t output_extent = 2; output_extent <= 32;
             output_extent += 2) {
            for (std::uint32_t length = 1; length <= source_extent;
                 ++length) {
                for (std::uint32_t source_begin = 0;
                     source_begin + length <= source_extent;
                     ++source_begin) {
                    for (std::uint32_t destination_begin = 0;
                         destination_begin + length <= source_extent;
                         ++destination_begin) {
                        const BruteMoveAxis expected =
                            brute_deterministic_planar_move_axis(
                                source_begin,
                                destination_begin,
                                length,
                                source_extent,
                                output_extent);
                        std::uint32_t mapped_source = 99;
                        std::uint32_t mapped_destination = 99;
                        std::uint32_t mapped_length = 99;
                        const bool mapped =
                            internal::map_deterministic_planar_move_axis(
                                source_begin,
                                destination_begin,
                                length,
                                source_extent,
                                output_extent,
                                mapped_source,
                                mapped_destination,
                                mapped_length);
                        require(mapped == expected.replayable,
                            "deterministic move axis disagrees with brute taps");
                        if (!mapped) {
                            ++rejected_count;
                            require(mapped_source == 0
                                    && mapped_destination == 0
                                    && mapped_length == 0,
                                "rejected deterministic axis retained output");
                            continue;
                        }
                        ++replayable_count;
                        require(mapped_source == expected.source_begin
                                && mapped_destination
                                    == expected.destination_begin
                                && mapped_length == expected.length,
                            "deterministic move axis did not select the maximal aligned interior");
                    }
                }
            }
        }
    }
    require(replayable_count != 0 && rejected_count != 0,
        "deterministic move axis exhaustive matrix was degenerate");

    std::uint32_t source = 0;
    std::uint32_t destination = 0;
    std::uint32_t length = 0;
    require(internal::map_deterministic_planar_move_axis(
                1, 3, 2, 6, 18, source, destination, length)
            && source == 4 && destination == 10 && length == 4,
        "odd-phase deterministic move did not preserve its replayable interior");
}

bool rect_contains(
    const gpu::WgcRect& rect,
    std::uint32_t x,
    std::uint32_t y) {
    if (rect.x < 0 || rect.y < 0) return false;
    const auto left = static_cast<std::uint32_t>(rect.x);
    const auto top = static_cast<std::uint32_t>(rect.y);
    return x >= left && y >= top
        && x - left < rect.width && y - top < rect.height;
}

bool rects_overlap(const gpu::WgcRect& left, const gpu::WgcRect& right) {
    if (left.x < 0 || left.y < 0 || right.x < 0 || right.y < 0) {
        return false;
    }
    const std::uint64_t left_right =
        static_cast<std::uint32_t>(left.x) + left.width;
    const std::uint64_t left_bottom =
        static_cast<std::uint32_t>(left.y) + left.height;
    const std::uint64_t right_right =
        static_cast<std::uint32_t>(right.x) + right.width;
    const std::uint64_t right_bottom =
        static_cast<std::uint32_t>(right.y) + right.height;
    return static_cast<std::uint32_t>(left.x) < right_right
        && static_cast<std::uint32_t>(right.x) < left_right
        && static_cast<std::uint32_t>(left.y) < right_bottom
        && static_cast<std::uint32_t>(right.y) < left_bottom;
}

bool one_dirty_contains_block(
    const internal::DeterministicPlanarMoveMapping& mapping,
    std::uint32_t x,
    std::uint32_t y) {
    for (std::uint32_t index = 0; index < mapping.dirty_count; ++index) {
        const auto& dirty = mapping.dirty_rects[index];
        if (rect_contains(dirty, x, y)
            && rect_contains(dirty, x + 1u, y + 1u)) {
            return true;
        }
    }
    return false;
}

void validate_deterministic_planar_move_mapping(
    const gpu::WgcRect& source,
    const gpu::WgcRect& destination,
    const internal::SideDataGeometry& geometry,
    const internal::DeterministicPlanarMoveMapping& mapping) {
    const auto& interior = mapping.move.destination;
    require(mapping.dirty_count <= mapping.dirty_rects.size()
            && internal::valid_side_data_geometry(geometry)
            && geometry.planar_420,
        "deterministic planar move mapping header is invalid");
    require(mapping.move.source_x >= 0 && mapping.move.source_y >= 0
            && interior.x >= 0 && interior.y >= 0
            && interior.width != 0 && interior.height != 0
            && ((static_cast<std::uint32_t>(mapping.move.source_x)
                    | static_cast<std::uint32_t>(mapping.move.source_y)
                    | static_cast<std::uint32_t>(interior.x)
                    | static_cast<std::uint32_t>(interior.y)
                    | interior.width | interior.height) & 1u) == 0,
        "deterministic planar move interior is not 4:2:0 aligned");

    gpu::WgcRect affected;
    require(internal::map_local_deterministic_planar_damage_rect(
                destination, geometry, affected)
            == internal::SideDataMapStatus::mapped,
        "mapped deterministic move has no affected support");
    std::uint64_t dirty_area = 0;
    for (std::uint32_t index = 0; index < mapping.dirty_count; ++index) {
        const auto& dirty = mapping.dirty_rects[index];
        require(dirty.x >= 0 && dirty.y >= 0
                && dirty.width != 0 && dirty.height != 0
                && ((static_cast<std::uint32_t>(dirty.x)
                        | static_cast<std::uint32_t>(dirty.y)
                        | dirty.width | dirty.height) & 1u) == 0
                && !rects_overlap(dirty, interior),
            "deterministic move edge strip is invalid or overlaps interior");
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            require(!rects_overlap(dirty, mapping.dirty_rects[prior]),
                "deterministic move edge strips overlap");
        }
        dirty_area += static_cast<std::uint64_t>(dirty.width) * dirty.height;
    }
    const std::uint64_t affected_area =
        static_cast<std::uint64_t>(affected.width) * affected.height;
    const std::uint64_t interior_area =
        static_cast<std::uint64_t>(interior.width) * interior.height;
    require(dirty_area + interior_area == affected_area,
        "move interior and edge strips do not exactly partition damage support");

    const std::int64_t source_delta_x =
        static_cast<std::int64_t>(destination.x) - source.x;
    const std::int64_t source_delta_y =
        static_cast<std::int64_t>(destination.y) - source.y;
    for (std::uint32_t y = 0; y < geometry.output_height; ++y) {
        const auto destination_tap_y = internal::deterministic_point_tap(
            y, geometry.source_height, geometry.output_height);
        for (std::uint32_t x = 0; x < geometry.output_width; ++x) {
            const auto destination_tap_x = internal::deterministic_point_tap(
                x, geometry.source_width, geometry.output_width);
            const bool affected_luma = destination_tap_x
                    >= static_cast<std::uint32_t>(destination.x)
                && destination_tap_x
                    - static_cast<std::uint32_t>(destination.x)
                    < destination.width
                && destination_tap_y
                    >= static_cast<std::uint32_t>(destination.y)
                && destination_tap_y
                    - static_cast<std::uint32_t>(destination.y)
                    < destination.height;
            if (!affected_luma) continue;
            if (!rect_contains(interior, x, y)) {
                bool covered = false;
                for (std::uint32_t index = 0;
                     index < mapping.dirty_count;
                     ++index) {
                    covered = covered
                        || rect_contains(mapping.dirty_rects[index], x, y);
                }
                require(covered,
                    "changed luma dependency escaped move edge damage");
                continue;
            }

            const auto source_output_x = static_cast<std::uint32_t>(
                mapping.move.source_x + x - interior.x);
            const auto source_output_y = static_cast<std::uint32_t>(
                mapping.move.source_y + y - interior.y);
            const auto source_tap_x = internal::deterministic_point_tap(
                source_output_x,
                geometry.source_width,
                geometry.output_width);
            const auto source_tap_y = internal::deterministic_point_tap(
                source_output_y,
                geometry.source_height,
                geometry.output_height);
            require(source_tap_x >= static_cast<std::uint32_t>(source.x)
                    && source_tap_x
                        - static_cast<std::uint32_t>(source.x) < source.width
                    && source_tap_y >= static_cast<std::uint32_t>(source.y)
                    && source_tap_y
                        - static_cast<std::uint32_t>(source.y) < source.height
                    && static_cast<std::int64_t>(destination_tap_x)
                        - source_tap_x == source_delta_x
                    && static_cast<std::int64_t>(destination_tap_y)
                        - source_tap_y == source_delta_y,
                "move interior luma tap is not an exact translation");
        }
    }

    for (std::uint32_t y = 0; y < geometry.output_height; y += 2) {
        for (std::uint32_t x = 0; x < geometry.output_width; x += 2) {
            bool affected_chroma = false;
            for (std::uint32_t dy = 0; dy != 2; ++dy) {
                const auto tap_y = internal::deterministic_point_tap(
                    y + dy, geometry.source_height, geometry.output_height);
                for (std::uint32_t dx = 0; dx != 2; ++dx) {
                    const auto tap_x = internal::deterministic_point_tap(
                        x + dx, geometry.source_width, geometry.output_width);
                    affected_chroma = affected_chroma
                        || (tap_x >= static_cast<std::uint32_t>(destination.x)
                            && tap_x
                                - static_cast<std::uint32_t>(destination.x)
                                < destination.width
                            && tap_y
                                >= static_cast<std::uint32_t>(destination.y)
                            && tap_y
                                - static_cast<std::uint32_t>(destination.y)
                                < destination.height);
                }
            }
            if (!affected_chroma) continue;
            if (!rect_contains(interior, x, y)) {
                require(one_dirty_contains_block(mapping, x, y),
                    "changed UV dependency escaped aligned edge damage");
                continue;
            }
            require(rect_contains(interior, x + 1u, y + 1u),
                "move interior split a UV sample");
            const auto source_base_x = static_cast<std::uint32_t>(
                mapping.move.source_x + x - interior.x);
            const auto source_base_y = static_cast<std::uint32_t>(
                mapping.move.source_y + y - interior.y);
            for (std::uint32_t dy = 0; dy != 2; ++dy) {
                for (std::uint32_t dx = 0; dx != 2; ++dx) {
                    const auto destination_tap_x =
                        internal::deterministic_point_tap(
                            x + dx,
                            geometry.source_width,
                            geometry.output_width);
                    const auto destination_tap_y =
                        internal::deterministic_point_tap(
                            y + dy,
                            geometry.source_height,
                            geometry.output_height);
                    const auto source_tap_x = internal::deterministic_point_tap(
                        source_base_x + dx,
                        geometry.source_width,
                        geometry.output_width);
                    const auto source_tap_y = internal::deterministic_point_tap(
                        source_base_y + dy,
                        geometry.source_height,
                        geometry.output_height);
                    require(static_cast<std::int64_t>(destination_tap_x)
                                - source_tap_x == source_delta_x
                            && static_cast<std::int64_t>(destination_tap_y)
                                - source_tap_y == source_delta_y,
                        "move interior UV taps changed chroma phase");
                }
            }
        }
    }
}

void test_deterministic_planar_move_mapping() {
    const internal::SideDataGeometry fixed_geometry{6, 6, 18, 18, true};
    const gpu::WgcRect fixed_source{1, 1, 2, 2};
    const gpu::WgcRect fixed_destination{3, 3, 2, 2};
    internal::DeterministicPlanarMoveMapping fixed;
    require(internal::map_local_deterministic_planar_move(
                fixed_source, fixed_destination, fixed_geometry, fixed)
            == internal::SideDataMapStatus::mapped,
        "odd-phase two-dimensional deterministic move did not map");
    require(fixed.move.source_x == 4 && fixed.move.source_y == 4
            && fixed.move.destination.x == 10
            && fixed.move.destination.y == 10
            && fixed.move.destination.width == 4
            && fixed.move.destination.height == 4
            && fixed.dirty_count == 4,
        "deterministic move did not select its expected interior and four strips");
    const std::array<gpu::WgcRect, 4> expected_strips{{
        {8, 8, 8, 2},
        {8, 14, 8, 2},
        {8, 10, 2, 4},
        {14, 10, 2, 4}}};
    for (std::uint32_t index = 0; index != expected_strips.size(); ++index) {
        const auto& actual = fixed.dirty_rects[index];
        const auto& expected = expected_strips[index];
        require(actual.x == expected.x && actual.y == expected.y
                && actual.width == expected.width
                && actual.height == expected.height,
            "deterministic move edge strips are not stable top/bottom/left/right");
    }
    validate_deterministic_planar_move_mapping(
        fixed_source, fixed_destination, fixed_geometry, fixed);

    const std::array<internal::SideDataGeometry, 5> geometries{{
        {6, 6, 18, 18, true},
        {6, 5, 8, 6, true},
        {8, 8, 4, 4, true},
        {4, 4, 8, 8, true},
        {5, 3, 8, 6, true}}};
    std::uint64_t mapped_count = 0;
    for (const auto& geometry : geometries) {
        for (std::uint32_t height = 1;
             height <= geometry.source_height;
             ++height) {
            for (std::uint32_t source_y = 0;
                 source_y + height <= geometry.source_height;
                 ++source_y) {
                for (std::uint32_t destination_y = 0;
                     destination_y + height <= geometry.source_height;
                     ++destination_y) {
                    for (std::uint32_t width = 1;
                         width <= geometry.source_width;
                         ++width) {
                        for (std::uint32_t source_x = 0;
                             source_x + width <= geometry.source_width;
                             ++source_x) {
                            for (std::uint32_t destination_x = 0;
                                 destination_x + width
                                     <= geometry.source_width;
                                 ++destination_x) {
                                const gpu::WgcRect source{
                                    static_cast<std::int32_t>(source_x),
                                    static_cast<std::int32_t>(source_y),
                                    width,
                                    height};
                                const gpu::WgcRect destination{
                                    static_cast<std::int32_t>(destination_x),
                                    static_cast<std::int32_t>(destination_y),
                                    width,
                                    height};
                                internal::DeterministicPlanarMoveMapping mapped;
                                const auto status =
                                    internal::map_local_deterministic_planar_move(
                                        source,
                                        destination,
                                        geometry,
                                        mapped);
                                if (status
                                    != internal::SideDataMapStatus::mapped) {
                                    continue;
                                }
                                ++mapped_count;
                                validate_deterministic_planar_move_mapping(
                                    source, destination, geometry, mapped);
                            }
                        }
                    }
                }
            }
        }
    }
    require(mapped_count != 0,
        "two-dimensional deterministic move matrix mapped no moves");

    internal::DeterministicPlanarMoveMapping rejected;
    require(internal::map_local_deterministic_planar_move(
                {0, 0, 2, 4}, {1, 0, 2, 4}, {5, 4, 8, 4, true}, rejected)
            == internal::SideDataMapStatus::unrepresentable,
        "fractional output displacement was accepted");
    require(internal::map_local_deterministic_planar_move(
                {0, 0, 2, 4}, {2, 0, 2, 4}, {8, 4, 4, 4, true}, rejected)
            == internal::SideDataMapStatus::unrepresentable,
        "odd planar output displacement was accepted");
    require(internal::map_local_deterministic_planar_move(
                {2, 0, 1, 4}, {2, 0, 1, 4}, {8, 4, 2, 4, true}, rejected)
            == internal::SideDataMapStatus::unrepresentable,
        "empty chroma-aligned move interior was accepted");
    require(internal::map_local_deterministic_planar_move(
                {0, 0, 1, 4}, {0, 0, 1, 4}, {8, 4, 2, 4, true}, rejected)
            == internal::SideDataMapStatus::empty,
        "unsampled native move did not map to empty output support");
    require(internal::map_local_deterministic_planar_move(
                {-1, 0, 1, 1}, {0, 0, 1, 1}, fixed_geometry, rejected)
            == internal::SideDataMapStatus::unrepresentable,
        "out-of-bounds deterministic move was accepted");

    gpu::WgcFrameDamage exact_capacity;
    exact_capacity.dirty_count = gpu::wgc_max_dirty_rects - fixed.dirty_count;
    exact_capacity.move_count = gpu::wgc_max_move_rects - 1;
    require(internal::append_deterministic_planar_move_mapping(
                fixed, exact_capacity)
            && exact_capacity.dirty_count == gpu::wgc_max_dirty_rects
            && exact_capacity.move_count == gpu::wgc_max_move_rects,
        "exact deterministic move capacity was rejected");

    gpu::WgcFrameDamage short_dirty_capacity;
    short_dirty_capacity.dirty_count =
        gpu::wgc_max_dirty_rects - fixed.dirty_count + 1;
    short_dirty_capacity.move_count = 3;
    short_dirty_capacity.dirty_rects[0] = {7, 8, 9, 10};
    short_dirty_capacity.move_rects[0] = {11, 12, {13, 14, 16, 18}};
    require(!internal::append_deterministic_planar_move_mapping(
                fixed, short_dirty_capacity)
            && short_dirty_capacity.dirty_count
                == gpu::wgc_max_dirty_rects - fixed.dirty_count + 1
            && short_dirty_capacity.move_count == 3
            && short_dirty_capacity.dirty_rects[0].x == 7
            && short_dirty_capacity.move_rects[0].source_x == 11,
        "dirty-capacity failure partially appended deterministic move data");

    gpu::WgcFrameDamage short_move_capacity;
    short_move_capacity.dirty_count = 2;
    short_move_capacity.move_count = gpu::wgc_max_move_rects;
    short_move_capacity.dirty_rects[0] = {1, 2, 4, 6};
    require(!internal::append_deterministic_planar_move_mapping(
                fixed, short_move_capacity)
            && short_move_capacity.dirty_count == 2
            && short_move_capacity.move_count == gpu::wgc_max_move_rects
            && short_move_capacity.dirty_rects[0].x == 1,
        "move-capacity failure partially appended edge damage");
}

void test_signed_cursor_position_mapping() {
    std::int32_t output = 0;
    require(internal::scale_signed_coordinate_nearest(-3, 4, 2, output)
            && output == -2,
        "negative cursor coordinate did not round symmetrically");
    require(internal::scale_signed_coordinate_nearest(3, 4, 2, output)
            && output == 2,
        "positive cursor coordinate did not round symmetrically");
    require(!internal::scale_signed_coordinate_nearest(
                std::numeric_limits<std::int64_t>::max(),
                1,
                std::numeric_limits<std::uint32_t>::max(),
                output),
        "overflowing cursor coordinate was accepted");
}

gpu::WgcCursorShape make_color_shape(
    gpu::WgcCursorShapeKind kind =
        gpu::WgcCursorShapeKind::color_bgra8) {
    gpu::WgcCursorShape shape;
    shape.kind = kind;
    shape.width = 2;
    shape.height = 2;
    shape.hotspot_x = 1;
    shape.hotspot_y = 1;
    shape.stride_bytes = 8;
    shape.data = {
        1, 2, 3, 4,     5, 6, 7, 8,
        9, 10, 11, 12,  13, 14, 15, 16};
    shape.sequence = internal::cursor_shape_content_sequence(shape);
    return shape;
}

void test_color_cursor_shape_scaling() {
    const internal::SideDataGeometry geometry{2, 2, 4, 4, true};
    require(internal::phase_invariant_cursor_geometry(geometry),
        "integer cursor scaling was not capability-gated as stable");
    const auto source = make_color_shape();
    gpu::WgcCursorShape mapped;
    require(internal::scale_cursor_shape(source, geometry, mapped),
        "color cursor shape did not scale");
    require(mapped.width == 4 && mapped.height == 4
            && mapped.hotspot_x == 2 && mapped.hotspot_y == 2
            && mapped.stride_bytes == 16 && mapped.sequence != 0,
        "scaled color cursor header is incorrect");
    require(mapped.data[0] == 1 && mapped.data[12] == 5
            && mapped.data[48] == 9 && mapped.data[60] == 13,
        "scaled color cursor pixels used the wrong source samples");

    gpu::WgcCursorShape repeated;
    require(internal::scale_cursor_shape(source, geometry, repeated)
            && repeated.sequence == mapped.sequence
            && repeated.data == mapped.data,
        "scaled cursor content key is not stable");

    const auto masked = make_color_shape(
        gpu::WgcCursorShapeKind::masked_color_bgra8);
    require(internal::scale_cursor_shape(masked, geometry, repeated)
            && repeated.kind
                == gpu::WgcCursorShapeKind::masked_color_bgra8,
        "masked-color cursor semantics were lost during scaling");

    const internal::SideDataGeometry fractional{4, 4, 6, 6, false};
    require(!internal::phase_invariant_cursor_geometry(fractional),
        "fractional cursor scaling was incorrectly marked phase invariant");
    require(internal::scale_cursor_shape(source, fractional, repeated)
            && repeated.width == 3 && repeated.height == 3
            && repeated.hotspot_x == 2 && repeated.hotspot_y == 2,
        "fractional cursor hotspot did not use symmetric point scaling");
}

void test_monochrome_cursor_shape_scaling() {
    gpu::WgcCursorShape source;
    source.kind = gpu::WgcCursorShapeKind::monochrome_and_xor;
    source.width = 4;
    source.height = 2;
    source.hotspot_x = 1;
    source.hotspot_y = 0;
    source.stride_bytes = 4;
    source.data.assign(16, 0);
    internal::set_monochrome_cursor_bit(source, 0, 0, 0);
    internal::set_monochrome_cursor_bit(source, 0, 2, 0);
    internal::set_monochrome_cursor_bit(source, 1, 2, 0);
    source.sequence = internal::cursor_shape_content_sequence(source);

    const internal::SideDataGeometry geometry{4, 2, 2, 1, false};
    gpu::WgcCursorShape mapped;
    require(internal::scale_cursor_shape(source, geometry, mapped),
        "monochrome cursor shape did not scale");
    require(mapped.width == 2 && mapped.height == 1
            && mapped.hotspot_x == 1 && mapped.hotspot_y == 0
            && mapped.stride_bytes == 4,
        "scaled monochrome cursor header is incorrect");
    require(internal::monochrome_cursor_bit(mapped, 0, 0, 0)
            && internal::monochrome_cursor_bit(mapped, 0, 1, 0)
            && !internal::monochrome_cursor_bit(mapped, 1, 0, 0)
            && internal::monochrome_cursor_bit(mapped, 1, 1, 0),
        "monochrome AND/XOR planes were not independently resampled");
}

void test_cursor_shape_capacity_fail_closed() {
    gpu::WgcCursorShape source;
    source.kind = gpu::WgcCursorShapeKind::color_bgra8;
    source.width = 256;
    source.height = 1;
    source.hotspot_x = 0;
    source.hotspot_y = 0;
    source.stride_bytes = 1024;
    source.data.assign(1024, 0xff);
    source.sequence = internal::cursor_shape_content_sequence(source);
    gpu::WgcCursorShape mapped;
    require(!internal::scale_cursor_shape(
                source, {1, 1, 2, 2, false}, mapped)
            && mapped.sequence == 0 && mapped.data.empty(),
        "oversized scaled cursor shape did not fail closed");
}

} // namespace

int main() {
    try {
        test_conservative_roi_damage_mapping();
        test_exact_move_mapping();
        test_deterministic_planar_damage_mapping();
        test_deterministic_planar_move_axis_exhaustive();
        test_deterministic_planar_move_mapping();
        test_signed_cursor_position_mapping();
        test_color_cursor_shape_scaling();
        test_monochrome_cursor_shape_scaling();
        test_cursor_shape_capacity_fail_closed();
        std::cout << "side-data geometry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "side-data geometry tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
