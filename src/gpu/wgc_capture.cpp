#include "wgc_capture.hpp"

#include "gpu_move_inference.hpp"
#include "side_data_geometry.hpp"
#include "shared_frame_bus.hpp"
#include "wgc_dxgi_bridge.hpp"

#include <d3d10.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace direct3d = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace metadata = winrt::Windows::Foundation::Metadata;

constexpr std::uint32_t kWaitInfinite = 0xffffffffu;
constexpr std::size_t kCursorShapeCacheSize = 16;

directx::DirectXPixelFormat capture_pixel_format(
    WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? directx::DirectXPixelFormat::R16G16B16A16Float
        : directx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
}

DXGI_FORMAT capture_dxgi_format(WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_B8G8R8A8_UNORM;
}

DXGI_COLOR_SPACE_TYPE capture_color_space(
    WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

bool planar_bus_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010;
}

GpuPixelFormat planar_gpu_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_NV12
        ? GpuPixelFormat::nv12
        : GpuPixelFormat::p010;
}

DXGI_COLOR_SPACE_TYPE resolved_bus_color_space(
    const SharedFrameBusConfig& config,
    WgcPixelFormat input_format) noexcept {
    if (config.color_space != DXGI_COLOR_SPACE_CUSTOM) {
        return config.color_space;
    }
    if (config.format == DXGI_FORMAT_P010
        && input_format == WgcPixelFormat::rgba16_float) {
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    }
    if (config.format == DXGI_FORMAT_NV12
        || config.format == DXGI_FORMAT_P010) {
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }
    return capture_color_space(input_format);
}

class MoveInferenceSubmissionGuard final {
public:
    using WakeFunction = void (*)(void*) noexcept;

    MoveInferenceSubmissionGuard() noexcept = default;
    ~MoveInferenceSubmissionGuard() {
        (void)cancel();
    }

    MoveInferenceSubmissionGuard(const MoveInferenceSubmissionGuard&) = delete;
    MoveInferenceSubmissionGuard& operator=(
        const MoveInferenceSubmissionGuard&) = delete;

    void arm(
        internal::GpuMoveInference& engine,
        const internal::GpuMoveInferenceSubmitInfo& submission,
        void* wake_context,
        WakeFunction wake) noexcept {
        engine_ = &engine;
        submission_ = submission;
        wake_context_ = wake_context;
        wake_ = wake;
        state_ = State::active;
    }

    [[nodiscard]] bool cancel() noexcept {
        if (state_ != State::active || engine_ == nullptr) return false;
        if (!engine_->cancel_submission(submission_)) return false;
        engine_ = nullptr;
        state_ = State::canceled;
        if (wake_ != nullptr) wake_(wake_context_);
        return true;
    }

    [[nodiscard]] bool finalize() noexcept {
        if (state_ != State::active || engine_ == nullptr) return false;
        if (!engine_->commit_submission(submission_)) return false;
        engine_ = nullptr;
        state_ = State::finalized;
        return true;
    }

private:
    enum class State : std::uint8_t {
        empty,
        active,
        canceled,
        finalized
    };

    internal::GpuMoveInference* engine_ = nullptr;
    internal::GpuMoveInferenceSubmitInfo submission_{};
    void* wake_context_ = nullptr;
    WakeFunction wake_ = nullptr;
    State state_ = State::empty;
};

enum class SlotState : std::uint8_t {
    free,
    writing,
    ready,
    reading
};

constexpr std::uint64_t kStateBits = 2;
constexpr std::uint64_t kStateMask = (1ull << kStateBits) - 1ull;

constexpr std::uint64_t make_control(
    std::uint64_t generation,
    SlotState state) noexcept {
    return (generation << kStateBits) | static_cast<std::uint64_t>(state);
}

constexpr SlotState control_state(std::uint64_t control) noexcept {
    return static_cast<SlotState>(control & kStateMask);
}

constexpr std::uint64_t control_generation(std::uint64_t control) noexcept {
    return control >> kStateBits;
}

WgcResult make_result(
    WgcStatus status,
    HRESULT hresult = S_OK,
    std::string message = {}) {
    if (message.empty()) {
        message = wgc_status_string(status);
    }
    return {status, hresult, std::move(message)};
}

WgcResult exception_result() noexcept {
    try {
        throw;
    } catch (const winrt::hresult_error& error) {
        try {
            return make_result(
                WgcStatus::capture_error,
                error.code(),
                winrt::to_string(error.message()));
        } catch (...) {
            return {WgcStatus::capture_error, error.code(), {}};
        }
    } catch (const std::bad_alloc&) {
        return {WgcStatus::out_of_memory, E_OUTOFMEMORY, {}};
    } catch (const std::exception& error) {
        try {
            return make_result(WgcStatus::capture_error, E_FAIL, error.what());
        } catch (...) {
            return {WgcStatus::capture_error, E_FAIL, {}};
        }
    } catch (...) {
        return {WgcStatus::capture_error, E_FAIL, {}};
    }
}

bool property_present(std::wstring_view property) noexcept {
    try {
        return metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            property);
    } catch (...) {
        return false;
    }
}

bool valid_mailbox_config(const WgcMailboxConfig& config) noexcept {
    switch (config.mode) {
    case WgcMailboxMode::full_frame:
        return config.x == 0 && config.y == 0
            && config.width == 0 && config.height == 0;
    case WgcMailboxMode::absolute_region:
        return config.width != 0 && config.height != 0;
    case WgcMailboxMode::centered_region:
        return config.x == 0 && config.y == 0
            && config.width != 0 && config.height != 0;
    default:
        return false;
    }
}

bool valid_capture_options(const WgcCaptureOptions& options) noexcept {
    return options.buffer_count >= 2
        && options.buffer_count <= 16
        && (options.pixel_format == WgcPixelFormat::bgra8
            || options.pixel_format == WgcPixelFormat::rgba16_float)
        && (options.damage_mode == WgcDamageMode::disabled
            || options.damage_mode == WgcDamageMode::native_report_only
            || options.damage_mode
                == WgcDamageMode::native_with_inferred_moves)
        && options.cursor_shape_refresh_interval_ms <= 10'000
        && (options.planar_transform_backend
                == GpuTransformBackend::automatic
            || options.planar_transform_backend
                == GpuTransformBackend::video_processor
            || options.planar_transform_backend
                == GpuTransformBackend::deterministic_planar)
        && options.capture_epoch != 0
        && options.capture_epoch_nonce != 0;
}

bool same_mailbox_config(
    const WgcMailboxConfig& left,
    const WgcMailboxConfig& right) noexcept {
    return left.mode == right.mode
        && left.x == right.x
        && left.y == right.y
        && left.width == right.width
        && left.height == right.height;
}

bool resolve_mailbox_region(
    const WgcMailboxConfig& config,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint64_t generation,
    WgcMailboxFrameInfo& output) noexcept {
    if (source_width == 0 || source_height == 0) return false;

    output = {};
    output.source_width = source_width;
    output.source_height = source_height;
    output.generation = generation;
    if (config.mode == WgcMailboxMode::full_frame) {
        output.width = source_width;
        output.height = source_height;
        return true;
    }
    if (config.width > source_width || config.height > source_height) {
        return false;
    }

    output.width = config.width;
    output.height = config.height;
    if (config.mode == WgcMailboxMode::centered_region) {
        output.x = (source_width - config.width) / 2u;
        output.y = (source_height - config.height) / 2u;
        return true;
    }
    if (config.mode != WgcMailboxMode::absolute_region
        || config.x > source_width - config.width
        || config.y > source_height - config.height) {
        return false;
    }
    output.x = config.x;
    output.y = config.y;
    return true;
}

bool mailbox_output_dimensions(
    const WgcMailboxConfig& config,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t& output_width,
    std::uint32_t& output_height) noexcept {
    if (config.mode == WgcMailboxMode::full_frame) {
        output_width = source_width;
        output_height = source_height;
    } else {
        output_width = config.width;
        output_height = config.height;
    }
    return output_width != 0 && output_height != 0;
}

std::uint64_t sample_publication_qpc() noexcept {
    LARGE_INTEGER timestamp{};
    return QueryPerformanceCounter(&timestamp) && timestamp.QuadPart > 0
        ? static_cast<std::uint64_t>(timestamp.QuadPart)
        : 0;
}

bool extract_cursor_shape(HCURSOR source, WgcCursorShape& output) noexcept {
    output = {};
    if (source == nullptr) return false;
    HICON cursor = CopyIcon(source);
    if (cursor == nullptr) return false;
    ICONINFO icon{};
    if (!GetIconInfo(cursor, &icon)) {
        DestroyIcon(cursor);
        return false;
    }

    bool success = false;
    HDC screen = GetDC(nullptr);
    try {
    do {
        if (screen == nullptr || icon.hbmMask == nullptr) break;
        BITMAP mask{};
        if (GetObjectW(icon.hbmMask, sizeof(mask), &mask) != sizeof(mask)
            || mask.bmWidth <= 0 || mask.bmHeight <= 0) {
            break;
        }
        output.hotspot_x = icon.xHotspot;
        output.hotspot_y = icon.yHotspot;

        if (icon.hbmColor != nullptr) {
            BITMAP color{};
            if (GetObjectW(icon.hbmColor, sizeof(color), &color) != sizeof(color)
                || color.bmWidth <= 0 || color.bmHeight <= 0
                || color.bmWidth > 256 || color.bmHeight > 256
                || icon.xHotspot >= static_cast<DWORD>(color.bmWidth)
                || icon.yHotspot >= static_cast<DWORD>(color.bmHeight)) {
                break;
            }
            output.kind = WgcCursorShapeKind::color_bgra8;
            output.width = static_cast<std::uint32_t>(color.bmWidth);
            output.height = static_cast<std::uint32_t>(color.bmHeight);
            output.stride_bytes = output.width * 4u;
            output.data.resize(
                static_cast<std::size_t>(output.stride_bytes) * output.height);
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = color.bmWidth;
            bitmap.bmiHeader.biHeight = -color.bmHeight;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            if (GetDIBits(
                    screen,
                    icon.hbmColor,
                    0,
                    output.height,
                    output.data.data(),
                    &bitmap,
                    DIB_RGB_COLORS) != static_cast<int>(output.height)) {
                break;
            }
        } else {
            if ((mask.bmHeight & 1) != 0
                || mask.bmWidth > 256 || mask.bmHeight > 512) {
                break;
            }
            output.kind = WgcCursorShapeKind::monochrome_and_xor;
            output.width = static_cast<std::uint32_t>(mask.bmWidth);
            output.height = static_cast<std::uint32_t>(mask.bmHeight / 2);
            if (icon.xHotspot >= output.width
                || icon.yHotspot >= output.height) {
                break;
            }
            output.stride_bytes = ((output.width + 31u) / 32u) * 4u;
            const std::uint32_t total_rows = output.height * 2u;
            output.data.resize(
                static_cast<std::size_t>(output.stride_bytes) * total_rows);
            struct MonoBitmapInfo final {
                BITMAPINFOHEADER header{};
                RGBQUAD colors[2]{};
            } bitmap;
            bitmap.header.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.header.biWidth = mask.bmWidth;
            bitmap.header.biHeight = -mask.bmHeight;
            bitmap.header.biPlanes = 1;
            bitmap.header.biBitCount = 1;
            bitmap.header.biCompression = BI_RGB;
            bitmap.colors[1] = {255, 255, 255, 0};
            if (GetDIBits(
                    screen,
                    icon.hbmMask,
                    0,
                    total_rows,
                    output.data.data(),
                    reinterpret_cast<BITMAPINFO*>(&bitmap),
                    DIB_RGB_COLORS) != static_cast<int>(total_rows)) {
                break;
            }
        }

        if (output.data.empty()
            || output.data.size()
                > shared_frame_bus_max_cursor_shape_bytes) {
            break;
        }

        output.sequence = internal::cursor_shape_content_sequence(output);
        success = true;
    } while (false);
    } catch (...) {
        success = false;
    }

    if (screen != nullptr) ReleaseDC(nullptr, screen);
    if (icon.hbmColor != nullptr) DeleteObject(icon.hbmColor);
    if (icon.hbmMask != nullptr) DeleteObject(icon.hbmMask);
    DestroyIcon(cursor);
    if (!success) output = {};
    return success;
}

