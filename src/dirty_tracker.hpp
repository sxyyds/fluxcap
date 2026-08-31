#ifndef FLUXCAP_DIRTY_TRACKER_HPP
#define FLUXCAP_DIRTY_TRACKER_HPP

#include <fluxcap/fluxcap.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fluxcap::internal {

using DirtyRect = fluxcap_rect;

class DirtyTracker final {
public:
    DirtyTracker() = default;
    DirtyTracker(uint32_t width, uint32_t height, uint32_t tile_size);

    void configure(uint32_t width, uint32_t height, uint32_t tile_size);
    void reset() noexcept;

    /*
     * Produces frame-relative rectangles. All internal buffers are allocated by
     * configure(); callers should reserve max_dirty_rects() in their output.
     */
    void analyze(
        const uint8_t* pixels,
        uint32_t stride,
        std::vector<DirtyRect>& output);

    [[nodiscard]] size_t max_dirty_rects() const noexcept;
    [[nodiscard]] bool has_baseline() const noexcept { return has_baseline_; }

private:
    struct Run {
        uint32_t first_column;
        uint32_t end_column;
        size_t rect_index;
    };

    using HashFunction = uint64_t (*)(
        const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t tile_size_ = 0;
    uint32_t columns_ = 0;
    uint32_t rows_ = 0;
    bool has_baseline_ = false;
    HashFunction hash_function_ = nullptr;
    std::vector<uint64_t> previous_hashes_;
    std::vector<uint64_t> current_hashes_;
    std::vector<uint8_t> changed_tiles_;
    std::vector<Run> previous_runs_;
    std::vector<Run> current_runs_;
};

} // namespace fluxcap::internal

#endif
