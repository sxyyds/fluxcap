#include "dirty_tracker.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <nmmintrin.h>
#endif

namespace fluxcap::internal {
namespace {

constexpr uint64_t kMixA = 0x9e3779b185ebca87ull;
constexpr uint64_t kMixB = 0xc2b2ae3d27d4eb4full;

uint64_t avalanche(uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

uint64_t hash_tile_portable(
    const uint8_t* pixels,
    uint32_t stride,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    uint64_t hash = kMixA ^ (static_cast<uint64_t>(width) << 32) ^ height;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* cursor = pixels + static_cast<size_t>(y + row) * stride
            + static_cast<size_t>(x) * 4u;
        size_t remaining = static_cast<size_t>(width) * 4u;

        while (remaining >= sizeof(uint64_t)) {
            uint64_t word = 0;
            std::memcpy(&word, cursor, sizeof(word));
            hash ^= word + kMixB + std::rotl(hash, 23);
            hash *= kMixA;
            cursor += sizeof(word);
            remaining -= sizeof(word);
        }
        if (remaining >= sizeof(uint32_t)) {
            uint32_t word = 0;
            std::memcpy(&word, cursor, sizeof(word));
            hash ^= static_cast<uint64_t>(word) + kMixB;
            hash = std::rotl(hash, 27) * kMixA;
        }
        hash ^= kMixB + row;
    }

    return avalanche(hash);
}

#if defined(_MSC_VER) && defined(_M_X64)
bool cpu_has_sse42() noexcept {
    int registers[4]{};
    __cpuid(registers, 1);
    return (registers[2] & (1 << 20)) != 0;
}

uint64_t hash_tile_crc32c(
    const uint8_t* pixels,
    uint32_t stride,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    uint64_t crc_a = 0x6d5a56daull;
    uint64_t crc_b = 0xa5a5f00dull;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* cursor = pixels + static_cast<size_t>(y + row) * stride
            + static_cast<size_t>(x) * 4u;
        size_t remaining = static_cast<size_t>(width) * 4u;

        while (remaining >= sizeof(uint64_t)) {
            uint64_t word = 0;
            std::memcpy(&word, cursor, sizeof(word));
            crc_a = _mm_crc32_u64(crc_a, word);
            crc_b = _mm_crc32_u64(crc_b, std::rotl(word ^ kMixA, 29));
            cursor += sizeof(word);
            remaining -= sizeof(word);
        }
        if (remaining >= sizeof(uint32_t)) {
            uint32_t word = 0;
            std::memcpy(&word, cursor, sizeof(word));
            crc_a = _mm_crc32_u32(static_cast<uint32_t>(crc_a), word);
            crc_b = _mm_crc32_u32(
                static_cast<uint32_t>(crc_b),
                word ^ static_cast<uint32_t>(kMixB));
        }
        crc_a = _mm_crc32_u32(static_cast<uint32_t>(crc_a), row ^ width);
        crc_b = _mm_crc32_u32(static_cast<uint32_t>(crc_b), row ^ height);
    }

    const uint64_t combined = (static_cast<uint64_t>(static_cast<uint32_t>(crc_a)) << 32)
        | static_cast<uint32_t>(crc_b);
    return avalanche(combined ^ (static_cast<uint64_t>(width) << 32) ^ height);
}
#endif

auto select_hash_function() noexcept -> uint64_t (*)(
    const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
#if defined(_MSC_VER) && defined(_M_X64)
    if (cpu_has_sse42()) {
        return &hash_tile_crc32c;
    }
#endif
    return &hash_tile_portable;
}

} // namespace

DirtyTracker::DirtyTracker(uint32_t width, uint32_t height, uint32_t tile_size) {
    configure(width, height, tile_size);
}