WgcResult shared_bus_error(
    const GpuError& error,
    const char* fallback_message) {
    WgcStatus status = WgcStatus::capture_error;
    if (error.status == GpuStatus::out_of_memory) {
        status = WgcStatus::out_of_memory;
    } else if (error.status == GpuStatus::device_lost) {
        status = WgcStatus::d3d_error;
    }
    const char* message = error.what();
    return make_result(
        status,
        FAILED(error.hresult) ? error.hresult : E_FAIL,
        message != nullptr && message[0] != '\0' ? message : fallback_message);
}

struct ActiveAcquireGuard final {
    explicit ActiveAcquireGuard(std::atomic<std::uint32_t>& value) noexcept
        : value_(value) {
        value_.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ActiveAcquireGuard() {
        value_.fetch_sub(1, std::memory_order_acq_rel);
    }
    std::atomic<std::uint32_t>& value_;
};

} // namespace

struct WgcCaptureState final : std::enable_shared_from_this<WgcCaptureState> {
    struct Slot final {
        ComPtr<ID3D11Texture2D> texture;
        std::atomic<std::uint64_t> control{make_control(0, SlotState::free)};
        std::atomic<std::uint64_t> mailbox_generation{0};
        WgcFrameInfo info{};
        WgcMailboxFrameInfo mailbox{};
        WgcFrameDamage damage{};
        WgcCursorInfo cursor{};
    };

    ~WgcCaptureState() { stop(); }

    bool bus_output_matches(
        std::uint32_t width,
        std::uint32_t height) const noexcept {
        if (!bus_only) return true;
        if (!bus_planar_output) {
            return width == bus_config.width && height == bus_config.height;
        }
        internal::SideDataGeometry geometry;
        geometry.source_width = width;
        geometry.source_height = height;
        geometry.output_width = bus_config.width;
        geometry.output_height = bus_config.height;
        geometry.planar_420 = true;
        return !options.include_cursor_metadata
            || internal::phase_invariant_cursor_geometry(geometry);
    }

    internal::SideDataGeometry side_data_geometry(
        const WgcMailboxFrameInfo& mailbox) const noexcept {
        internal::SideDataGeometry output;
        output.source_width = mailbox.width;
        output.source_height = mailbox.height;
        output.output_width = bus_only && bus_planar_output
            ? bus_config.width
            : mailbox.width;
        output.output_height = bus_only && bus_planar_output
            ? bus_config.height
            : mailbox.height;
        output.planar_420 = bus_only && bus_planar_output;
        return output;
    }

    bool mailbox_dimensions_match_bus(
        const WgcMailboxConfig& config,
        std::uint32_t source_width,
        std::uint32_t source_height) const noexcept {
        if (!bus_only) return true;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        return mailbox_output_dimensions(
                config, source_width, source_height, width, height)
            && bus_output_matches(width, height);
    }

    bool resolve_output_region(
        const WgcMailboxConfig& config,
        std::uint32_t source_width,
        std::uint32_t source_height,
        std::uint64_t generation,
        WgcMailboxFrameInfo& output) const noexcept {
        return resolve_mailbox_region(
                config, source_width, source_height, generation, output)
            && bus_output_matches(output.width, output.height);
    }

    GpuError ensure_planar_bus_transform(
        const D3D11_TEXTURE2D_DESC& source,
        const WgcMailboxFrameInfo& mailbox) {
        GpuTransformConfig config;
        config.input_width = source.Width;
        config.input_height = source.Height;
        config.input_region_x = mailbox.x;
        config.input_region_y = mailbox.y;
        config.input_region_width = mailbox.width;
        config.input_region_height = mailbox.height;
        config.output_width = bus_config.width;
        config.output_height = bus_config.height;
        config.output_format = planar_gpu_format(bus_config.format);
        config.input_format = source.Format;
        config.input_color_space = capture_color_space(options.pixel_format);
        config.output_color_space = resolved_bus_color_space(
            bus_config, options.pixel_format);
        config.backend = options.planar_transform_backend;
        config.external_output_only = true;

        if (bus_transform.initialized()) {
            const GpuTransformConfig current = bus_transform.config();
            if (current.input_width == config.input_width
                && current.input_height == config.input_height
                && current.input_region_x == config.input_region_x
                && current.input_region_y == config.input_region_y
                && current.input_region_width == config.input_region_width
                && current.input_region_height == config.input_region_height
                && current.input_format == config.input_format
                && current.output_width == config.output_width
                && current.output_height == config.output_height
                && current.output_format == config.output_format
                && current.input_color_space == config.input_color_space
                && current.output_color_space == config.output_color_space
                && current.backend == config.backend
                && current.external_output_only
                    == config.external_output_only) {
                return {};
            }
        }

        GpuTransform replacement;
        const GpuError created = GpuTransform::create(
            device.Get(), config, replacement);
        if (!created) return created;
        bus_transform = std::move(replacement);
        bus_output_color_space = config.output_color_space;
        return {};
    }

    bool move_inference_requested() const noexcept {
        return options.damage_mode
            == WgcDamageMode::native_with_inferred_moves;
    }

    GpuError ensure_move_inference(
        const D3D11_TEXTURE2D_DESC& source,
        const WgcMailboxFrameInfo& mailbox) {
        internal::GpuMoveInferenceConfig config;
        config.width = mailbox.width;
        config.height = mailbox.height;
        config.format = source.Format;
        config.texture_width = source.Width;
        config.texture_height = source.Height;
        config.region_x = mailbox.x;
        config.region_y = mailbox.y;
        config.max_candidate_tiles = 512;
        config.readback_slots = 3;
        config.minimum_group_tiles = 2;
        if (move_inference.initialized()) {
            const auto current = move_inference.config();
            if (current.width == config.width
                && current.height == config.height
                && current.format == config.format
                && current.max_candidate_tiles == config.max_candidate_tiles
                && current.readback_slots == config.readback_slots
                && current.minimum_group_tiles == config.minimum_group_tiles) {
                return move_inference.reconfigure_source(
                    config.texture_width,
                    config.texture_height,
                    config.region_x,
                    config.region_y);
            }
            if (move_inference.pending_count() != 0) {
                return {
                    GpuStatus::invalid_argument,
                    E_UNEXPECTED,
                    {}};
            }
        }
        internal::GpuMoveInference replacement;
        const GpuError created = internal::GpuMoveInference::create(
            device.Get(), config, replacement);
        if (!created) return created;
        move_inference = std::move(replacement);
        return {};
    }

