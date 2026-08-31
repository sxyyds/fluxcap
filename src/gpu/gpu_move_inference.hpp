#ifndef FLUXCAP_GPU_MOVE_INFERENCE_HPP
#define FLUXCAP_GPU_MOVE_INFERENCE_HPP

#include <fluxcap/gpu.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace fluxcap::gpu::internal {

inline constexpr std::uint32_t gpu_move_tile_size = 16;

enum class GpuMoveConfidence : std::uint8_t {
    none,
    dual_hash_unique,
    dual_hash_unique_grouped
};

inline constexpr std::uint32_t gpu_move_evidence_dual_hash = 1u << 0;
inline constexpr std::uint32_t gpu_move_evidence_unique_previous = 1u << 1;
inline constexpr std::uint32_t gpu_move_evidence_complete_tiles = 1u << 2;
inline constexpr std::uint32_t gpu_move_evidence_equal_displacement = 1u << 3;
inline constexpr std::uint32_t gpu_move_evidence_dirty_preserved = 1u << 4;

inline constexpr std::uint32_t gpu_move_result_valid = 1u << 0;
inline constexpr std::uint32_t gpu_move_result_inferred = 1u << 1;
inline constexpr std::uint32_t gpu_move_result_dirty_preserved = 1u << 2;
inline constexpr std::uint32_t gpu_move_result_fail_closed = 1u << 3;
inline constexpr std::uint32_t gpu_move_result_capacity_exceeded = 1u << 4;

inline constexpr std::uint32_t gpu_move_submit_baseline = 1u << 0;
inline constexpr std::uint32_t gpu_move_submit_scheduled = 1u << 1;
inline constexpr std::uint32_t gpu_move_submit_no_candidates = 1u << 2;
inline constexpr std::uint32_t gpu_move_submit_fail_closed = 1u << 3;
inline constexpr std::uint32_t gpu_move_submit_candidate_overflow = 1u << 4;
inline constexpr std::uint32_t gpu_move_submit_no_readback_slot = 1u << 5;

struct GpuMoveInferenceConfig final {
    // Logical region hashed by the inference grid. Move rectangles are
    // reported in this region's local coordinate space.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // Zero texture dimensions mean width/height. A larger input texture plus
    // a non-zero region origin lets WGC hash an ROI in place without first
    // copying that ROI into another texture.
    std::uint32_t texture_width = 0;
    std::uint32_t texture_height = 0;
    std::uint32_t region_x = 0;
    std::uint32_t region_y = 0;
    std::uint32_t max_candidate_tiles = 512;
    std::uint32_t readback_slots = 3;
    // Requiring adjacent evidence sharply reduces false move classifications.
    std::uint32_t minimum_group_tiles = 2;
};

struct GpuInferredMove final {
    WgcMoveRect rectangle{};
    std::uint32_t tile_count = 0;
    std::uint32_t evidence_flags = 0;
    GpuMoveConfidence confidence = GpuMoveConfidence::none;
    std::array<std::uint8_t, 3> reserved{};
};

struct GpuMoveInferenceResult final {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::uint64_t base_sequence = 0;
    std::uint32_t move_count = 0;
    std::uint32_t flags = 0;
    std::array<GpuInferredMove, wgc_max_move_rects> moves{};
};

struct GpuMoveInferenceSubmitInfo final {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::uint64_t base_sequence = 0;
    // Opaque identity for finalizing or canceling exactly this submission.
    std::uint64_t submission_token = 0;
    std::uint32_t candidate_count = 0;
    std::uint32_t flags = 0;
};

// Internal, deliberately asynchronous move inference. The caller must retain
// the frame associated with result.sequence until try_resolve() returns it;
// attaching a result to a newer frame would make the metadata incorrect.
class GpuMoveInference final {
public:
    GpuMoveInference() noexcept;
    ~GpuMoveInference();
    GpuMoveInference(const GpuMoveInference&) = delete;
    GpuMoveInference& operator=(const GpuMoveInference&) = delete;
    GpuMoveInference(GpuMoveInference&&) noexcept;
    GpuMoveInference& operator=(GpuMoveInference&&) noexcept;

    [[nodiscard]] static GpuError create(
        ID3D11Device*,
        const GpuMoveInferenceConfig&,
        GpuMoveInference&) noexcept;

    [[nodiscard]] GpuError submit(
        ID3D11Texture2D*,
        std::uint64_t sequence,
        std::span<const WgcRect> dirty_rects,
        GpuMoveInferenceSubmitInfo&) noexcept;

    // Updates only the sampled source texture and ROI. The logical tile grid
    // and format are unchanged, so finalized pending readbacks stay valid.
    // The next submitted frame becomes a fresh baseline.
    [[nodiscard]] GpuError reconfigure_source(
        std::uint32_t texture_width,
        std::uint32_t texture_height,
        std::uint32_t region_x,
        std::uint32_t region_y) noexcept;

    // A successful submit remains provisional until the associated frame is
    // published. Finalizing makes its readback visible to try_resolve().
    [[nodiscard]] bool commit_submission(
        const GpuMoveInferenceSubmitInfo&) noexcept;

    // Cancels only the matching provisional submit and restores the prior
    // hash baseline. Older finalized readbacks remain resolvable.
    [[nodiscard]] bool cancel_submission(
        const GpuMoveInferenceSubmitInfo&) noexcept;

    [[nodiscard]] GpuError try_resolve(
        GpuMoveInferenceResult&, bool& ready) noexcept;

    // Breaks the hash chain without invalidating finalized pending results.
    // The next submitted frame becomes a fresh baseline in the same epoch.
    void reset_baseline() noexcept;

    // Pending GPU work is drained normally but its old-epoch results are
    // discarded. The next submitted frame becomes a fresh baseline.
    void reset_history() noexcept;

    [[nodiscard]] GpuMoveInferenceConfig config() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] std::uint32_t pending_count() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxcap::gpu::internal

#endif