void DirtyTracker::configure(uint32_t width, uint32_t height, uint32_t tile_size) {
    if (width == 0 || height == 0 || tile_size == 0) {
        throw std::invalid_argument("dirty tracker dimensions and tile size must be non-zero");
    }

    const uint64_t columns = (static_cast<uint64_t>(width) + tile_size - 1u) / tile_size;
    const uint64_t rows = (static_cast<uint64_t>(height) + tile_size - 1u) / tile_size;
    const uint64_t tile_count = columns * rows;
    if (columns > std::numeric_limits<uint32_t>::max()
        || rows > std::numeric_limits<uint32_t>::max()
        || tile_count > std::numeric_limits<size_t>::max()) {
        throw std::overflow_error("dirty tracker grid is too large");
    }

    width_ = width;
    height_ = height;
    tile_size_ = tile_size;
    columns_ = static_cast<uint32_t>(columns);
    rows_ = static_cast<uint32_t>(rows);
    hash_function_ = select_hash_function();

    previous_hashes_.assign(static_cast<size_t>(tile_count), 0);
    current_hashes_.assign(static_cast<size_t>(tile_count), 0);
    changed_tiles_.assign(static_cast<size_t>(tile_count), 0);
    previous_runs_.clear();
    current_runs_.clear();
    previous_runs_.reserve((columns_ + 1u) / 2u);
    current_runs_.reserve((columns_ + 1u) / 2u);
    has_baseline_ = false;
}

void DirtyTracker::reset() noexcept {
    has_baseline_ = false;
}

size_t DirtyTracker::max_dirty_rects() const noexcept {
    return changed_tiles_.size();
}

void DirtyTracker::analyze(
    const uint8_t* pixels,
    uint32_t stride,
    std::vector<DirtyRect>& output) {
    if (pixels == nullptr || stride < width_ * 4u || hash_function_ == nullptr) {
        throw std::invalid_argument("invalid frame passed to dirty tracker");
    }

    output.clear();
    for (uint32_t tile_y = 0; tile_y < rows_; ++tile_y) {
        const uint32_t y = tile_y * tile_size_;
        const uint32_t tile_height = std::min(tile_size_, height_ - y);
        for (uint32_t tile_x = 0; tile_x < columns_; ++tile_x) {
            const uint32_t x = tile_x * tile_size_;
            const uint32_t tile_width = std::min(tile_size_, width_ - x);
            const size_t index = static_cast<size_t>(tile_y) * columns_ + tile_x;
            const uint64_t hash = hash_function_(
                pixels, stride, x, y, tile_width, tile_height);
            current_hashes_[index] = hash;
            changed_tiles_[index] = static_cast<uint8_t>(
                !has_baseline_ || hash != previous_hashes_[index]);
        }
    }

    if (!has_baseline_) {
        output.push_back({0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)});
        previous_hashes_.swap(current_hashes_);
        has_baseline_ = true;
        return;
    }

    previous_runs_.clear();
    for (uint32_t tile_y = 0; tile_y < rows_; ++tile_y) {
        current_runs_.clear();
        uint32_t column = 0;
        size_t previous_index = 0;

        while (column < columns_) {
            const size_t tile_index = static_cast<size_t>(tile_y) * columns_ + column;
            if (changed_tiles_[tile_index] == 0) {
                ++column;
                continue;
            }

            const uint32_t first = column;
            do {
                ++column;
            } while (column < columns_
                && changed_tiles_[static_cast<size_t>(tile_y) * columns_ + column] != 0);
            const uint32_t end = column;

            while (previous_index < previous_runs_.size()
                && previous_runs_[previous_index].first_column < first) {
                ++previous_index;
            }

            size_t rect_index = output.size();
            if (previous_index < previous_runs_.size()
                && previous_runs_[previous_index].first_column == first
                && previous_runs_[previous_index].end_column == end) {
                rect_index = previous_runs_[previous_index].rect_index;
                const uint32_t y = tile_y * tile_size_;
                const uint32_t added_height = std::min(tile_size_, height_ - y);
                output[rect_index].height += static_cast<int32_t>(added_height);
            } else {
                const uint32_t x = first * tile_size_;
                const uint32_t y = tile_y * tile_size_;
                const uint32_t right = std::min(width_, end * tile_size_);
                const uint32_t bottom = std::min(height_, y + tile_size_);
                output.push_back({
                    static_cast<int32_t>(x),
                    static_cast<int32_t>(y),
                    static_cast<int32_t>(right - x),
                    static_cast<int32_t>(bottom - y)});
            }
            current_runs_.push_back({first, end, rect_index});
        }

        previous_runs_.swap(current_runs_);
    }

    previous_hashes_.swap(current_hashes_);
}

} // namespace fluxcap::internal