    void drain_move_results_locked() noexcept {
        if (!move_inference.initialized()
            || bus_state == nullptr
            || bus_producer_token == 0) {
            return;
        }
        for (;;) {
            internal::GpuMoveInferenceResult inferred;
            bool ready = false;
            const GpuError resolved = move_inference.try_resolve(
                inferred, ready);
            if (!resolved) {
                record_error(shared_bus_error(
                    resolved, "failed to resolve GPU move inference"));
                if (resolved.status == GpuStatus::device_lost) {
                    running.store(false, std::memory_order_release);
                }
                return;
            }
            if (!ready) return;

            SharedFrameBusMoveResult result;
            result.epoch = epoch;
            result.epoch_nonce = epoch_nonce;
            result.sequence = inferred.sequence;
            result.base_sequence = inferred.base_sequence;
            result.flags = shared_frame_bus_move_result_valid;
            if ((inferred.flags & internal::gpu_move_result_fail_closed) != 0) {
                result.flags |= shared_frame_bus_move_result_fail_closed;
            }
            if ((inferred.flags
                    & internal::gpu_move_result_capacity_exceeded) != 0) {
                result.flags |=
                    shared_frame_bus_move_result_capacity_exceeded;
            }

            bool mapping_failed = false;
            if ((result.flags
                    & shared_frame_bus_move_result_fail_closed) == 0) {
                const auto inference_config = move_inference.config();
                internal::SideDataGeometry geometry;
                geometry.source_width = inference_config.width;
                geometry.source_height = inference_config.height;
                geometry.output_width = bus_config.width;
                geometry.output_height = bus_config.height;
                geometry.planar_420 = bus_planar_output;
                mapping_failed =
                    internal::scaled_side_data_geometry(geometry);
                for (std::uint32_t index = 0;
                     !mapping_failed && index < inferred.move_count;
                     ++index) {
                    WgcMoveRect mapped;
                    if (!internal::map_move_rect_exact(
                            inferred.moves[index].rectangle,
                            geometry,
                            mapped)) {
                        mapping_failed = true;
                        break;
                    }
                    result.move_rects[result.move_count++] = mapped;
                }
            }
            if (mapping_failed) {
                // The frame's conservatively mapped dirty rectangles remain
                // authoritative. An asynchronous amendment may add only moves
                // that can be replayed exactly in bus coordinates.
                result.move_count = 0;
                result.move_rects = {};
                result.flags &= ~(
                    shared_frame_bus_move_result_inferred
                    | shared_frame_bus_move_result_capacity_exceeded);
                result.flags |= shared_frame_bus_move_result_fail_closed;
            } else if (result.move_count != 0) {
                result.flags |= shared_frame_bus_move_result_inferred;
            }
            const GpuError published =
                internal::publish_shared_frame_bus_move_result(
                    bus_state, bus_producer_token, result);
            if (!published) {
                record_error(shared_bus_error(
                    published, "failed to publish GPU move inference"));
                if (published.status == GpuStatus::device_lost) {
                    running.store(false, std::memory_order_release);
                }
                return;
            }
            move_results_published.fetch_add(1, std::memory_order_relaxed);
            if ((result.flags
                    & shared_frame_bus_move_result_fail_closed) != 0) {
                move_inference_fail_closed.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    }

    void wake_move_result_worker() noexcept {
        {
            std::lock_guard lock(move_result_mutex);
            move_result_wake = true;
        }
        move_result_cv.notify_one();
    }

    void move_result_worker_main(std::stop_token stop_token) noexcept {
        for (;;) {
            {
                std::unique_lock lock(move_result_mutex);
                move_result_cv.wait(lock, [&] {
                    return stop_token.stop_requested() || move_result_wake;
                });
                if (stop_token.stop_requested()) return;
                move_result_wake = false;
            }
            for (;;) {
                std::uint32_t pending = 0;
                {
                    std::lock_guard callback_lock(callback_mutex);
                    drain_move_results_locked();
                    pending = move_inference.pending_count();
                }
                if (stop_token.stop_requested() || pending == 0) break;
                std::unique_lock lock(move_result_mutex);
                move_result_cv.wait_for(
                    lock,
                    std::chrono::milliseconds(1),
                    [&] { return stop_token.stop_requested(); });
                if (stop_token.stop_requested()) return;
            }
        }
    }

    void stop_move_result_worker() noexcept {
        if (!move_result_worker.joinable()) return;
        move_result_worker.request_stop();
        move_result_cv.notify_all();
        move_result_worker.join();
    }

    WgcResult initialize_device(ID3D11Device* supplied) {
        if (supplied != nullptr) {
            device = supplied;
            device->GetImmediateContext(&context);
        } else {
            constexpr D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0};
            D3D_FEATURE_LEVEL created_level{};
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
                | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            HRESULT hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                levels,
                static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,
                &device,
                &created_level,
                &context);
            if (hr == E_INVALIDARG) {
                hr = D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_HARDWARE,
                    nullptr,
                    flags,
                    levels + 1,
                    1,
                    D3D11_SDK_VERSION,
                    &device,
                    &created_level,
                    &context);
            }
            if (FAILED(hr)) {
                return make_result(
                    WgcStatus::d3d_error,
                    hr,
                    "D3D11CreateDevice failed");
            }
        }
        if (device == nullptr || context == nullptr) {
            return make_result(WgcStatus::d3d_error, E_FAIL, "D3D11 device has no immediate context");
        }

        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread))) {
            multithread->SetMultithreadProtected(TRUE);
        }

        HRESULT hr = internal::create_winrt_d3d_device(device.Get(), winrt_device);
        if (FAILED(hr)) {
            return make_result(
                WgcStatus::d3d_error,
                hr,
                "failed to create the WinRT Direct3D device bridge");
        }
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            return make_result(WgcStatus::capture_error, HRESULT_FROM_WIN32(GetLastError()));
        }
        qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
        return make_result(WgcStatus::ok);
    }

    WgcResult initialize_item_for_window(HWND window) {
        if (window == nullptr || !IsWindow(window)) {
            return make_result(WgcStatus::invalid_argument, E_INVALIDARG, "window handle is invalid");
        }
        try {
            auto interop = winrt::get_activation_factory<
                capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            capture::GraphicsCaptureItem created{nullptr};
            const HRESULT hr = interop->CreateForWindow(
                window,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(created));
            if (FAILED(hr)) {
                return make_result(WgcStatus::capture_error, hr, "CreateForWindow failed");
            }
            item = std::move(created);
            return initialize_pool();
        } catch (...) {
            return exception_result();
        }
    }

    WgcResult initialize_item_for_monitor(HMONITOR monitor) {
        if (monitor == nullptr) {
            return make_result(WgcStatus::invalid_argument, E_INVALIDARG, "monitor handle is invalid");
        }
        try {
            auto interop = winrt::get_activation_factory<
                capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            capture::GraphicsCaptureItem created{nullptr};
            const HRESULT hr = interop->CreateForMonitor(
                monitor,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(created));
            if (FAILED(hr)) {
                return make_result(WgcStatus::capture_error, hr, "CreateForMonitor failed");
            }
            item = std::move(created);
            return initialize_pool();
        } catch (...) {
            return exception_result();
        }
    }

    WgcResult initialize_pool() {
        if (!capture::GraphicsCaptureSession::IsSupported()) {
            return make_result(WgcStatus::not_supported, E_NOINTERFACE, "Windows Graphics Capture is not supported");
        }
        if (move_inference_requested() && !bus_only) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "asynchronous move inference requires a SharedFrameBus v4 capture");
        }
        const auto size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) {
            return make_result(WgcStatus::invalid_argument, E_INVALIDARG, "capture target has an empty size");
        }
        pool_size = size;
        mailbox_source_width = static_cast<std::uint32_t>(size.Width);
        mailbox_source_height = static_cast<std::uint32_t>(size.Height);
        WgcMailboxFrameInfo initial_region;
        mailbox_region_available = resolve_output_region(
            mailbox_config,
            mailbox_source_width,
            mailbox_source_height,
            mailbox_generation,
            initial_region);
        if (bus_only && bus_planar_output && mailbox_region_available) {
            D3D11_TEXTURE2D_DESC source_description{};
            source_description.Width = mailbox_source_width;
            source_description.Height = mailbox_source_height;
            source_description.MipLevels = 1;
            source_description.ArraySize = 1;
            source_description.Format = capture_dxgi_format(
                options.pixel_format);
            source_description.SampleDesc.Count = 1;
            const GpuError preflight = ensure_planar_bus_transform(
                source_description, initial_region);
            if (!preflight) {
                return shared_bus_error(
                    preflight,
                    "planar WGC bus conversion preflight failed");
            }
        }
        frame_pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrt_device,
            capture_pixel_format(options.pixel_format),
            2,
            pool_size);
        session = frame_pool.CreateCaptureSession(item);

        native_damage_enabled = false;
        if (options.damage_mode == WgcDamageMode::native_report_only
            || move_inference_requested()) {
            try {
                const auto session4 =
                    session.try_as<capture::IGraphicsCaptureSession4>();
                if (session4 != nullptr) {
                    session4.DirtyRegionMode(
                        capture::GraphicsCaptureDirtyRegionMode::ReportOnly);
                    native_damage_enabled = true;
                }
            } catch (...) {
                native_damage_enabled = false;
                note_property_failure(wgc_property_failure_dirty_region);
            }
        }
        if (move_inference_requested() && !native_damage_enabled) {
            return make_result(
                WgcStatus::not_supported,
                E_NOINTERFACE,
                "GPU move inference requires native WGC dirty regions");
        }
        force_full_damage(true);

        const bool cursor_capture_control_present =
            property_present(L"IsCursorCaptureEnabled");
        if (options.include_cursor_metadata
            && !cursor_capture_control_present) {
            return make_result(
                WgcStatus::not_supported,
                E_NOINTERFACE,
                "independent cursor metadata requires WGC cursor capture control");
        }
        if (cursor_capture_control_present) {
            try {
                session.IsCursorCaptureEnabled(
                    options.include_cursor_metadata
                        ? false
                        : options.include_cursor);
            } catch (...) {
                if (options.include_cursor_metadata) {
                    return exception_result();
                }
                note_property_failure(wgc_property_failure_cursor);
            }
        }
        if (property_present(L"IsBorderRequired")) {
            try {
                session.IsBorderRequired(options.require_border);
            } catch (...) {
                note_property_failure(wgc_property_failure_border);
            }
        }
        if (property_present(L"IncludeSecondaryWindows")) {
            try {
                session.IncludeSecondaryWindows(options.include_secondary_windows);
            } catch (...) {
                note_property_failure(wgc_property_failure_secondary_windows);
            }
        }
        if (options.min_update_interval_us != 0
            && property_present(L"MinUpdateInterval")) {
            try {
                session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{
                    static_cast<std::int64_t>(options.min_update_interval_us) * 10});
            } catch (...) {
                note_property_failure(
                    wgc_property_failure_min_update_interval);
            }
        }

        if (!bus_only) {
            slots.reserve(options.buffer_count);
            for (std::uint32_t index = 0; index < options.buffer_count; ++index) {
                slots.push_back(std::make_unique<Slot>());
            }
        }
        return make_result(WgcStatus::ok);
    }

    WgcResult start() {
        std::lock_guard lock(lifecycle_mutex);
        if (started.load(std::memory_order_acquire)
            || stopped.load(std::memory_order_acquire)) {
            return make_result(WgcStatus::invalid_state, E_UNEXPECTED, "capture session cannot be started in its current state");
        }
        try {
            const std::weak_ptr<WgcCaptureState> weak = shared_from_this();
            frame_token = frame_pool.FrameArrived(
                [weak](const capture::Direct3D11CaptureFramePool& sender, const winrt::Windows::Foundation::IInspectable&) {
                    if (auto state = weak.lock()) {
                        state->on_frame(sender);
                    }
                });
            closed_token = item.Closed(
                [weak](const capture::GraphicsCaptureItem&, const winrt::Windows::Foundation::IInspectable&) {
                    if (auto state = weak.lock()) {
                        state->on_closed();
                    }
                });
            started.store(true, std::memory_order_release);
            running.store(true, std::memory_order_release);
            if (move_inference_requested()) {
                move_result_worker = std::jthread(
                    [this](std::stop_token stop_token) noexcept {
                        move_result_worker_main(stop_token);
                    });
            }
            session.StartCapture();
            return make_result(WgcStatus::ok);
        } catch (...) {
            running.store(false, std::memory_order_release);
            started.store(false, std::memory_order_release);
            unregister_events();
            stop_move_result_worker();
            return exception_result();
        }
    }

    void stop() noexcept {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        if (stopped.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        running.store(false, std::memory_order_release);
        unregister_events();
        stop_move_result_worker();
        {
            std::lock_guard callback_lock(callback_mutex);
            try {
                if (session != nullptr) {
                    session.Close();
                }
                if (frame_pool != nullptr) {
                    frame_pool.Close();
                }
            } catch (...) {
            }
        }
        if (bus_state != nullptr && bus_producer_token != 0) {
            internal::release_shared_frame_bus_producer(
                bus_state, bus_producer_token);
            bus_producer_token = 0;
        }
        bus_state.reset();
        event_cv.notify_all();
    }

    void unregister_events() noexcept {
        try {
            if (frame_pool != nullptr && frame_token.value != 0) {
                frame_pool.FrameArrived(frame_token);
                frame_token = {};
            }
        } catch (...) {
        }
        try {
            if (item != nullptr && closed_token.value != 0) {
                item.Closed(closed_token);
                closed_token = {};
            }
        } catch (...) {
        }
    }

    void on_closed() noexcept {
        closed.store(true, std::memory_order_release);
        running.store(false, std::memory_order_release);
        event_cv.notify_all();
    }

    void discard_ready_slots_locked() noexcept {
        for (auto& slot : slots) {
            auto control = slot->control.load(std::memory_order_acquire);
            while (control_state(control) == SlotState::ready
                && !slot->control.compare_exchange_weak(
                    control,
                    make_control(control_generation(control), SlotState::free),
                    std::memory_order_acq_rel)) {
            }
        }
    }

    WgcResult set_mailbox_config(const WgcMailboxConfig& config) {
        if (!valid_mailbox_config(config)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "invalid WGC mailbox configuration");
        }

        std::lock_guard callback_lock(callback_mutex);
        if (stopped.load(std::memory_order_acquire)) {
            return make_result(
                WgcStatus::invalid_state,
                E_UNEXPECTED,
                "capture session has already stopped");
        }
        if (same_mailbox_config(mailbox_config, config)) {
            return make_result(WgcStatus::ok);
        }
        if (!mailbox_dimensions_match_bus(
                config, mailbox_source_width, mailbox_source_height)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "WGC mailbox output dimensions do not match the shared frame bus; create a new bus epoch");
        }
        bool available = false;
        {
            std::lock_guard event_lock(event_mutex);
            if (mailbox_generation == std::numeric_limits<std::uint64_t>::max()) {
                return make_result(
                    WgcStatus::invalid_state,
                    E_FAIL,
                    "WGC mailbox generation exhausted");
            }
            mailbox_config = config;
            ++mailbox_generation;
            WgcMailboxFrameInfo resolved;
            available = resolve_output_region(
                mailbox_config,
                mailbox_source_width,
                mailbox_source_height,
                mailbox_generation,
                resolved);
            mailbox_region_available = available;
            discard_ready_slots_locked();
        }
        force_full_damage(true);
        pending_damage_base_sequence = sequence;
        event_cv.notify_all();
        if (available) {
            clear_error();
        } else {
            record_error(make_result(
                WgcStatus::region_unavailable,
                E_BOUNDS,
                "WGC mailbox region exceeds the current source dimensions"));
        }
        return make_result(WgcStatus::ok);
    }

    WgcMailboxState read_mailbox_state() const noexcept {
        try {
            std::lock_guard callback_lock(callback_mutex);
            std::lock_guard event_lock(event_mutex);
            return {
                mailbox_config,
                mailbox_source_width,
                mailbox_source_height,
                mailbox_generation,
                mailbox_region_available};
        } catch (...) {
            return {};
        }
    }

    WgcMailboxFrameInfo read_mailbox_frame_info(
        std::uint32_t index,
        std::uint64_t token) const noexcept {
        if (index >= slots.size() || token == 0) return {};
        const auto control = slots[index]->control.load(std::memory_order_acquire);
        if (control_state(control) != SlotState::reading
            || control_generation(control) != token) {
            return {};
        }
        return slots[index]->mailbox;
    }

    void force_full_damage(bool discontinuity = false) noexcept {
        pending_damage_full = true;
        pending_dirty_rects.clear();
        pending_damage_discontinuity =
            pending_damage_discontinuity || discontinuity;
    }

    void collect_frame_damage(
        const capture::Direct3D11CaptureFrame& frame) noexcept {
        if (!native_damage_enabled) {
            force_full_damage();
            return;
        }
        try {
            const auto frame2 = frame.try_as<capture::IDirect3D11CaptureFrame2>();
            if (frame2 == nullptr) {
                force_full_damage();
                return;
            }
            const auto regions = frame2.DirtyRegions();
            for (const auto& region : regions) {
                if (region.Width <= 0 || region.Height <= 0) continue;
                if (pending_damage_full) continue;
                if (pending_dirty_rects.size() >= wgc_max_dirty_rects) {
                    pending_damage_overflow = true;
                    force_full_damage();
                    continue;
                }
                pending_dirty_rects.push_back({
                    region.X,
                    region.Y,
                    static_cast<std::uint32_t>(region.Width),
                    static_cast<std::uint32_t>(region.Height)});
            }
        } catch (...) {
            force_full_damage();
        }
    }

    WgcFrameDamage current_damage(
        const WgcMailboxFrameInfo& mailbox) const noexcept {
        const internal::SideDataGeometry geometry =
            side_data_geometry(mailbox);
        std::uint32_t base_flags = wgc_damage_valid
            | wgc_damage_native_move_unavailable;
        if (pending_damage_overflow) base_flags |= wgc_damage_overflow;
        if (pending_damage_discontinuity) {
            base_flags |= wgc_damage_discontinuity;
        }
        const auto full_frame = [&](std::uint32_t extra_flags) noexcept {
            WgcFrameDamage full;
            full.base_sequence = pending_damage_base_sequence;
            full.flags = base_flags | wgc_damage_full_frame | extra_flags;
            full.dirty_count = 1;
            full.dirty_rects[0] = {
                0, 0, geometry.output_width, geometry.output_height};
            return full;
        };
        if (!internal::valid_side_data_geometry(geometry)) {
            return full_frame(
                wgc_damage_overflow | wgc_damage_discontinuity);
        }
        // VideoProcessor scaling uses a driver-selected spatial kernel. A
        // source-space projection cannot prove final-byte coverage until a
        // deterministic backend is selected. Keep 1:1 planar conversion on
        // the existing chroma-aligned contract, but fail closed for scaling.
        if (bus_planar_output
            && internal::scaled_side_data_geometry(geometry)
            && !bus_transform.spatially_deterministic()) {
            return full_frame(0);
        }
        if (pending_damage_full || sequence == 0) {
            return full_frame(
                sequence == 0 ? wgc_damage_discontinuity : 0u);
        }

        WgcFrameDamage output;
        output.base_sequence = pending_damage_base_sequence;
        output.flags = base_flags | wgc_damage_native;
        for (const WgcRect& source : pending_dirty_rects) {
            WgcRect mapped;
            const internal::SideDataMapStatus status =
                bus_planar_output
                    && bus_transform.spatially_deterministic()
                ? internal::map_source_deterministic_planar_damage_rect(
                    source, mailbox.x, mailbox.y, geometry, mapped)
                : internal::map_source_damage_rect(
                    source, mailbox.x, mailbox.y, geometry, mapped);
            if (status == internal::SideDataMapStatus::empty) {
                continue;
            }
            if (status == internal::SideDataMapStatus::unrepresentable) {
                return full_frame(
                    wgc_damage_overflow | wgc_damage_discontinuity);
            }
            if (output.dirty_count >= wgc_max_dirty_rects) {
                return full_frame(wgc_damage_overflow);
            }
            output.dirty_rects[output.dirty_count++] = mapped;
        }
        return output;
    }

    void current_inference_dirty_rects(
        const WgcMailboxFrameInfo& mailbox,
        bool force_full,
        std::array<WgcRect, wgc_max_dirty_rects>& output,
        std::uint32_t& count) const noexcept {
        output = {};
        count = 0;
        const auto use_full_frame = [&]() noexcept {
            output = {};
            output[0] = {0, 0, mailbox.width, mailbox.height};
            count = 1;
        };
        if (force_full || pending_damage_full || sequence == 0) {
            use_full_frame();
            return;
        }
        for (const WgcRect& source : pending_dirty_rects) {
            WgcRect local;
            const internal::SideDataMapStatus status =
                internal::clip_source_rect_to_region(
                    source,
                    mailbox.x,
                    mailbox.y,
                    mailbox.width,
                    mailbox.height,
                    local);
            if (status == internal::SideDataMapStatus::empty) continue;
            if (status == internal::SideDataMapStatus::unrepresentable
                || count == output.size()) {
                use_full_frame();
                return;
            }
            output[count++] = local;
        }
    }

    void complete_damage_publish(
        const WgcFrameDamage& damage,
        std::uint64_t published_sequence) noexcept {
        if ((damage.flags & wgc_damage_native) != 0) {
            native_damage_frames.fetch_add(1, std::memory_order_relaxed);
        }
        if ((damage.flags & wgc_damage_full_frame) != 0) {
            full_damage_frames.fetch_add(1, std::memory_order_relaxed);
        }
        pending_dirty_rects.clear();
        pending_damage_full = false;
        pending_damage_overflow = false;
        pending_damage_discontinuity = false;
        pending_damage_base_sequence = published_sequence;
    }

    bool capture_origin(RECT& output, bool& estimated) const noexcept {
        estimated = false;
        if (target_monitor != nullptr) {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(target_monitor, &info)) return false;
            output = info.rcMonitor;
            return true;
        }
        if (target_window == nullptr || !IsWindow(target_window)) return false;
        estimated = true;
        if (SUCCEEDED(DwmGetWindowAttribute(
                target_window,
                DWMWA_EXTENDED_FRAME_BOUNDS,
                &output,
                sizeof(output)))) {
            return true;
        }
        return GetWindowRect(target_window, &output) != FALSE;
    }

    WgcCursorInfo sample_cursor(
        const WgcMailboxFrameInfo& mailbox) noexcept {
        WgcCursorInfo output;
        if (!options.include_cursor_metadata) return output;
        const internal::SideDataGeometry geometry =
            side_data_geometry(mailbox);
        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        output.sample_qpc = sample_publication_qpc();
        if (!GetCursorInfo(&cursor)) return output;
        output.screen_x = cursor.ptScreenPos.x;
        output.screen_y = cursor.ptScreenPos.y;

        RECT origin{};
        bool estimated = false;
        if (capture_origin(origin, estimated)) {
            const std::int64_t local_x =
                static_cast<std::int64_t>(cursor.ptScreenPos.x)
                - origin.left - mailbox.x;
            const std::int64_t local_y =
                static_cast<std::int64_t>(cursor.ptScreenPos.y)
                - origin.top - mailbox.y;
            if (internal::scale_signed_coordinate_nearest(
                    local_x,
                    geometry.source_width,
                    geometry.output_width,
                    output.frame_x)
                && internal::scale_signed_coordinate_nearest(
                    local_y,
                    geometry.source_height,
                    geometry.output_height,
                    output.frame_y)) {
                output.flags |= wgc_cursor_position_valid;
                if (estimated) {
                    output.flags |= wgc_cursor_position_estimated;
                }
            }
        }
        if ((cursor.flags & CURSOR_SHOWING) != 0) {
            output.flags |= wgc_cursor_visible;
        }

        const std::uint64_t refresh_ticks = qpc_frequency == 0
            ? 0
            : (static_cast<std::uint64_t>(
                    options.cursor_shape_refresh_interval_ms) * qpc_frequency
                + 999u) / 1'000u;
        const bool geometry_changed =
            last_cursor_source_width != geometry.source_width
            || last_cursor_source_height != geometry.source_height
            || last_cursor_output_width != geometry.output_width
            || last_cursor_output_height != geometry.output_height;
        const bool refresh_shape = cursor.hCursor != last_cursor_handle
            || geometry_changed
            || last_cursor_shape_qpc == 0
            || output.sample_qpc >= last_cursor_shape_qpc + refresh_ticks;
        if (refresh_shape) {
            WgcCursorShape source_shape;
            WgcCursorShape shape;
            if (cursor.hCursor != nullptr
                && extract_cursor_shape(cursor.hCursor, source_shape)
                && internal::scale_cursor_shape(
                    source_shape, geometry, shape)) {
                std::lock_guard lock(cursor_mutex);
                const bool known = std::any_of(
                    cursor_shapes.begin(),
                    cursor_shapes.end(),
                    [&](const WgcCursorShape& value) {
                        return value.sequence == shape.sequence;
                    });
                if (!known) {
                    cursor_shapes.push_back(shape);
                    if (cursor_shapes.size() > kCursorShapeCacheSize) {
                        cursor_shapes.pop_front();
                    }
                    cursor_shape_updates.fetch_add(1, std::memory_order_relaxed);
                }
                last_cursor_shape_sequence = shape.sequence;
            } else {
                // A new handle or animated-frame refresh must fail closed.
                // Reusing the previous sequence would label stale pixels as
                // the current system cursor shape.
                last_cursor_shape_sequence = 0;
            }
            last_cursor_handle = cursor.hCursor;
            last_cursor_shape_qpc = output.sample_qpc;
            last_cursor_source_width = geometry.source_width;
            last_cursor_source_height = geometry.source_height;
            last_cursor_output_width = geometry.output_width;
            last_cursor_output_height = geometry.output_height;
        }

        output.shape_sequence = last_cursor_shape_sequence;
        {
            std::lock_guard lock(cursor_mutex);
            const auto found = std::find_if(
                cursor_shapes.begin(),
                cursor_shapes.end(),
                [&](const WgcCursorShape& value) {
                    return value.sequence == output.shape_sequence;
                });
            if (found != cursor_shapes.end()) {
                output.width = found->width;
                output.height = found->height;
                output.hotspot_x = found->hotspot_x;
                output.hotspot_y = found->hotspot_y;
            } else if (output.shape_sequence != 0) {
                output.flags |= wgc_cursor_shape_pending;
            }
        }

        return output;
    }

    WgcResult read_cursor_shape(
        std::uint64_t shape_sequence,
        WgcCursorShape& output) const noexcept {
        output = {};
        if (!options.include_cursor_metadata || shape_sequence == 0) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "cursor shape sequence is unavailable");
        }
        try {
            std::lock_guard lock(cursor_mutex);
            const auto found = std::find_if(
                cursor_shapes.begin(),
                cursor_shapes.end(),
                [&](const WgcCursorShape& value) {
                    return value.sequence == shape_sequence;
                });
            if (found == cursor_shapes.end()) {
                return make_result(
                    WgcStatus::no_buffer,
                    HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
                    "cursor shape is no longer cached");
            }
            output = *found;
            return make_result(WgcStatus::ok);
        } catch (const std::bad_alloc&) {
            return make_result(WgcStatus::out_of_memory, E_OUTOFMEMORY);
        } catch (...) {
            return make_result(WgcStatus::capture_error, E_FAIL);
        }
    }

    void on_frame(const capture::Direct3D11CaptureFramePool& sender) noexcept {
        std::lock_guard callback_lock(callback_mutex);
        if (!running.load(std::memory_order_acquire)) {
            return;
        }
        try {
            capture::Direct3D11CaptureFrame newest{nullptr};
            for (;;) {
                auto candidate = sender.TryGetNextFrame();
                if (candidate == nullptr) {
                    break;
                }
                received_frames.fetch_add(1, std::memory_order_relaxed);
                collect_frame_damage(candidate);
                if (newest != nullptr) {
                    dropped_at_source.fetch_add(1, std::memory_order_relaxed);
                    newest.Close();
                }
                newest = std::move(candidate);
            }
            if (newest == nullptr) {
                return;
            }

            const auto content_size = newest.ContentSize();
            ComPtr<ID3D11Texture2D> source;
            HRESULT hr = internal::get_texture_from_surface(newest.Surface(), source);
            if (FAILED(hr)) {
                record_error(make_result(WgcStatus::capture_error, hr, "failed to unwrap capture surface"));
                newest.Close();
                return;
            }
            D3D11_TEXTURE2D_DESC source_desc{};
            source->GetDesc(&source_desc);
            if (source_desc.Format != capture_dxgi_format(options.pixel_format)) {
                record_error(make_result(
                    WgcStatus::d3d_error,
                    E_UNEXPECTED,
                    "WGC surface format does not match the requested pixel format"));
                newest.Close();
                return;
            }
            const bool empty_content = content_size.Width <= 0
                || content_size.Height <= 0;
            const bool pool_needs_recreate = !empty_content
                && (content_size.Width != pool_size.Width
                    || content_size.Height != pool_size.Height);
            const bool surface_needs_recreate = !empty_content
                && (static_cast<std::uint32_t>(content_size.Width) > source_desc.Width
                    || static_cast<std::uint32_t>(content_size.Height)
                        > source_desc.Height);
            if (empty_content || pool_needs_recreate || surface_needs_recreate) {
                force_full_damage(true);
                const std::uint32_t pending_width = static_cast<std::uint32_t>(
                    std::max(0, content_size.Width));
                const std::uint32_t pending_height = static_cast<std::uint32_t>(
                    std::max(0, content_size.Height));
                WgcMailboxFrameInfo pending_mailbox;
                const bool available = resolve_output_region(
                    mailbox_config,
                    pending_width,
                    pending_height,
                    mailbox_generation,
                    pending_mailbox);
                {
                    std::lock_guard event_lock(event_mutex);
                    mailbox_source_width = pending_width;
                    mailbox_source_height = pending_height;
                    mailbox_region_available = available;
                    discard_ready_slots_locked();
                }
                event_cv.notify_all();
                if (!available) {
                    record_error(make_result(
                        WgcStatus::region_unavailable,
                        E_BOUNDS,
                        "WGC mailbox region is unavailable for the current source dimensions"));
                }
                newest.Close();
                recreate_if_needed(content_size);
                return;
            }
            const std::uint32_t width = static_cast<std::uint32_t>(
                content_size.Width);
            const std::uint32_t height = static_cast<std::uint32_t>(
                content_size.Height);

            WgcMailboxFrameInfo mailbox;
            if (!resolve_output_region(
                    mailbox_config,
                    width,
                    height,
                    mailbox_generation,
                    mailbox)) {
                force_full_damage(true);
                {
                    std::lock_guard event_lock(event_mutex);
                    mailbox_source_width = width;
                    mailbox_source_height = height;
                    mailbox_region_available = false;
                    discard_ready_slots_locked();
                }
                event_cv.notify_all();
                record_error(make_result(
                    WgcStatus::region_unavailable,
                    E_BOUNDS,
                    "WGC mailbox region exceeds the current source dimensions"));
                newest.Close();
                recreate_if_needed(content_size);
                return;
            }
            {
                std::lock_guard event_lock(event_mutex);
                mailbox_source_width = width;
                mailbox_source_height = height;
                mailbox_region_available = true;
            }

            D3D11_BOX box{};
            box.left = mailbox.x;
            box.top = mailbox.y;
            box.right = mailbox.x + mailbox.width;
            box.bottom = mailbox.y + mailbox.height;
            box.back = 1;
            if (bus_only && bus_planar_output) {
                const GpuError transform_ready =
                    ensure_planar_bus_transform(source_desc, mailbox);
                if (!transform_ready) {
                    record_error(shared_bus_error(
                        transform_ready,
                        "failed to configure planar WGC bus conversion"));
                    if (transform_ready.status == GpuStatus::device_lost) {
                        running.store(false, std::memory_order_release);
                    }
                    newest.Close();
                    return;
                }
            }
            WgcFrameDamage damage = current_damage(mailbox);
            WgcCursorInfo cursor = sample_cursor(mailbox);

            if (bus_only) {
                if (move_inference_requested()) {
                    drain_move_results_locked();
                }
                if (!bus_planar_output
                    && (mailbox.width != bus_config.width
                        || mailbox.height != bus_config.height)) {
                    {
                        std::lock_guard event_lock(event_mutex);
                        mailbox_source_width = width;
                        mailbox_source_height = height;
                        mailbox_region_available = false;
                        discard_ready_slots_locked();
                    }
                    event_cv.notify_all();
                    record_error(make_result(
                        WgcStatus::region_unavailable,
                        E_BOUNDS,
                        "WGC full-frame dimensions changed; create a new shared frame bus epoch"));
                    newest.Close();
                    recreate_if_needed(content_size);
                    return;
                }
                if (!bus_planar_output
                    && source_desc.Format != bus_config.format) {
                    record_error(make_result(
                        WgcStatus::d3d_error,
                        E_INVALIDARG,
                        "WGC surface format does not match the shared frame bus"));
                    newest.Close();
                    return;
                }
                if (sequence == std::numeric_limits<std::uint64_t>::max()) {
                    record_error(make_result(
                        WgcStatus::capture_error,
                        E_FAIL,
                        "capture sequence exhausted"));
                    newest.Close();
                    return;
                }

                if (cursor.shape_sequence != 0) {
                    if (cursor.shape_sequence
                        == last_bus_cursor_shape_sequence) {
                        cursor.flags &= ~wgc_cursor_shape_pending;
                    } else {
                        // The frame is committed before its out-of-band shape.
                        // Consumers must treat this first reference as pending;
                        // a later frame clears the flag after publication.
                        cursor.flags |= wgc_cursor_shape_pending;
                    }
                }

                SharedFrameBusWriteLease lease;
                const GpuError begun = internal::begin_shared_frame_bus_publish(
                    bus_state, bus_producer_token, lease);
                if (!begun) {
                    if (begun.status == GpuStatus::timeout) {
                        skipped_no_buffer.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        record_error(shared_bus_error(
                            begun, "failed to acquire a shared frame bus slot"));
                        if (begun.status == GpuStatus::device_lost) {
                            running.store(false, std::memory_order_release);
                        }
                    }
                    newest.Close();
                    return;
                }

                const std::uint64_t publish_sequence = lease.sequence();
                MoveInferenceSubmissionGuard inference_submission;
                bool inference_scheduled = false;
                if (move_inference_requested()) {
                    const bool had_inference = move_inference.initialized();
                    const GpuError inference_ready = ensure_move_inference(
                        source_desc, mailbox);
                    if (!inference_ready) {
                        lease.reset();
                        record_error(shared_bus_error(
                            inference_ready,
                            "failed to configure GPU move inference"));
                        running.store(false, std::memory_order_release);
                        newest.Close();
                        return;
                    }
                    if (had_inference
                        && (damage.flags & wgc_damage_discontinuity) != 0) {
                        move_inference.reset_baseline();
                    }
                    internal::GpuMoveInferenceSubmitInfo submit_info;
                    std::array<WgcRect, wgc_max_dirty_rects>
                        inference_dirty_rects{};
                    std::uint32_t inference_dirty_count = 0;
                    current_inference_dirty_rects(
                        mailbox,
                        (damage.flags & wgc_damage_full_frame) != 0,
                        inference_dirty_rects,
                        inference_dirty_count);
                    const auto dirty = std::span<const WgcRect>(
                        inference_dirty_rects.data(),
                        inference_dirty_count);
                    const GpuError submitted = move_inference.submit(
                        source.Get(), publish_sequence, dirty, submit_info);
                    if (!submitted) {
                        lease.reset();
                        record_error(shared_bus_error(
                            submitted,
                            "failed to submit GPU move inference"));
                        running.store(false, std::memory_order_release);
                        newest.Close();
                        return;
                    }
                    inference_submission.arm(
                        move_inference,
                        submit_info,
                        this,
                        [](void* context) noexcept {
                            static_cast<WgcCaptureState*>(context)
                                ->wake_move_result_worker();
                        });
                    move_inference_submissions.fetch_add(
                        1, std::memory_order_relaxed);
                    if ((submit_info.flags
                            & internal::gpu_move_submit_scheduled) != 0) {
                        damage.flags |= wgc_damage_move_pending;
                        inference_scheduled = true;
                    }
                    if ((submit_info.flags
                            & internal::gpu_move_submit_fail_closed) != 0) {
                        move_inference_fail_closed.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    if ((submit_info.flags
                            & internal::gpu_move_submit_no_readback_slot) != 0) {
                        move_inference_no_readback.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }

                if (bus_planar_output) {
                    const GpuError transformed = bus_transform.process_into(
                        source.Get(), lease.texture());
                    if (!transformed) {
                        (void)inference_submission.cancel();
                        lease.reset();
                        record_error(shared_bus_error(
                            transformed,
                            "failed to write the planar WGC bus slot"));
                        if (transformed.status == GpuStatus::device_lost) {
                            running.store(false, std::memory_order_release);
                        }
                        newest.Close();
                        return;
                    }
                    ingress_transform_submissions.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    context->CopySubresourceRegion(
                        lease.texture(),
                        0,
                        0,
                        0,
                        0,
                        source.Get(),
                        0,
                        &box);
                    ingress_copy_submissions.fetch_add(
                        1, std::memory_order_relaxed);
                }
                WgcCursorShape cursor_shape_to_publish;
                bool publish_cursor_shape_after_commit = false;
                if (cursor.shape_sequence != 0
                    && cursor.shape_sequence
                        != last_bus_cursor_shape_sequence) {
                    const WgcResult found = read_cursor_shape(
                        cursor.shape_sequence, cursor_shape_to_publish);
                    publish_cursor_shape_after_commit = static_cast<bool>(found);
                }
                SharedFrameBusFrameMetadata metadata;
                metadata.valid_fields =
                    shared_frame_bus_metadata_source_timestamp
                    | shared_frame_bus_metadata_qpc
                    | shared_frame_bus_metadata_source_dimensions
                    | shared_frame_bus_metadata_roi
                    | shared_frame_bus_metadata_mailbox_generation
                    | shared_frame_bus_metadata_color_space;
                metadata.source_timestamp_100ns =
                    newest.SystemRelativeTime().count();
                metadata.timestamp_qpc = sample_publication_qpc();
                metadata.qpc_frequency = qpc_frequency;
                metadata.mailbox_generation = mailbox.generation;
                metadata.source_width = mailbox.source_width;
                metadata.source_height = mailbox.source_height;
                metadata.roi_x = mailbox.x;
                metadata.roi_y = mailbox.y;
                metadata.roi_width = mailbox.width;
                metadata.roi_height = mailbox.height;
                metadata.color_space = bus_output_color_space;
                SharedFrameBusFrameSideData side_data;
                side_data.epoch = epoch;
                side_data.epoch_nonce = epoch_nonce;
                side_data.damage = damage;
                side_data.cursor = cursor;
                const GpuError committed =
                    internal::commit_shared_frame_bus_publish(
                        bus_state,
                        bus_producer_token,
                        std::move(lease),
                        metadata,
                         side_data);
                if (!committed) {
                    (void)inference_submission.cancel();
                    record_error(shared_bus_error(
                        committed, "failed to commit a shared WGC frame"));
                    if (committed.status == GpuStatus::device_lost) {
                        running.store(false, std::memory_order_release);
                    }
                    newest.Close();
                    return;
                }
                (void)inference_submission.finalize();

                if (publish_cursor_shape_after_commit) {
                    const GpuError shape_published =
                        internal::publish_shared_frame_bus_cursor_shape(
                            bus_state,
                            bus_producer_token,
                            cursor_shape_to_publish);
                    if (shape_published) {
                        last_bus_cursor_shape_sequence =
                            cursor.shape_sequence;
                    }
                }

                sequence = publish_sequence;
                complete_damage_publish(damage, sequence);
                if (options.include_cursor_metadata) {
                    cursor_metadata_frames.fetch_add(1, std::memory_order_relaxed);
                }
                published_frames.fetch_add(1, std::memory_order_relaxed);
                clear_error();
                if (inference_scheduled) {
                    drain_move_results_locked();
                    if (move_inference.pending_count() != 0) {
                        wake_move_result_worker();
                    }
                }
                newest.Close();
                return;
            }

            std::size_t slot_index = 0;
            Slot* slot = acquire_write_slot(slot_index);
            if (slot == nullptr) {
                skipped_no_buffer.fetch_add(1, std::memory_order_relaxed);
                newest.Close();
                recreate_if_needed(content_size);
                return;
            }

            hr = ensure_slot_texture(
                *slot, mailbox.width, mailbox.height, source_desc.Format);
            if (FAILED(hr)) {
                release_write_slot(*slot);
                record_error(make_result(WgcStatus::d3d_error, hr, "failed to allocate capture ring texture"));
                newest.Close();
                return;
            }

            context->CopySubresourceRegion(
                slot->texture.Get(),
                0,
                0,
                0,
                0,
                source.Get(),
                0,
                &box);
            ingress_copy_submissions.fetch_add(1, std::memory_order_relaxed);

            const std::uint64_t timestamp_qpc = sample_publication_qpc();
            const std::int64_t source_timestamp_100ns =
                newest.SystemRelativeTime().count();
            if (sequence == (std::numeric_limits<std::uint64_t>::max() >> kStateBits)) {
                release_write_slot(*slot);
                record_error(make_result(WgcStatus::capture_error, E_FAIL, "capture sequence exhausted"));
                newest.Close();
                return;
            }
            slot->info = {
                mailbox.width,
                mailbox.height,
                source_desc.Format,
                ++sequence,
                timestamp_qpc,
                qpc_frequency,
                source_timestamp_100ns,
                capture_color_space(options.pixel_format),
                epoch,
                epoch_nonce,
                sequence == 1 ? wgc_frame_discontinuity : 0u};
            slot->mailbox = mailbox;
            slot->damage = damage;
            slot->cursor = cursor;
            slot->mailbox_generation.store(
                mailbox.generation, std::memory_order_release);
            {
                std::lock_guard event_lock(event_mutex);
                slot->control.store(
                    make_control(sequence, SlotState::ready),
                    std::memory_order_release);
            }
            complete_damage_publish(damage, sequence);
            if (options.include_cursor_metadata) {
                cursor_metadata_frames.fetch_add(1, std::memory_order_relaxed);
            }
            published_frames.fetch_add(1, std::memory_order_relaxed);
            clear_error();
            event_cv.notify_all();
            newest.Close();
            recreate_if_needed(content_size);
        } catch (...) {
            record_error(exception_result());
        }
    }

    void recreate_if_needed(winrt::Windows::Graphics::SizeInt32 size) noexcept {
        if (size.Width <= 0 || size.Height <= 0
            || (size.Width == pool_size.Width && size.Height == pool_size.Height)
            || !running.load(std::memory_order_acquire)) {
            return;
        }
        try {
            frame_pool.Recreate(
                winrt_device,
                capture_pixel_format(options.pixel_format),
                2,
                size);
            pool_size = size;
            force_full_damage(true);
            size_changes.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            record_error(exception_result());
        }
    }

    HRESULT ensure_slot_texture(
        Slot& slot,
        std::uint32_t width,
        std::uint32_t height,
        DXGI_FORMAT format) noexcept {
        if (slot.texture != nullptr) {
            D3D11_TEXTURE2D_DESC current{};
            slot.texture->GetDesc(&current);
            if (current.Width == width && current.Height == height && current.Format == format) {
                return S_OK;
            }
            slot.texture.Reset();
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        return device->CreateTexture2D(&description, nullptr, &slot.texture);
    }

    Slot* acquire_write_slot(std::size_t& index_out) noexcept {
        for (std::size_t index = 0; index < slots.size(); ++index) {
            auto control = slots[index]->control.load(std::memory_order_acquire);
            while (control_state(control) == SlotState::free) {
                if (slots[index]->control.compare_exchange_weak(
                        control,
                        make_control(control_generation(control), SlotState::writing),
                        std::memory_order_acq_rel)) {
                    index_out = index;
                    return slots[index].get();
                }
            }
        }

        const bool preserve_mailbox = active_acquires.load(std::memory_order_acquire) != 0;
        for (std::size_t attempt = 0; attempt < slots.size(); ++attempt) {
            std::size_t oldest_index = slots.size();
            std::size_t ready_count = 0;
            std::uint64_t oldest_control = 0;
            std::uint64_t oldest_sequence = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t index = 0; index < slots.size(); ++index) {
                const auto control = slots[index]->control.load(std::memory_order_acquire);
                if (control_state(control) != SlotState::ready) {
                    continue;
                }
                ++ready_count;
                if (control_generation(control) < oldest_sequence) {
                    oldest_sequence = control_generation(control);
                    oldest_control = control;
                    oldest_index = index;
                }
            }
            if (oldest_index == slots.size() || (preserve_mailbox && ready_count <= 1)) {
                return nullptr;
            }
            if (slots[oldest_index]->control.compare_exchange_strong(
                    oldest_control,
                    make_control(oldest_sequence, SlotState::writing),
                    std::memory_order_acq_rel)) {
                overwritten_frames.fetch_add(1, std::memory_order_relaxed);
                index_out = oldest_index;
                return slots[oldest_index].get();
            }
        }
        return nullptr;
    }

    static void release_write_slot(Slot& slot) noexcept {
        const auto control = slot.control.load(std::memory_order_relaxed);
        slot.control.store(
            make_control(control_generation(control), SlotState::free),
            std::memory_order_release);
    }

    bool release_lease(std::uint32_t index, std::uint64_t token) noexcept {
        if (index >= slots.size()) {
            return false;
        }
        auto expected = make_control(token, SlotState::reading);
        return slots[index]->control.compare_exchange_strong(
            expected,
            make_control(token, SlotState::free),
            std::memory_order_acq_rel);
    }

    WgcResult acquire_latest(std::uint32_t timeout_ms, WgcFrameLease& output) {
        if (bus_only) {
            return make_result(
                WgcStatus::invalid_state,
                E_UNEXPECTED,
                "bus-only WGC capture has no private frame ring");
        }
        if (output) {
            return make_result(WgcStatus::invalid_state, E_UNEXPECTED, "output already owns a frame lease");
        }
        ActiveAcquireGuard guard(active_acquires);
        using Clock = std::chrono::steady_clock;
        const auto deadline = timeout_ms == kWaitInfinite
            ? Clock::time_point::max()
            : Clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock event_lock(event_mutex);
        const auto ready_or_finished = [this] {
            return has_ready_frame()
                || closed.load(std::memory_order_acquire)
                || !running.load(std::memory_order_acquire)
                || !mailbox_region_available;
        };

        for (;;) {
            while (!has_ready_frame()) {
                if (closed.load(std::memory_order_acquire)) {
                    return make_result(WgcStatus::target_closed, RO_E_CLOSED);
                }
                if (!running.load(std::memory_order_acquire)) {
                    return make_result(WgcStatus::invalid_state, E_UNEXPECTED, "capture session is not running");
                }
                if (!mailbox_region_available) {
                    return make_result(
                        WgcStatus::region_unavailable,
                        E_BOUNDS,
                        "WGC mailbox region exceeds the current source dimensions");
                }
                if (timeout_ms == 0) {
                    return make_result(WgcStatus::timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
                }
                if (timeout_ms == kWaitInfinite) {
                    event_cv.wait(event_lock, ready_or_finished);
                } else if (!event_cv.wait_until(event_lock, deadline, ready_or_finished)) {
                    return make_result(WgcStatus::timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
                }
            }

            std::size_t newest_index = slots.size();
            std::uint64_t newest_control = 0;
            std::uint64_t newest_sequence = 0;
            for (std::size_t index = 0; index < slots.size(); ++index) {
                const auto control = slots[index]->control.load(std::memory_order_acquire);
                if (control_state(control) == SlotState::ready
                    && slots[index]->mailbox_generation.load(
                        std::memory_order_acquire) == mailbox_generation
                    && control_generation(control) >= newest_sequence) {
                    newest_index = index;
                    newest_control = control;
                    newest_sequence = control_generation(control);
                }
            }
            if (newest_index == slots.size()) {
                if (timeout_ms != kWaitInfinite && Clock::now() >= deadline) {
                    return make_result(WgcStatus::timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
                }
                continue;
            }

            Slot& newest = *slots[newest_index];
            if (!newest.control.compare_exchange_strong(
                    newest_control,
                    make_control(newest_sequence, SlotState::reading),
                    std::memory_order_acq_rel)) {
                if (timeout_ms != kWaitInfinite && Clock::now() >= deadline) {
                    return make_result(WgcStatus::timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
                }
                continue;
            }

            const WgcFrameInfo info = newest.info;
            ID3D11Texture2D* texture = newest.texture.Get();
            output = WgcFrameLease(
                shared_from_this(),
                static_cast<std::uint32_t>(newest_index),
                newest_sequence,
                texture,
                info,
                newest.damage,
                newest.cursor);
            event_lock.unlock();

            for (std::size_t index = 0; index < slots.size(); ++index) {
                if (index == newest_index) {
                    continue;
                }
                auto control = slots[index]->control.load(std::memory_order_acquire);
                while (control_state(control) == SlotState::ready
                    && control_generation(control) < newest_sequence
                    && !slots[index]->control.compare_exchange_weak(
                        control,
                        make_control(control_generation(control), SlotState::free),
                        std::memory_order_acq_rel)) {
                }
            }
            return make_result(WgcStatus::ok);
        }
    }

    bool has_ready_frame() const noexcept {
        return std::any_of(slots.begin(), slots.end(), [this](const auto& slot) {
            return control_state(slot->control.load(std::memory_order_acquire))
                    == SlotState::ready
                && slot->mailbox_generation.load(std::memory_order_acquire)
                    == mailbox_generation;
        });
    }

    void record_error(WgcResult error) noexcept {
        try {
            std::lock_guard lock(error_mutex);
            last_failure = std::move(error);
        } catch (...) {
        }
    }

    void clear_error() noexcept {
        try {
            std::lock_guard lock(error_mutex);
            last_failure = make_result(WgcStatus::ok);
        } catch (...) {
        }
    }

    WgcResult read_error() const noexcept {
        try {
            std::lock_guard lock(error_mutex);
            return last_failure;
        } catch (...) {
            return {WgcStatus::capture_error, E_FAIL, {}};
        }
    }

    WgcCaptureOptions options{};
    WgcMailboxConfig mailbox_config{};
    HWND target_window = nullptr;
    HMONITOR target_monitor = nullptr;
    bool bus_only = false;
    std::shared_ptr<SharedFrameBusPublisherState> bus_state;
    SharedFrameBusConfig bus_config{};
    std::uint64_t bus_producer_token = 0;
    bool bus_planar_output = false;
    GpuTransform bus_transform;
    internal::GpuMoveInference move_inference;
    DXGI_COLOR_SPACE_TYPE bus_output_color_space = DXGI_COLOR_SPACE_CUSTOM;
    std::uint64_t mailbox_generation = 1;
    std::uint32_t mailbox_source_width = 0;
    std::uint32_t mailbox_source_height = 0;
    bool mailbox_region_available = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    direct3d::IDirect3DDevice winrt_device{nullptr};
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool frame_pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 pool_size{};
    winrt::event_token frame_token{};
    winrt::event_token closed_token{};
    std::vector<std::unique_ptr<Slot>> slots;
    std::uint64_t sequence = 0;
    std::uint64_t qpc_frequency = 0;
    std::uint64_t epoch = 1;
    std::uint64_t epoch_nonce = 1;
    bool native_damage_enabled = false;
    bool pending_damage_full = true;
    bool pending_damage_overflow = false;
    bool pending_damage_discontinuity = true;
    std::uint64_t pending_damage_base_sequence = 0;
    std::vector<WgcRect> pending_dirty_rects;

    mutable std::mutex cursor_mutex;
    std::deque<WgcCursorShape> cursor_shapes;
    HCURSOR last_cursor_handle = nullptr;
    std::uint64_t last_cursor_shape_qpc = 0;
    std::uint64_t last_cursor_shape_sequence = 0;
    std::uint64_t last_bus_cursor_shape_sequence = 0;
    std::uint32_t last_cursor_source_width = 0;
    std::uint32_t last_cursor_source_height = 0;
    std::uint32_t last_cursor_output_width = 0;
    std::uint32_t last_cursor_output_height = 0;

    std::mutex lifecycle_mutex;
    mutable std::mutex callback_mutex;
    std::mutex move_result_mutex;
    std::condition_variable move_result_cv;
    std::jthread move_result_worker;
    bool move_result_wake = false;
    mutable std::mutex event_mutex;
    std::condition_variable event_cv;
    mutable std::mutex error_mutex;
    WgcResult last_failure{};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> running{false};
    std::atomic<bool> closed{false};
    std::atomic<std::uint32_t> active_acquires{0};

    std::atomic<std::uint64_t> received_frames{0};
    std::atomic<std::uint64_t> published_frames{0};
    std::atomic<std::uint64_t> overwritten_frames{0};
    std::atomic<std::uint64_t> skipped_no_buffer{0};
    std::atomic<std::uint64_t> dropped_at_source{0};
    std::atomic<std::uint64_t> size_changes{0};
    std::atomic<std::uint64_t> ingress_copy_submissions{0};
    std::atomic<std::uint64_t> ingress_transform_submissions{0};
    std::atomic<std::uint64_t> native_damage_frames{0};
    std::atomic<std::uint64_t> full_damage_frames{0};
    std::atomic<std::uint64_t> move_inference_submissions{0};
    std::atomic<std::uint64_t> move_results_published{0};
    std::atomic<std::uint64_t> move_inference_fail_closed{0};
    std::atomic<std::uint64_t> move_inference_no_readback{0};
    std::atomic<std::uint64_t> cursor_metadata_frames{0};
    std::atomic<std::uint64_t> cursor_shape_updates{0};
    std::atomic<std::uint64_t> recovery_attempts{0};
    std::atomic<std::uint64_t> recovery_successes{0};
    std::atomic<std::uint32_t> property_failure_mask_{0};
    std::atomic<std::uint32_t> property_failure_count_{0};

    void note_property_failure(std::uint32_t bit) noexcept {
        property_failure_count_.fetch_add(1, std::memory_order_relaxed);
        property_failure_mask_.fetch_or(bit, std::memory_order_relaxed);
    }
};

WgcFrameLease::WgcFrameLease(
    std::shared_ptr<WgcCaptureState> state,
    std::uint32_t slot,
    std::uint64_t token,
    ID3D11Texture2D* texture,
    WgcFrameInfo info,
    WgcFrameDamage damage,
    WgcCursorInfo cursor) noexcept
    : state_(std::move(state)),
      slot_(slot),
      token_(token),
      texture_(texture),
      info_(info),
      damage_(damage),
      cursor_(cursor) {}

WgcFrameLease::~WgcFrameLease() { reset(); }

WgcFrameLease::WgcFrameLease(WgcFrameLease&& other) noexcept
    : state_(std::move(other.state_)),
      slot_(other.slot_),
      token_(other.token_),
      texture_(other.texture_),
      info_(other.info_),
      damage_(other.damage_),
      cursor_(other.cursor_) {
    other.texture_ = nullptr;
    other.token_ = 0;
}

WgcFrameLease& WgcFrameLease::operator=(WgcFrameLease&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        slot_ = other.slot_;
        token_ = other.token_;
        texture_ = other.texture_;
        info_ = other.info_;
        damage_ = other.damage_;
        cursor_ = other.cursor_;
        other.texture_ = nullptr;
        other.token_ = 0;
    }
    return *this;
}

WgcFrameLease::operator bool() const noexcept {
    return state_ != nullptr && texture_ != nullptr && token_ != 0;
}

ID3D11Texture2D* WgcFrameLease::texture() const noexcept { return texture_; }
const WgcFrameInfo& WgcFrameLease::info() const noexcept { return info_; }
const WgcFrameDamage& WgcFrameLease::damage() const noexcept { return damage_; }
const WgcCursorInfo& WgcFrameLease::cursor_info() const noexcept {
    return cursor_;
}
WgcMailboxFrameInfo WgcFrameLease::mailbox_info() const noexcept {
    return state_ != nullptr && token_ != 0
        ? state_->read_mailbox_frame_info(slot_, token_)
        : WgcMailboxFrameInfo{};
}

void WgcFrameLease::reset() noexcept {
    if (state_ != nullptr && token_ != 0) {
        (void)state_->release_lease(slot_, token_);
    }
    state_.reset();
    texture_ = nullptr;
    token_ = 0;
    info_ = {};
    damage_ = {};
    cursor_ = {};
}

WgcCapture::WgcCapture(std::shared_ptr<WgcCaptureState> state) noexcept
    : state_(std::move(state)) {}

WgcCapture::~WgcCapture() { stop(); }

WgcCapture::WgcCapture(WgcCapture&& other) noexcept = default;

WgcCapture& WgcCapture::operator=(WgcCapture&& other) noexcept {
    if (this != &other) {
        stop();
        state_ = std::move(other.state_);
    }
    return *this;
}

WgcResult WgcCapture::create_for_window(
    HWND window,
    ID3D11Device* device,
    const WgcCaptureOptions& options,
    WgcCapture& output) noexcept {
    return create_for_window(
        window, device, options, WgcMailboxConfig{}, output);
}

WgcResult WgcCapture::create_for_window(
    HWND window,
    ID3D11Device* device,
    const WgcCaptureOptions& options,
    const WgcMailboxConfig& mailbox,
    WgcCapture& output) noexcept {
    try {
        if (!valid_capture_options(options)) {
            return make_result(WgcStatus::invalid_argument, E_INVALIDARG, "invalid WGC capture options");
        }
        if (!valid_mailbox_config(mailbox)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "invalid WGC mailbox configuration");
        }
        auto state = std::make_shared<WgcCaptureState>();
        state->options = options;
        state->epoch = options.capture_epoch;
        state->epoch_nonce = options.capture_epoch_nonce;
        state->mailbox_config = mailbox;
        state->target_window = window;
        WgcResult result = state->initialize_device(device);
        if (!result) {
            return result;
        }
        result = state->initialize_item_for_window(window);
        if (!result) {
            return result;
        }
        output.stop();
        output = WgcCapture(std::move(state));
        return make_result(WgcStatus::ok);
    } catch (...) {
        return exception_result();
    }
}

WgcResult WgcCapture::create_for_monitor(
    HMONITOR monitor,
    ID3D11Device* device,
    const WgcCaptureOptions& options,
    WgcCapture& output) noexcept {
    return create_for_monitor(
        monitor, device, options, WgcMailboxConfig{}, output);
}

WgcResult WgcCapture::create_for_monitor(
    HMONITOR monitor,
    ID3D11Device* device,
    const WgcCaptureOptions& options,
    const WgcMailboxConfig& mailbox,
    WgcCapture& output) noexcept {
    try {
        if (!valid_capture_options(options)) {
            return make_result(WgcStatus::invalid_argument, E_INVALIDARG, "invalid WGC capture options");
        }
        if (!valid_mailbox_config(mailbox)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "invalid WGC mailbox configuration");
        }
        auto state = std::make_shared<WgcCaptureState>();
        state->options = options;
        state->epoch = options.capture_epoch;
        state->epoch_nonce = options.capture_epoch_nonce;
        state->mailbox_config = mailbox;
        state->target_monitor = monitor;
        WgcResult result = state->initialize_device(device);
        if (!result) {
            return result;
        }
        result = state->initialize_item_for_monitor(monitor);
        if (!result) {
            return result;
        }
        output.stop();
        output = WgcCapture(std::move(state));
        return make_result(WgcStatus::ok);
    } catch (...) {
        return exception_result();
    }
}

WgcResult WgcCapture::create_for_window_to_bus(
    HWND window,
    SharedFrameBusPublisher& publisher,
    const WgcCaptureOptions& options,
    const WgcMailboxConfig& mailbox,
    WgcCapture& output) noexcept {
    try {
        if (!valid_capture_options(options)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "invalid WGC capture options");
        }
        if (!valid_mailbox_config(mailbox)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "invalid WGC mailbox configuration");
        }
        if (publisher.state_ == nullptr) {
            return make_result(
                WgcStatus::invalid_argument,
                E_UNEXPECTED,
                "shared frame bus publisher is uninitialized");
        }

        auto state = std::make_shared<WgcCaptureState>();
        state->options = options;
        state->epoch = options.capture_epoch;
        state->epoch_nonce = options.capture_epoch_nonce;
        state->mailbox_config = mailbox;
        state->target_window = window;
        state->bus_only = true;
        state->bus_state = publisher.state_;
        state->bus_config = internal::shared_frame_bus_config(state->bus_state);
        state->bus_planar_output = planar_bus_format(state->bus_config.format);
        state->bus_output_color_space = resolved_bus_color_space(
            state->bus_config, options.pixel_format);
        if (!state->bus_planar_output
            && state->bus_config.format
                != capture_dxgi_format(options.pixel_format)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "WGC direct bus format does not match the requested capture pixel format");
        }
        if (!state->bus_planar_output
            && state->bus_output_color_space
                != capture_color_space(options.pixel_format)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "WGC direct bus color space does not match the requested capture pixel format");
        }
        if (state->bus_planar_output
            && (((state->bus_config.width | state->bus_config.height) & 1u) != 0
                || (state->bus_config.bind_flags
                    & D3D11_BIND_RENDER_TARGET) == 0)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "planar WGC bus dimensions must be even and render-target bound");
        }

        WgcResult result = state->initialize_device(
            internal::shared_frame_bus_device(state->bus_state));
        if (!result) return result;
        result = state->initialize_item_for_window(window);
        if (!result) return result;
        if (!state->mailbox_dimensions_match_bus(
                mailbox,
                state->mailbox_source_width,
                state->mailbox_source_height)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "WGC mailbox output dimensions do not match the shared frame bus");
        }

        const GpuError reserved = internal::reserve_shared_frame_bus_producer(
            state->bus_state, state->bus_producer_token);
        if (!reserved) {
            return make_result(
                WgcStatus::invalid_state,
                FAILED(reserved.hresult) ? reserved.hresult : E_UNEXPECTED,
                reserved.what());
        }
        state->sequence = internal::shared_frame_bus_sequence(state->bus_state);
        state->pending_damage_base_sequence = state->sequence;
        output.stop();
        output = WgcCapture(std::move(state));
        return make_result(WgcStatus::ok);
    } catch (...) {
        return exception_result();
    }
}

WgcResult WgcCapture::create_for_monitor_to_bus(
    HMONITOR monitor,
    SharedFrameBusPublisher& publisher,
    const WgcCaptureOptions& options,
    const WgcMailboxConfig& mailbox,
    WgcCapture& output) noexcept {
    try {
        if (!valid_capture_options(options)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "invalid WGC capture options");
        }
        if (!valid_mailbox_config(mailbox)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "invalid WGC mailbox configuration");
        }
        if (publisher.state_ == nullptr) {
            return make_result(
                WgcStatus::invalid_argument,
                E_UNEXPECTED,
                "shared frame bus publisher is uninitialized");
        }

        auto state = std::make_shared<WgcCaptureState>();
        state->options = options;
        state->epoch = options.capture_epoch;
        state->epoch_nonce = options.capture_epoch_nonce;
        state->mailbox_config = mailbox;
        state->target_monitor = monitor;
        state->bus_only = true;
        state->bus_state = publisher.state_;
        state->bus_config = internal::shared_frame_bus_config(state->bus_state);
        state->bus_planar_output = planar_bus_format(state->bus_config.format);
        state->bus_output_color_space = resolved_bus_color_space(
            state->bus_config, options.pixel_format);
        if (!state->bus_planar_output
            && state->bus_config.format
                != capture_dxgi_format(options.pixel_format)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "WGC direct bus format does not match the requested capture pixel format");
        }
        if (!state->bus_planar_output
            && state->bus_output_color_space
                != capture_color_space(options.pixel_format)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "WGC direct bus color space does not match the requested capture pixel format");
        }
        if (state->bus_planar_output
            && (((state->bus_config.width | state->bus_config.height) & 1u) != 0
                || (state->bus_config.bind_flags
                    & D3D11_BIND_RENDER_TARGET) == 0)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "planar WGC bus dimensions must be even and render-target bound");
        }

        WgcResult result = state->initialize_device(
            internal::shared_frame_bus_device(state->bus_state));
        if (!result) return result;
        result = state->initialize_item_for_monitor(monitor);
        if (!result) return result;
        if (!state->mailbox_dimensions_match_bus(
                mailbox,
                state->mailbox_source_width,
                state->mailbox_source_height)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "WGC mailbox output dimensions do not match the shared frame bus");
        }

        const GpuError reserved = internal::reserve_shared_frame_bus_producer(
            state->bus_state, state->bus_producer_token);
        if (!reserved) {
            return make_result(
                WgcStatus::invalid_state,
                FAILED(reserved.hresult) ? reserved.hresult : E_UNEXPECTED,
                reserved.what());
        }
        state->sequence = internal::shared_frame_bus_sequence(state->bus_state);
        state->pending_damage_base_sequence = state->sequence;
        output.stop();
        output = WgcCapture(std::move(state));
        return make_result(WgcStatus::ok);
    } catch (...) {
        return exception_result();
    }
}

WgcResult WgcCapture::start() noexcept {
    if (state_ == nullptr) {
        return make_result(WgcStatus::invalid_state, E_UNEXPECTED);
    }
    try {
        return state_->start();
    } catch (...) {
        return exception_result();
    }
}

void WgcCapture::stop() noexcept {
    if (state_ != nullptr) {
        state_->stop();
    }
}

WgcResult WgcCapture::acquire_latest(
    std::uint32_t timeout_ms,
    WgcFrameLease& output) noexcept {
    if (state_ == nullptr) {
        return make_result(WgcStatus::invalid_state, E_UNEXPECTED);
    }
    try {
        return state_->acquire_latest(timeout_ms, output);
    } catch (...) {
        return exception_result();
    }
}

WgcResult WgcCapture::set_mailbox_config(
    const WgcMailboxConfig& config) noexcept {
    if (state_ == nullptr) {
        return make_result(WgcStatus::invalid_state, E_UNEXPECTED);
    }
    try {
        return state_->set_mailbox_config(config);
    } catch (...) {
        return exception_result();
    }
}

WgcMailboxState WgcCapture::mailbox_state() const noexcept {
    return state_ != nullptr ? state_->read_mailbox_state() : WgcMailboxState{};
}

WgcResult WgcCapture::cursor_shape(
    std::uint64_t shape_sequence,
    WgcCursorShape& output) const noexcept {
    if (state_ == nullptr) {
        output = {};
        return make_result(WgcStatus::invalid_state, E_UNEXPECTED);
    }
    return state_->read_cursor_shape(shape_sequence, output);
}

ID3D11Device* WgcCapture::device() const noexcept {
    return state_ != nullptr ? state_->device.Get() : nullptr;
}

ID3D11DeviceContext* WgcCapture::context() const noexcept {
    return state_ != nullptr ? state_->context.Get() : nullptr;
}

bool WgcCapture::running() const noexcept {
    return state_ != nullptr && state_->running.load(std::memory_order_acquire);
}

bool WgcCapture::target_closed() const noexcept {
    return state_ != nullptr && state_->closed.load(std::memory_order_acquire);
}

WgcCaptureStats WgcCapture::stats() const noexcept {
    if (state_ == nullptr) {
        return {};
    }
    return {
        state_->received_frames.load(std::memory_order_relaxed),
        state_->published_frames.load(std::memory_order_relaxed),
        state_->overwritten_frames.load(std::memory_order_relaxed),
        state_->skipped_no_buffer.load(std::memory_order_relaxed),
        state_->dropped_at_source.load(std::memory_order_relaxed),
        state_->size_changes.load(std::memory_order_relaxed),
        state_->ingress_copy_submissions.load(std::memory_order_relaxed),
        state_->ingress_transform_submissions.load(std::memory_order_relaxed),
        state_->native_damage_frames.load(std::memory_order_relaxed),
        state_->full_damage_frames.load(std::memory_order_relaxed),
        state_->move_inference_submissions.load(std::memory_order_relaxed),
        state_->move_results_published.load(std::memory_order_relaxed),
        state_->move_inference_fail_closed.load(std::memory_order_relaxed),
        state_->move_inference_no_readback.load(std::memory_order_relaxed),
        state_->cursor_metadata_frames.load(std::memory_order_relaxed),
        state_->cursor_shape_updates.load(std::memory_order_relaxed),
        state_->recovery_attempts.load(std::memory_order_relaxed),
        state_->recovery_successes.load(std::memory_order_relaxed),
        state_->epoch,
        state_->epoch_nonce,
        {},
        0,
        0,
        0,
        0,
        state_->property_failure_mask_.load(std::memory_order_relaxed),
        state_->property_failure_count_.load(std::memory_order_relaxed)};
}

WgcResult WgcCapture::last_error() const noexcept {
    return state_ != nullptr
        ? state_->read_error()
        : make_result(WgcStatus::invalid_state, E_UNEXPECTED);
}

const char* wgc_status_string(WgcStatus status) noexcept {
    switch (status) {
    case WgcStatus::ok: return "ok";
    case WgcStatus::invalid_argument: return "invalid argument";
    case WgcStatus::invalid_state: return "invalid state";
    case WgcStatus::not_supported: return "not supported";
    case WgcStatus::timeout: return "timeout";
    case WgcStatus::target_closed: return "target closed";
    case WgcStatus::no_buffer: return "no buffer";
    case WgcStatus::out_of_memory: return "out of memory";
    case WgcStatus::d3d_error: return "D3D error";
    case WgcStatus::capture_error: return "capture error";
    case WgcStatus::region_unavailable: return "region unavailable";
    default: return "unknown WGC status";
    }
}

} // namespace fluxcap::gpu
