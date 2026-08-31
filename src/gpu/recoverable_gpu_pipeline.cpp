#include "recoverable_gpu_pipeline.hpp"

#include "desktop_duplication_capture.hpp"
#include "side_data_geometry.hpp"

#include <d3d11.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;
using internal::PipelineEpochSupervisor;
using internal::PipelineEpochTicket;
using internal::RecoverableGpuPipelineBuildStage;
using internal::RecoverableGpuPipelineTestFault;
using internal::RecoverableGpuPipelineTestFaultKind;
using internal::RecoverableGpuPipelineTestHook;
namespace capture = winrt::Windows::Graphics::Capture;

std::mutex test_hook_mutex;
RecoverableGpuPipelineTestHook installed_test_hook{};

RecoverableGpuPipelineTestHook test_hook_snapshot() noexcept {
    try {
        std::lock_guard lock(test_hook_mutex);
        return installed_test_hook;
    } catch (...) {
        return {};
    }
}

RecoverableGpuPipelineResult make_result(
    RecoverableGpuPipelineStatus status,
    HRESULT hresult = S_OK,
    std::string message = {}) {
    if (message.empty()) {
        message = recoverable_gpu_pipeline_status_string(status);
    }
    return {status, hresult, std::move(message)};
}

RecoverableGpuPipelineResult gpu_result(
    const GpuError& error,
    const char* stage) {
    RecoverableGpuPipelineStatus status =
        RecoverableGpuPipelineStatus::build_failed;
    switch (error.status) {
    case GpuStatus::invalid_argument:
        status = RecoverableGpuPipelineStatus::invalid_argument;
        break;
    case GpuStatus::unsupported:
        status = RecoverableGpuPipelineStatus::unsupported;
        break;
    case GpuStatus::out_of_memory:
        status = RecoverableGpuPipelineStatus::out_of_memory;
        break;
    case GpuStatus::device_lost:
        status = RecoverableGpuPipelineStatus::device_lost;
        break;
    case GpuStatus::system_error:
        status = RecoverableGpuPipelineStatus::system_error;
        break;
    case GpuStatus::timeout:
    case GpuStatus::ok:
    default:
        break;
    }
    std::string message = stage;
    message += ": ";
    message += error.what();
    return make_result(status, error.hresult, std::move(message));
}

RecoverableGpuPipelineResult wgc_result(
    const WgcResult& error,
    const char* stage) {
    RecoverableGpuPipelineStatus status =
        RecoverableGpuPipelineStatus::build_failed;
    switch (error.status) {
    case WgcStatus::invalid_argument:
        status = RecoverableGpuPipelineStatus::invalid_argument;
        break;
    case WgcStatus::not_supported:
        status = RecoverableGpuPipelineStatus::unsupported;
        break;
    case WgcStatus::target_closed:
        status = RecoverableGpuPipelineStatus::target_closed;
        break;
    case WgcStatus::out_of_memory:
        status = RecoverableGpuPipelineStatus::out_of_memory;
        break;
    case WgcStatus::d3d_error:
        if (error.hresult == DXGI_ERROR_DEVICE_REMOVED
            || error.hresult == DXGI_ERROR_DEVICE_RESET
            || error.hresult == DXGI_ERROR_DEVICE_HUNG) {
            status = RecoverableGpuPipelineStatus::device_lost;
        }
        break;
    case WgcStatus::ok:
    case WgcStatus::invalid_state:
    case WgcStatus::timeout:
    case WgcStatus::no_buffer:
    case WgcStatus::capture_error:
    case WgcStatus::region_unavailable:
    default:
        break;
    }
    std::string message = stage;
    message += ": ";
    message += error.message;
    return make_result(status, error.hresult, std::move(message));
}

RecoverableGpuPipelineResult async_result(
    const AsyncGpuPipelineResult& error,
    const char* stage) {
    RecoverableGpuPipelineStatus status =
        RecoverableGpuPipelineStatus::build_failed;
    switch (error.status) {
    case AsyncGpuPipelineStatus::invalid_argument:
        status = RecoverableGpuPipelineStatus::invalid_argument;
        break;
    case AsyncGpuPipelineStatus::device_lost:
        status = RecoverableGpuPipelineStatus::device_lost;
        break;
    case AsyncGpuPipelineStatus::out_of_memory:
        status = RecoverableGpuPipelineStatus::out_of_memory;
        break;
    case AsyncGpuPipelineStatus::system_error:
        status = RecoverableGpuPipelineStatus::system_error;
        break;
    case AsyncGpuPipelineStatus::ok:
    case AsyncGpuPipelineStatus::invalid_state:
    case AsyncGpuPipelineStatus::no_buffer:
    case AsyncGpuPipelineStatus::timeout:
    case AsyncGpuPipelineStatus::transform_error:
    case AsyncGpuPipelineStatus::encoder_error:
    default:
        break;
    }
    std::string message = stage;
    message += ": ";
    message += error.message;
    return make_result(status, error.hresult, std::move(message));
}

RecoverableGpuPipelineResult encoder_probe_result(
    const GpuEncoderResult& error) {
    RecoverableGpuPipelineStatus status =
        RecoverableGpuPipelineStatus::system_error;
    switch (error.status) {
    case GpuEncoderStatus::invalid_argument:
        status = RecoverableGpuPipelineStatus::invalid_argument;
        break;
    case GpuEncoderStatus::not_supported:
    case GpuEncoderStatus::unsupported_input_format:
        status = RecoverableGpuPipelineStatus::unsupported;
        break;
    case GpuEncoderStatus::out_of_memory:
        status = RecoverableGpuPipelineStatus::out_of_memory;
        break;
    case GpuEncoderStatus::device_lost:
        status = RecoverableGpuPipelineStatus::device_lost;
        break;
    case GpuEncoderStatus::ok:
    case GpuEncoderStatus::invalid_state:
    case GpuEncoderStatus::device_mismatch:
    case GpuEncoderStatus::timeout:
    case GpuEncoderStatus::callback_failed:
    case GpuEncoderStatus::media_foundation_error:
    case GpuEncoderStatus::d3d_error:
    default:
        break;
    }
    std::string message = "automatic planar encoder capability probe failed";
    if (!error.message.empty()) {
        message += ": ";
        message += error.message;
    }
    return make_result(status, error.hresult, std::move(message));
}

bool infrastructure_failure_hresult(HRESULT hresult) noexcept {
    return hresult == DXGI_ERROR_ACCESS_LOST
        || hresult == DXGI_ERROR_DEVICE_REMOVED
        || hresult == DXGI_ERROR_DEVICE_RESET
        || hresult == DXGI_ERROR_DEVICE_HUNG
        || hresult == E_OUTOFMEMORY;
}

bool device_failure_hresult(HRESULT hresult) noexcept {
    return hresult == DXGI_ERROR_DEVICE_REMOVED
        || hresult == DXGI_ERROR_DEVICE_RESET
        || hresult == DXGI_ERROR_DEVICE_HUNG;
}

bool automatic_planar_fallback_error(
    const RecoverableGpuPipelineResult& error) noexcept {
    if (infrastructure_failure_hresult(error.hresult)) {
        return false;
    }
    return error.status == RecoverableGpuPipelineStatus::unsupported
        || error.status == RecoverableGpuPipelineStatus::build_failed;
}

bool retryable_initial_build_error(
    const RecoverableGpuPipelineResult& error) noexcept {
    return error.status == RecoverableGpuPipelineStatus::build_failed
        || error.status == RecoverableGpuPipelineStatus::device_lost
        || error.status == RecoverableGpuPipelineStatus::out_of_memory
        || error.status == RecoverableGpuPipelineStatus::system_error;
}

bool valid_mailbox(const WgcMailboxConfig& mailbox) noexcept {
    switch (mailbox.mode) {
    case WgcMailboxMode::full_frame:
        return mailbox.x == 0 && mailbox.y == 0
            && mailbox.width == 0 && mailbox.height == 0;
    case WgcMailboxMode::absolute_region:
        return mailbox.width != 0 && mailbox.height != 0;
    case WgcMailboxMode::centered_region:
        return mailbox.x == 0 && mailbox.y == 0
            && mailbox.width != 0 && mailbox.height != 0;
    default:
        return false;
    }
}

DXGI_FORMAT capture_format(WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_B8G8R8A8_UNORM;
}

DXGI_COLOR_SPACE_TYPE capture_color_space(WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

DXGI_FORMAT transform_output_format(GpuPixelFormat format) noexcept {
    switch (format) {
    case GpuPixelFormat::bgra8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case GpuPixelFormat::nv12: return DXGI_FORMAT_NV12;
    case GpuPixelFormat::p010: return DXGI_FORMAT_P010;
    case GpuPixelFormat::rgba16_float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_COLOR_SPACE_TYPE resolved_transform_output_color_space(
    const GpuTransformConfig& config,
    WgcPixelFormat capture_pixel_format) noexcept {
    if (config.output_color_space != DXGI_COLOR_SPACE_CUSTOM) {
        return config.output_color_space;
    }
    switch (config.output_format) {
    case GpuPixelFormat::bgra8:
        return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    case GpuPixelFormat::rgba16_float:
        return capture_color_space(capture_pixel_format);
    case GpuPixelFormat::p010:
        if (capture_pixel_format == WgcPixelFormat::rgba16_float) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        }
        [[fallthrough]];
    case GpuPixelFormat::nv12:
        return config.full_range_yuv
            ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
            : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    default:
        return DXGI_COLOR_SPACE_CUSTOM;
    }
}

bool fixed_mailbox_matches_input(
    const WgcMailboxConfig& mailbox,
    const GpuTransformConfig& transform) noexcept {
    return mailbox.mode == WgcMailboxMode::full_frame
        || (mailbox.width == transform.input_width
            && mailbox.height == transform.input_height);
}

bool planar_dimensions_eligible(
    const RecoverableGpuPipelineConfig& config,
    std::uint32_t input_width,
    std::uint32_t input_height) noexcept {
    const auto& transform = config.pipeline.transform;
    const auto& encoder = config.pipeline.encoder;
    internal::SideDataGeometry side_data_geometry;
    side_data_geometry.source_width = input_width;
    side_data_geometry.source_height = input_height;
    side_data_geometry.output_width = transform.output_width;
    side_data_geometry.output_height = transform.output_height;
    side_data_geometry.planar_420 = true;
    return (encoder.input_format == DXGI_FORMAT_NV12
            || encoder.input_format == DXGI_FORMAT_P010)
        && transform.output_width == encoder.width
        && transform.output_height == encoder.height
        && ((transform.output_width | transform.output_height) & 1u) == 0
        && internal::valid_side_data_geometry(side_data_geometry)
        && (!config.capture.include_cursor_metadata
            || internal::phase_invariant_cursor_geometry(
                side_data_geometry));
}

RecoverableGpuPipelineResult normalize_config(
    const RecoverableGpuPipelineConfig& input,
    EncodedPacketCallback callback,
    RecoverableGpuPipelineConfig& output,
    DXGI_FORMAT& bus_format) {
    output = input;
    const auto& transform = input.pipeline.transform;
    const auto& encoder = input.pipeline.encoder;
    const auto& retry = input.retry;
    const DXGI_COLOR_SPACE_TYPE output_color_space =
        resolved_transform_output_color_space(
            transform, input.capture.pixel_format);
    const DXGI_COLOR_SPACE_TYPE encoder_color_space =
        encoder.input_color_space == DXGI_COLOR_SPACE_CUSTOM
        ? output_color_space
        : encoder.input_color_space;
    const bool valid_monitor_backend =
        input.monitor_backend
            == RecoverableMonitorCaptureBackend::automatic
        || input.monitor_backend
            == RecoverableMonitorCaptureBackend::windows_graphics_capture
        || input.monitor_backend
            == RecoverableMonitorCaptureBackend::desktop_duplication;
    const bool fixed_region = transform.input_region_x != 0
        || transform.input_region_y != 0
        || transform.input_region_width != 0
        || transform.input_region_height != 0;
    if (callback == nullptr
        || !valid_monitor_backend
        || !valid_mailbox(input.mailbox)
        || input.capture.buffer_count < 2
        || input.capture.buffer_count > 16
        || (input.capture.pixel_format != WgcPixelFormat::bgra8
            && input.capture.pixel_format != WgcPixelFormat::rgba16_float)
        || (input.capture.damage_mode != WgcDamageMode::disabled
            && input.capture.damage_mode
                != WgcDamageMode::native_report_only
            && input.capture.damage_mode
                != WgcDamageMode::native_with_inferred_moves)
        || input.capture.cursor_shape_refresh_interval_ms > 10'000
        || (transform.backend != GpuTransformBackend::automatic
            && transform.backend != GpuTransformBackend::video_processor
            && transform.backend
                != GpuTransformBackend::deterministic_planar)
        || input.bus_slot_count < 2
        || input.bus_slot_count > shared_frame_bus_max_slots
        || transform.input_width == 0
        || transform.input_height == 0
        || transform.output_width == 0
        || transform.output_height == 0
        || transform.frame_rate_numerator == 0
        || transform.frame_rate_denominator == 0
        || encoder.width != transform.output_width
        || encoder.height != transform.output_height
        || encoder.frame_rate_numerator == 0
        || encoder.frame_rate_denominator == 0
        || transform_output_format(transform.output_format)
            == DXGI_FORMAT_UNKNOWN
        || transform_output_format(transform.output_format)
            != encoder.input_format
        || output_color_space == DXGI_COLOR_SPACE_CUSTOM
        || output_color_space == DXGI_COLOR_SPACE_RESERVED
        || encoder_color_space != output_color_space
        || input.pipeline.queue_depth < 2
        || input.pipeline.queue_depth > 32
        || !fixed_mailbox_matches_input(input.mailbox, transform)
        || fixed_region
        || retry.maximum_backoff_ms < retry.initial_backoff_ms
        || retry.backoff_multiplier == 0
        || retry.monitor_interval_ms == 0
        || retry.monitor_interval_ms > 60'000
        || retry.max_retry_count > 10'000) {
        return make_result(
            RecoverableGpuPipelineStatus::invalid_argument,
            E_INVALIDARG,
            "invalid recoverable GPU pipeline configuration");
    }

    const bool planar_eligible = planar_dimensions_eligible(
        input, transform.input_width, transform.input_height);
    bool use_planar = false;
    switch (input.bus_mode) {
    case RecoverableGpuPipelineBusMode::automatic:
        use_planar = planar_eligible;
        break;
    case RecoverableGpuPipelineBusMode::capture_native:
        break;
    case RecoverableGpuPipelineBusMode::planar_encoder_input:
        if (!planar_eligible) {
                return make_result(
                    RecoverableGpuPipelineStatus::invalid_argument,
                    E_INVALIDARG,
                    "planar bus mode requires an even encoder-sized NV12/P010 output contract");
        }
        use_planar = true;
        break;
    default:
        return make_result(
            RecoverableGpuPipelineStatus::invalid_argument,
            E_INVALIDARG,
            "unknown recoverable GPU pipeline bus mode");
    }

    bus_format = use_planar
        ? encoder.input_format
        : capture_format(input.capture.pixel_format);
    auto& normalized_transform = output.pipeline.transform;
    auto& normalized_encoder = output.pipeline.encoder;
    normalized_transform.output_color_space = output_color_space;
    normalized_encoder.input_color_space = encoder_color_space;
    normalized_transform.input_format = bus_format;
    normalized_transform.input_region_x = 0;
    normalized_transform.input_region_y = 0;
    normalized_transform.input_region_width = 0;
    normalized_transform.input_region_height = 0;
    normalized_transform.input_color_space = use_planar
        ? encoder_color_space
        : capture_color_space(input.capture.pixel_format);
    return make_result(RecoverableGpuPipelineStatus::ok);
}

HRESULT create_fresh_device(ComPtr<ID3D11Device>& output) noexcept {
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    ComPtr<ID3D11DeviceContext> context;
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
        | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &output,
        &level,
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
            &output,
            &level,
            &context);
    }
    return hr;
}

HRESULT create_fresh_device(
    IDXGIAdapter1* adapter,
    ComPtr<ID3D11Device>& output) noexcept {
    if (adapter == nullptr) return E_POINTER;
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    ComPtr<ID3D11DeviceContext> context;
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
        | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &output,
        &level,
        &context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &output,
            &level,
            &context);
    }
    return hr;
}

void close_local_registration_handles(
    SharedFrameBusRegistration& registration) noexcept {
    const auto close_encoded = [](std::uint64_t& encoded) noexcept {
        if (encoded != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(
                static_cast<std::uintptr_t>(encoded)));
            encoded = 0;
        }
    };
    close_encoded(registration.control_mapping_handle);
    close_encoded(registration.publisher_process_handle);
    close_encoded(registration.ready_fence_handle);
    close_encoded(registration.done_fence_handle);
}

struct LocalRegistrationHandleGuard final {
    SharedFrameBusRegistration* registration = nullptr;

    ~LocalRegistrationHandleGuard() {
        if (registration != nullptr) {
            close_local_registration_handles(*registration);
        }
    }

    void release() noexcept { registration = nullptr; }
};

} // namespace

namespace internal {

void set_recoverable_gpu_pipeline_test_hook(
    const RecoverableGpuPipelineTestHook* hook) noexcept {
    try {
        std::lock_guard lock(test_hook_mutex);
        installed_test_hook = hook != nullptr
            ? *hook
            : RecoverableGpuPipelineTestHook{};
    } catch (...) {
    }
}

} // namespace internal

class RecoverableGpuPipeline::Impl final
    : public std::enable_shared_from_this<RecoverableGpuPipeline::Impl> {
public:
    enum class TargetKind : std::uint8_t { window, monitor };

    struct PacketBridge final {
        Impl* owner = nullptr;
        EncodedPacketCallback callback = nullptr;
        void* context = nullptr;
        std::atomic<bool> enabled{false};

        static void invoke(void* opaque, const EncodedPacket& packet) noexcept {
            auto& bridge = *static_cast<PacketBridge*>(opaque);
            if (!bridge.enabled.load(std::memory_order_acquire)
                || bridge.callback == nullptr) {
                return;
            }
            Impl* previous = active_callback_owner_;
            active_callback_owner_ = bridge.owner;
            try {
                bridge.callback(bridge.context, packet);
            } catch (...) {
            }
            active_callback_owner_ = previous;
        }
    };

    struct EpochGraph final {
        EpochGraph(
            Impl* owner,
            PipelineEpochTicket epoch_ticket,
            DXGI_FORMAT format,
            DXGI_COLOR_SPACE_TYPE color_space,
            std::uint32_t width,
            std::uint32_t height,
            std::uint32_t output_width,
            std::uint32_t output_height,
            bool planar) noexcept
            : ticket(epoch_ticket),
              bus_format(format),
              bus_color_space(color_space),
              source_width(width),
              source_height(height),
              bus_width(output_width),
              bus_height(output_height),
              planar_bus(planar) {
            callback.owner = owner;
            callback.callback = owner->callback_;
            callback.context = owner->callback_context_;
        }

        ~EpochGraph() { teardown(); }

        WgcResult start_capture() {
            return desktop_duplication
                ? duplication_capture.start()
                : capture.start();
        }

        void stop_capture() noexcept {
            if (desktop_duplication) duplication_capture.stop();
            else capture.stop();
        }

        bool capture_is_running() const noexcept {
            return desktop_duplication
                ? duplication_capture.running()
                : capture.running();
        }

        bool capture_target_closed() const noexcept {
            return desktop_duplication
                ? duplication_capture.target_closed()
                : capture.target_closed();
        }

        WgcResult capture_error() const noexcept {
            return desktop_duplication
                ? duplication_capture.last_error()
                : capture.last_error();
        }

        WgcMailboxState capture_mailbox() const noexcept {
            return desktop_duplication
                ? duplication_capture.mailbox_state()
                : capture.mailbox_state();
        }

        WgcCaptureStats capture_statistics() const noexcept {
            return desktop_duplication
                ? duplication_capture.stats()
                : capture.stats();
        }

        void teardown() noexcept {
            if (torn_down) return;
            torn_down = true;
            callback.enabled.store(false, std::memory_order_release);
            stop_capture();
            pipeline.close();
            capture = WgcCapture{};
            duplication_capture = DesktopDuplicationCapture{};
            publisher = SharedFrameBusPublisher{};
            device.Reset();
            started = false;
            if (hook_teardown_armed
                && hook.teardown_epoch != nullptr) {
                hook.teardown_epoch(hook.context, ticket);
            }
        }

        PacketBridge callback{};
        ComPtr<ID3D11Device> device;
        SharedFrameBusPublisher publisher;
        WgcCapture capture;
        DesktopDuplicationCapture duplication_capture;
        AsyncGpuPipeline pipeline;
        PipelineEpochTicket ticket{};
        DXGI_FORMAT bus_format = DXGI_FORMAT_UNKNOWN;
        DXGI_COLOR_SPACE_TYPE bus_color_space = DXGI_COLOR_SPACE_CUSTOM;
        std::uint32_t source_width = 0;
        std::uint32_t source_height = 0;
        std::uint32_t bus_width = 0;
        std::uint32_t bus_height = 0;
        RecoverableGpuPipelineTestHook hook{};
        bool planar_bus = false;
        bool desktop_duplication = false;
        bool desktop_duplication_candidate = false;
        bool test_only = false;
        bool started = false;
        bool hook_teardown_armed = false;
        bool torn_down = false;
    };

    Impl(
        TargetKind target_kind,
        HWND window,
        HMONITOR monitor,
        RecoverableGpuPipelineConfig config,
        EncodedPacketCallback callback,
        void* callback_context) noexcept
        : target_kind_(target_kind),
          window_(window),
          monitor_(monitor),
          config_(std::move(config)),
          callback_(callback),
          callback_context_(callback_context),
          hook_(test_hook_snapshot()) {}

    ~Impl() { stop(); }

    RecoverableGpuPipelineResult start() {
        std::unique_lock lifecycle_lock(lifecycle_mutex_);
        if (started_once_) {
            return make_result(
                RecoverableGpuPipelineStatus::invalid_state,
                E_UNEXPECTED,
                "recoverable GPU pipeline instances can only be started once");
        }
        started_once_ = true;
        stop_requested_.store(false, std::memory_order_release);
        {
            std::lock_guard state_lock(state_mutex_);
            state_ = RecoverableGpuPipelineState::starting;
            start_complete_ = false;
            start_result_ = make_result(RecoverableGpuPipelineStatus::ok);
            last_error_ = start_result_;
        }

        try {
            auto self = shared_from_this();
            control_thread_ = std::thread(
                [self = std::move(self)] { self->control_main(); });
        } catch (const std::bad_alloc&) {
            const auto error = make_result(
                RecoverableGpuPipelineStatus::out_of_memory,
                E_OUTOFMEMORY,
                "recoverable control thread allocation failed");
            fail_start(error);
            return error;
        } catch (...) {
            const auto error = make_result(
                RecoverableGpuPipelineStatus::system_error,
                E_FAIL,
                "recoverable control thread creation failed");
            fail_start(error);
            return error;
        }
        lifecycle_lock.unlock();

        std::unique_lock state_lock(state_mutex_);
        state_cv_.wait(state_lock, [this] { return start_complete_; });
        const RecoverableGpuPipelineResult result = start_result_;
        state_lock.unlock();
        if (!result) join_control_thread(false);
        return result;
    }

    void stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        wait_cv_.notify_all();
        {
            std::lock_guard lock(state_mutex_);
            if (state_ != RecoverableGpuPipelineState::stopped
                && state_ != RecoverableGpuPipelineState::failed) {
                state_ = RecoverableGpuPipelineState::stopping;
            }
        }
        if (active_callback_owner_ == this) return;
        join_control_thread(true);
    }

    RecoverableGpuPipelineSnapshot snapshot() const noexcept {
        RecoverableGpuPipelineSnapshot output;
        try {
            {
                std::lock_guard lock(state_mutex_);
                output.state = state_;
            }
            const auto epoch = supervisor_.snapshot();
            output.epoch = epoch.ticket.epoch;
            output.epoch_nonce = epoch.ticket.nonce;
            output.recovery_attempts = epoch.recovery_attempts;
            output.recovery_successes = epoch.recovery_successes;
            output.first_recovered_frame_pending = epoch.first_frame_pending;
            output.rebuild_attempts =
                rebuild_attempts_.load(std::memory_order_relaxed);
            output.rebuild_failures =
                rebuild_failures_.load(std::memory_order_relaxed);
            output.consecutive_retry_count =
                consecutive_retry_count_.load(std::memory_order_relaxed);
            output.automatic_planar_fallbacks =
                automatic_planar_fallbacks_.load(std::memory_order_relaxed);
            output.automatic_planar_disabled =
                automatic_planar_disabled_.load(std::memory_order_relaxed);
            output.last_device_removed_reason =
                last_device_removed_reason_.load(std::memory_order_relaxed);
            std::lock_guard graph_lock(graph_mutex_);
            if (graph_ != nullptr) {
                output.active_bus_format = graph_->bus_format;
                output.active_bus_color_space = graph_->bus_color_space;
                output.active_external_encoder_input = graph_->planar_bus;
                output.active_monitor_backend = graph_->desktop_duplication
                    ? RecoverableMonitorCaptureBackend::desktop_duplication
                    : RecoverableMonitorCaptureBackend::windows_graphics_capture;
                output.active_source_width = graph_->source_width;
                output.active_source_height = graph_->source_height;
                output.active_bus_width = graph_->bus_width;
                output.active_bus_height = graph_->bus_height;
                output.capture_running = graph_->test_only
                    ? graph_->started
                    : graph_->capture_is_running();
                output.encoder_accepting = graph_->test_only
                    ? graph_->started
                    : graph_->pipeline.accepting();
                if (!graph_->test_only) {
                    output.capture = graph_->capture_statistics();
                    output.bus = graph_->publisher.stats();
                    output.pipeline = graph_->pipeline.stats();
                }
            }
        } catch (...) {
        }
        return output;
    }

    RecoverableGpuPipelineResult last_error() const noexcept {
        try {
            std::lock_guard lock(state_mutex_);
            return last_error_;
        } catch (...) {
            return make_result(
                RecoverableGpuPipelineStatus::system_error,
                E_FAIL,
                "recoverable pipeline error state is unavailable");
        }
    }

    bool running() const noexcept {
        try {
            std::lock_guard lock(state_mutex_);
            return state_ == RecoverableGpuPipelineState::running;
        } catch (...) {
            return false;
        }
    }

private:
    struct DetectedFault final {
        RecoverableGpuPipelineTestFaultKind kind =
            RecoverableGpuPipelineTestFaultKind::none;
        HRESULT hresult = S_OK;
        std::string message;
        bool disable_automatic_planar = false;
    };

    bool desktop_duplication_options_supported() const noexcept {
        return !config_.capture.require_border
            && !config_.capture.include_secondary_windows
            && config_.capture.min_update_interval_us == 0
            && !config_.capture.include_cursor;
    }

    bool desktop_duplication_requested() const noexcept {
        if (target_kind_ != TargetKind::monitor) return false;
        switch (config_.monitor_backend) {
        case RecoverableMonitorCaptureBackend::desktop_duplication:
            return true;
        case RecoverableMonitorCaptureBackend::automatic:
            return desktop_duplication_options_supported();
        case RecoverableMonitorCaptureBackend::windows_graphics_capture:
        default:
            return false;
        }
    }

    RecoverableGpuPipelineResult before_stage(
        RecoverableGpuPipelineBuildStage stage,
        PipelineEpochTicket ticket,
        DXGI_FORMAT bus_format) {
        if (hook_.before_build_stage == nullptr) {
            return {};
        }
        return hook_.before_build_stage(
            hook_.context, stage, ticket, bus_format);
    }

    RecoverableGpuPipelineResult query_epoch_dimensions(
        PipelineEpochTicket ticket,
        std::uint32_t& width,
        std::uint32_t& height) {
        if (config_.mailbox.mode != WgcMailboxMode::full_frame) {
            width = config_.mailbox.width;
            height = config_.mailbox.height;
            return make_result(RecoverableGpuPipelineStatus::ok);
        }
        if (hook_.query_source_dimensions != nullptr) {
            if (!hook_.query_source_dimensions(
                    hook_.context, ticket, width, height)
                || width == 0 || height == 0) {
                return make_result(
                    RecoverableGpuPipelineStatus::build_failed,
                    HRESULT_FROM_WIN32(ERROR_RETRY),
                    "capture target dimensions are temporarily unavailable");
            }
            return make_result(RecoverableGpuPipelineStatus::ok);
        }

        if (desktop_duplication_requested()) {
            DXGI_OUTPUT_DESC description{};
            const HRESULT found = internal::find_monitor_output(
                monitor_, nullptr, nullptr, &description);
            if (SUCCEEDED(found)) {
                const LONG output_width = description.DesktopCoordinates.right
                    - description.DesktopCoordinates.left;
                const LONG output_height = description.DesktopCoordinates.bottom
                    - description.DesktopCoordinates.top;
                if (output_width > 0 && output_height > 0) {
                    width = static_cast<std::uint32_t>(output_width);
                    height = static_cast<std::uint32_t>(output_height);
                    return make_result(RecoverableGpuPipelineStatus::ok);
                }
            }
            if (config_.monitor_backend
                == RecoverableMonitorCaptureBackend::desktop_duplication) {
                return make_result(
                    RecoverableGpuPipelineStatus::unsupported,
                    FAILED(found) ? found : E_BOUNDS,
                    "monitor is not available through Desktop Duplication");
            }
        }

        try {
            auto interop = winrt::get_activation_factory<
                capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            capture::GraphicsCaptureItem item{nullptr};
            HRESULT hr = target_kind_ == TargetKind::window
                ? interop->CreateForWindow(
                    window_,
                    winrt::guid_of<ABI::Windows::Graphics::Capture::
                        IGraphicsCaptureItem>(),
                    winrt::put_abi(item))
                : interop->CreateForMonitor(
                    monitor_,
                    winrt::guid_of<ABI::Windows::Graphics::Capture::
                        IGraphicsCaptureItem>(),
                    winrt::put_abi(item));
            if (FAILED(hr)) {
                return make_result(
                    RecoverableGpuPipelineStatus::build_failed,
                    hr,
                    "capture target size query failed");
            }
            const auto size = item.Size();
            if (size.Width <= 0 || size.Height <= 0) {
                return make_result(
                    RecoverableGpuPipelineStatus::build_failed,
                    HRESULT_FROM_WIN32(ERROR_RETRY),
                    "capture target has empty dimensions");
            }
            width = static_cast<std::uint32_t>(size.Width);
            height = static_cast<std::uint32_t>(size.Height);
            return make_result(RecoverableGpuPipelineStatus::ok);
        } catch (...) {
            return make_result(
                RecoverableGpuPipelineStatus::build_failed,
                winrt::to_hresult(),
                "capture target size query raised a WinRT exception");
        }
    }

    DXGI_FORMAT epoch_bus_format(bool planar) const noexcept {
        return planar
            ? config_.pipeline.encoder.input_format
            : capture_format(config_.capture.pixel_format);
    }

    AsyncGpuPipelineConfig epoch_pipeline_config(
        std::uint32_t width,
        std::uint32_t height,
        bool planar) const {
        AsyncGpuPipelineConfig output = config_.pipeline;
        output.transform.input_width = planar
            ? output.transform.output_width
            : width;
        output.transform.input_height = planar
            ? output.transform.output_height
            : height;
        output.transform.input_format = epoch_bus_format(planar);
        output.transform.input_region_x = 0;
        output.transform.input_region_y = 0;
        output.transform.input_region_width = 0;
        output.transform.input_region_height = 0;
        output.transform.input_color_space = planar
            ? resolved_transform_output_color_space(
                output.transform, config_.capture.pixel_format)
            : capture_color_space(config_.capture.pixel_format);
        if (planar) {
            output.encoder.require_video_encoder_input_bind = true;
        }
        return output;
    }

    void disable_automatic_planar() noexcept {
        if (config_.bus_mode != RecoverableGpuPipelineBusMode::automatic) {
            return;
        }
        bool expected = false;
        if (automatic_planar_disabled_.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
            automatic_planar_fallbacks_.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    bool disable_planar_after_graph_failure(
        const EpochGraph* graph,
        const RecoverableGpuPipelineResult& error) noexcept {
        if (graph != nullptr && graph->planar_bus
            && config_.bus_mode
                == RecoverableGpuPipelineBusMode::automatic
            && automatic_planar_fallback_error(error)) {
            disable_automatic_planar();
            return true;
        }
        return false;
    }

    RecoverableGpuPipelineResult probe_planar_topology(
        EpochGraph& graph,
        std::uint32_t width,
        std::uint32_t height,
        bool& supported) {
        supported = false;
        if (hook_.planar_topology_supported != nullptr) {
            supported = hook_.planar_topology_supported(
                hook_.context, graph.ticket, width, height);
            return make_result(RecoverableGpuPipelineStatus::ok);
        }
        if (graph.test_only) {
            supported = true;
            return make_result(RecoverableGpuPipelineStatus::ok);
        }
        GpuEncoderConfig encoder_config = config_.pipeline.encoder;
        encoder_config.require_video_encoder_input_bind = true;
        GpuEncoderSupport support;
        const GpuEncoderResult probed = GpuEncoder::probe(
            graph.device.Get(), encoder_config, support);
        if (!probed) return encoder_probe_result(probed);
        supported = support.supported
            && support.external_planar_input
            && support.video_encoder_input_bind_supported;
        return make_result(RecoverableGpuPipelineStatus::ok);
    }

    RecoverableGpuPipelineResult build_epoch_candidate(
        PipelineEpochTicket ticket,
        std::uint32_t width,
        std::uint32_t height,
        bool planar,
        std::unique_ptr<EpochGraph>& output,
        bool& fallback_allowed) {
        fallback_allowed = false;
        const DXGI_FORMAT bus_format = epoch_bus_format(planar);
        const AsyncGpuPipelineConfig pipeline_config =
            epoch_pipeline_config(width, height, planar);
        const DXGI_COLOR_SPACE_TYPE bus_color_space =
            pipeline_config.transform.input_color_space;
        const std::uint32_t bus_width =
            pipeline_config.transform.input_width;
        const std::uint32_t bus_height =
            pipeline_config.transform.input_height;
        auto graph = std::make_unique<EpochGraph>(
            this,
            ticket,
            bus_format,
            bus_color_space,
            width,
            height,
            bus_width,
            bus_height,
            planar);
        graph->hook = hook_;
        graph->test_only = hook_.bypass_hardware;
        graph->desktop_duplication_candidate =
            desktop_duplication_requested();

        const auto run_stage = [&](RecoverableGpuPipelineBuildStage stage) {
            return before_stage(stage, ticket, bus_format);
        };

        RecoverableGpuPipelineResult injected = run_stage(
            RecoverableGpuPipelineBuildStage::device);
        if (!injected) return injected;
        if (!graph->test_only) {
            HRESULT hr = E_FAIL;
            if (graph->desktop_duplication_candidate) {
                ComPtr<IDXGIAdapter1> adapter;
                const HRESULT found = internal::find_monitor_output(
                    monitor_, &adapter, nullptr, nullptr);
                if (SUCCEEDED(found)) {
                    hr = create_fresh_device(adapter.Get(), graph->device);
                } else {
                    hr = found;
                }
                if (FAILED(hr)
                    && config_.monitor_backend
                        == RecoverableMonitorCaptureBackend::automatic) {
                    graph->desktop_duplication_candidate = false;
                    graph->device.Reset();
                    hr = create_fresh_device(graph->device);
                }
            } else {
                hr = create_fresh_device(graph->device);
            }
            if (FAILED(hr)) {
                return make_result(
                    RecoverableGpuPipelineStatus::build_failed,
                    hr,
                    "fresh D3D11 hardware device creation failed");
            }
        }
        if (planar
            && config_.bus_mode
                == RecoverableGpuPipelineBusMode::automatic) {
            fallback_allowed = true;
            bool supported = false;
            const RecoverableGpuPipelineResult probed =
                probe_planar_topology(
                    *graph, bus_width, bus_height, supported);
            if (!probed) return probed;
            if (!supported) {
                return make_result(
                    RecoverableGpuPipelineStatus::unsupported,
                    E_NOINTERFACE,
                    "automatic planar topology is not supported by the active device and encoder");
            }
        }
        if (graph->test_only) {
            constexpr RecoverableGpuPipelineBuildStage stages[] = {
                RecoverableGpuPipelineBuildStage::shared_bus,
                RecoverableGpuPipelineBuildStage::consumer_registration,
                RecoverableGpuPipelineBuildStage::consumer_open,
                RecoverableGpuPipelineBuildStage::async_pipeline,
                RecoverableGpuPipelineBuildStage::capture};
            for (const auto stage : stages) {
                injected = run_stage(stage);
                if (!injected) return injected;
            }
            output = std::move(graph);
            return make_result(RecoverableGpuPipelineStatus::ok);
        }

        injected = run_stage(RecoverableGpuPipelineBuildStage::shared_bus);
        if (!injected) return injected;
        SharedFrameBusConfig bus_config;
        bus_config.width = bus_width;
        bus_config.height = bus_height;
        bus_config.format = bus_format;
        bus_config.color_space = bus_color_space;
        if (planar) {
            bus_config.bind_flags = D3D11_BIND_RENDER_TARGET
                | D3D11_BIND_VIDEO_ENCODER;
        }
        bus_config.slot_count = config_.bus_slot_count;
        const GpuError bus_created = SharedFrameBusPublisher::create(
            graph->device.Get(), bus_config, graph->publisher);
        if (!bus_created) return gpu_result(bus_created, "shared bus create");

        injected = run_stage(
            RecoverableGpuPipelineBuildStage::consumer_registration);
        if (!injected) return injected;
        SharedFrameBusRegistration registration;
        const GpuError registered = graph->publisher.register_consumer(
            GetCurrentProcess(), registration);
        if (!registered) {
            return gpu_result(registered, "local bus consumer registration");
        }
        LocalRegistrationHandleGuard registration_handles{&registration};

        injected = run_stage(RecoverableGpuPipelineBuildStage::consumer_open);
        if (!injected) return injected;
        SharedFrameBusConsumer consumer;
        registration_handles.release();
        const GpuError opened = SharedFrameBusConsumer::open(
            graph->device.Get(), registration, true, consumer);
        if (!opened) return gpu_result(opened, "local bus consumer open");

        injected = run_stage(
            RecoverableGpuPipelineBuildStage::async_pipeline);
        if (!injected) return injected;
        const AsyncGpuPipelineResult pipeline_created =
            AsyncGpuPipeline::create_from_shared_bus(
                graph->device.Get(),
                std::move(consumer),
                pipeline_config,
                &PacketBridge::invoke,
                &graph->callback,
                graph->pipeline);
        if (!pipeline_created) {
            return async_result(pipeline_created, "async transform/encoder create");
        }

        injected = run_stage(RecoverableGpuPipelineBuildStage::capture);
        if (!injected) return injected;
        WgcCaptureOptions capture_options = config_.capture;
        capture_options.capture_epoch = ticket.epoch;
        capture_options.capture_epoch_nonce = ticket.nonce;
        capture_options.planar_transform_backend =
            config_.pipeline.transform.backend;
        WgcResult capture_created;
        if (target_kind_ == TargetKind::window) {
            capture_created = WgcCapture::create_for_window_to_bus(
                window_, graph->publisher, capture_options,
                config_.mailbox, graph->capture);
        } else {
            if (graph->desktop_duplication_candidate) {
                capture_created =
                    DesktopDuplicationCapture::create_for_monitor_to_bus(
                        monitor_, graph->publisher, capture_options,
                        config_.mailbox, graph->duplication_capture);
                if (capture_created) {
                    graph->desktop_duplication = true;
                } else if (config_.monitor_backend
                        != RecoverableMonitorCaptureBackend::automatic
                    || capture_created.status != WgcStatus::not_supported) {
                    return wgc_result(
                        capture_created,
                        "Desktop Duplication direct-bus capture create");
                }
            }
            if (!graph->desktop_duplication) {
                capture_created = WgcCapture::create_for_monitor_to_bus(
                    monitor_, graph->publisher, capture_options,
                    config_.mailbox, graph->capture);
            }
        }
        if (!capture_created) {
            return wgc_result(
                capture_created,
                graph->desktop_duplication
                    ? "Desktop Duplication direct-bus capture create"
                    : "WGC direct-bus capture create");
        }

        output = std::move(graph);
        return make_result(RecoverableGpuPipelineStatus::ok);
    }

    RecoverableGpuPipelineResult build_epoch(
        PipelineEpochTicket ticket,
        std::unique_ptr<EpochGraph>& output) {
        rebuild_attempts_.fetch_add(1, std::memory_order_relaxed);
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        RecoverableGpuPipelineResult dimensions;
        for (;;) {
            dimensions = query_epoch_dimensions(ticket, width, height);
            if (dimensions
                || dimensions.hresult
                    != HRESULT_FROM_WIN32(ERROR_RETRY)) {
                break;
            }
            if (!wait_or_stop(config_.retry.monitor_interval_ms)) {
                return make_result(
                    RecoverableGpuPipelineStatus::invalid_state,
                    E_ABORT,
                    "pipeline stopped while waiting for source dimensions");
            }
        }
        if (!dimensions) return dimensions;

        const bool eligible = planar_dimensions_eligible(
            config_, width, height);
        bool planar = false;
        switch (config_.bus_mode) {
        case RecoverableGpuPipelineBusMode::automatic:
            planar = eligible
                && !automatic_planar_disabled_.load(
                    std::memory_order_relaxed);
            break;
        case RecoverableGpuPipelineBusMode::capture_native:
            break;
        case RecoverableGpuPipelineBusMode::planar_encoder_input:
            if (!eligible) {
                return make_result(
                    RecoverableGpuPipelineStatus::build_failed,
                    E_BOUNDS,
                    "resized source no longer satisfies the explicit planar bus contract");
            }
            planar = true;
            break;
        default:
            return make_result(
                RecoverableGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "unknown recoverable GPU pipeline bus mode");
        }

        bool fallback_allowed = false;
        RecoverableGpuPipelineResult built = build_epoch_candidate(
            ticket, width, height, planar, output, fallback_allowed);
        if (!built && planar && fallback_allowed
            && !stop_requested_.load(std::memory_order_acquire)
            && automatic_planar_fallback_error(built)
            && config_.bus_mode
                == RecoverableGpuPipelineBusMode::automatic) {
            output.reset();
            disable_automatic_planar();
            bool ignored = false;
            built = build_epoch_candidate(
                ticket, width, height, false, output, ignored);
        }
        return built;
    }

    RecoverableGpuPipelineResult activate_epoch(EpochGraph& graph) {
        RecoverableGpuPipelineResult injected = before_stage(
            RecoverableGpuPipelineBuildStage::capture_start,
            graph.ticket,
            graph.bus_format);
        if (!injected) return injected;
        graph.callback.enabled.store(true, std::memory_order_release);
        if (!graph.test_only) {
            const WgcResult started = graph.start_capture();
            if (!started) {
                graph.callback.enabled.store(false, std::memory_order_release);
                return wgc_result(
                    started,
                    graph.desktop_duplication
                        ? "Desktop Duplication capture start"
                        : "WGC capture start");
            }
        }
        graph.started = true;
        injected = before_stage(
            RecoverableGpuPipelineBuildStage::complete,
            graph.ticket,
            graph.bus_format);
        if (!injected) {
            graph.callback.enabled.store(false, std::memory_order_release);
            return injected;
        }
        graph.hook_teardown_armed = true;
        return make_result(RecoverableGpuPipelineStatus::ok);
    }

    void install_graph(std::unique_ptr<EpochGraph> graph) noexcept {
        std::lock_guard lock(graph_mutex_);
        graph_ = std::move(graph);
    }

    std::unique_ptr<EpochGraph> take_graph() noexcept {
        std::lock_guard lock(graph_mutex_);
        return std::move(graph_);
    }

    void teardown_active_graph() noexcept {
        std::unique_ptr<EpochGraph> old = take_graph();
        if (old != nullptr) old->teardown();
    }

    void set_state(RecoverableGpuPipelineState state) noexcept {
        try {
            std::lock_guard lock(state_mutex_);
            state_ = state;
        } catch (...) {
        }
    }

    void set_last_error(const RecoverableGpuPipelineResult& error) noexcept {
        try {
            std::lock_guard lock(state_mutex_);
            last_error_ = error;
        } catch (...) {
        }
    }

    void publish_start(
        const RecoverableGpuPipelineResult& result,
        RecoverableGpuPipelineState state) noexcept {
        try {
            std::lock_guard lock(state_mutex_);
            start_result_ = result;
            last_error_ = result;
            state_ = state;
            start_complete_ = true;
            state_cv_.notify_all();
        } catch (...) {
        }
    }

    void fail_start(const RecoverableGpuPipelineResult& error) noexcept {
        try {
            std::lock_guard lock(state_mutex_);
            start_result_ = error;
            last_error_ = error;
            state_ = RecoverableGpuPipelineState::failed;
            start_complete_ = true;
            state_cv_.notify_all();
        } catch (...) {
        }
    }

    void publish_control_failure(
        const RecoverableGpuPipelineResult& error) noexcept {
        try {
            std::lock_guard lock(state_mutex_);
            last_error_ = error;
            state_ = RecoverableGpuPipelineState::failed;
            if (!start_complete_) {
                start_result_ = error;
                start_complete_ = true;
                state_cv_.notify_all();
            }
        } catch (...) {
        }
    }

    bool wait_or_stop(std::uint32_t milliseconds) noexcept {
        if (stop_requested_.load(std::memory_order_acquire)) return false;
        std::unique_lock lock(wait_mutex_);
        wait_cv_.wait_for(
            lock,
            std::chrono::milliseconds(milliseconds),
            [this] { return stop_requested_.load(std::memory_order_acquire); });
        return !stop_requested_.load(std::memory_order_acquire);
    }

    std::uint32_t retry_delay(std::uint32_t failed_attempts) const noexcept {
        const auto& policy = config_.retry;
        std::uint64_t delay = policy.initial_backoff_ms;
        if (policy.maximum_backoff_ms == 0) return 0;
        for (std::uint32_t index = 1; index < failed_attempts; ++index) {
            const std::uint64_t maximum = policy.maximum_backoff_ms;
            if (delay >= maximum / policy.backoff_multiplier) {
                delay = maximum;
            } else {
                delay *= policy.backoff_multiplier;
            }
        }
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            delay, policy.maximum_backoff_ms));
    }

    DetectedFault detect_fault(PipelineEpochTicket ticket) {
        RecoverableGpuPipelineTestFault injected;
        bool has_injected_fault = false;
        if (hook_.poll_fault != nullptr) {
            has_injected_fault = hook_.poll_fault(
                    hook_.context, ticket, injected)
                && injected.kind
                    != RecoverableGpuPipelineTestFaultKind::none;
        }

        std::lock_guard lock(graph_mutex_);
        if (graph_ == nullptr) return {};
        if (has_injected_fault) {
            return {
                injected.kind,
                injected.hresult,
                "injected graph fault",
                graph_->planar_bus
                    && injected.kind
                        == RecoverableGpuPipelineTestFaultKind::capture_error
                    && !infrastructure_failure_hresult(injected.hresult)};
        }

        std::uint32_t observed_width = 0;
        std::uint32_t observed_height = 0;
        bool observed_dimensions = false;
        if (hook_.query_source_dimensions != nullptr) {
            observed_dimensions = hook_.query_source_dimensions(
                hook_.context, ticket, observed_width, observed_height);
        } else if (!graph_->test_only) {
            const WgcMailboxState mailbox = graph_->capture_mailbox();
            observed_width = mailbox.source_width;
            observed_height = mailbox.source_height;
            observed_dimensions = observed_width != 0 && observed_height != 0;
        }
        if (config_.mailbox.mode == WgcMailboxMode::full_frame
            && observed_dimensions
            && observed_width != 0 && observed_height != 0
            && (observed_width != graph_->source_width
                || observed_height != graph_->source_height)) {
            return {
                RecoverableGpuPipelineTestFaultKind::capture_error,
                E_BOUNDS,
                "full-frame source dimensions changed",
                false};
        }

        if (graph_->test_only) return {};
        const HRESULT removed = graph_->device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            return {
                RecoverableGpuPipelineTestFaultKind::device_lost,
                removed,
                "D3D11 device was removed"};
        }
        if (graph_->capture_target_closed()) {
            const WgcResult error = graph_->capture_error();
            return {
                RecoverableGpuPipelineTestFaultKind::target_closed,
                error.hresult,
                error.message};
        }
        if (!graph_->capture_is_running()) {
            const WgcResult error = graph_->capture_error();
            const bool device_lost = device_failure_hresult(error.hresult);
            const bool planar_failure = graph_->planar_bus
                && (error.status == WgcStatus::d3d_error
                    || error.status == WgcStatus::capture_error
                    || error.status == WgcStatus::not_supported)
                && !infrastructure_failure_hresult(error.hresult);
            return {
                error.status == WgcStatus::target_closed
                    ? RecoverableGpuPipelineTestFaultKind::target_closed
                    : device_lost
                    ? RecoverableGpuPipelineTestFaultKind::device_lost
                    : RecoverableGpuPipelineTestFaultKind::capture_error,
                error.hresult,
                error.message,
                planar_failure};
        }
        if (!graph_->pipeline.accepting()) {
            const AsyncGpuPipelineResult error = graph_->pipeline.last_error();
            const bool device_lost =
                error.status == AsyncGpuPipelineStatus::device_lost
                || device_failure_hresult(error.hresult);
            const bool infrastructure_failure =
                device_lost
                || error.status == AsyncGpuPipelineStatus::out_of_memory
                || infrastructure_failure_hresult(error.hresult);
            return {
                device_lost
                    ? RecoverableGpuPipelineTestFaultKind::async_device_lost
                    : RecoverableGpuPipelineTestFaultKind::capture_error,
                error.hresult,
                error.message,
                graph_->planar_bus && !infrastructure_failure};
        }
        const WgcResult capture_error = graph_->capture_error();
        if (capture_error.status == WgcStatus::d3d_error
            || capture_error.status == WgcStatus::capture_error
            || capture_error.status == WgcStatus::out_of_memory
            || capture_error.status == WgcStatus::not_supported) {
            const bool device_lost = device_failure_hresult(
                capture_error.hresult);
            const bool planar_failure = graph_->planar_bus
                && capture_error.status != WgcStatus::out_of_memory
                && !infrastructure_failure_hresult(capture_error.hresult);
            return {
                device_lost
                    ? RecoverableGpuPipelineTestFaultKind::device_lost
                    : RecoverableGpuPipelineTestFaultKind::capture_error,
                capture_error.hresult,
                capture_error.message,
                planar_failure};
        }
        return {};
    }

    void acknowledge_first_recovered_frame(
        PipelineEpochTicket ticket) noexcept {
        const auto epoch = supervisor_.snapshot();
        if (!epoch.first_frame_pending || epoch.ticket != ticket) return;
        bool published = false;
        if (hook_.frame_published != nullptr) {
            published = hook_.frame_published(hook_.context, ticket);
        }
        if (!published) {
            std::lock_guard lock(graph_mutex_);
            if (graph_ != nullptr && !graph_->test_only
                && graph_->ticket == ticket) {
                const WgcCaptureStats capture = graph_->capture_statistics();
                const AsyncGpuPipelineStats pipeline = graph_->pipeline.stats();
                published = capture.published_frames != 0
                    && pipeline.processed_frames != 0
                    && pipeline.forced_keyframes != 0;
            }
        }
        if (published) {
            (void)supervisor_.acknowledge_frame_published(ticket);
        }
    }

    bool recover_from_fault(
        PipelineEpochTicket failed_ticket,
        const DetectedFault& fault,
        std::uint32_t failed_builds = 0,
        bool wait_before_first_build = false) {
        if (fault.disable_automatic_planar) {
            disable_automatic_planar();
        }
        const bool device_failure =
            fault.kind == RecoverableGpuPipelineTestFaultKind::device_lost
            || fault.kind
                == RecoverableGpuPipelineTestFaultKind::async_device_lost;
        if (device_failure) {
            last_device_removed_reason_.store(
                fault.hresult, std::memory_order_relaxed);
        }
        set_state(RecoverableGpuPipelineState::quiescing);
        set_last_error(make_result(
            device_failure
                ? RecoverableGpuPipelineStatus::device_lost
                : RecoverableGpuPipelineStatus::build_failed,
            fault.hresult,
            fault.message.empty() ? "GPU graph failed" : fault.message));

        const auto reported = supervisor_.report_device_lost(
            failed_ticket, static_cast<std::int32_t>(fault.hresult));
        if (!reported) {
            set_last_error(make_result(
                RecoverableGpuPipelineStatus::system_error,
                E_UNEXPECTED,
                "epoch supervisor rejected the active graph failure"));
            set_state(RecoverableGpuPipelineState::failed);
            return false;
        }

        teardown_active_graph();
        const auto retired = supervisor_.retire(failed_ticket);
        if (!retired) {
            set_last_error(make_result(
                RecoverableGpuPipelineStatus::system_error,
                E_UNEXPECTED,
                "epoch supervisor could not retire the failed graph"));
            set_state(RecoverableGpuPipelineState::failed);
            return false;
        }
        set_state(RecoverableGpuPipelineState::retired);

        PipelineEpochTicket retired_ticket = failed_ticket;
        if (failed_builds > config_.retry.max_retry_count) {
            std::string message = "GPU graph recovery retries exhausted";
            if (!fault.message.empty()) {
                message += ": ";
                message += fault.message;
            }
            set_last_error(make_result(
                RecoverableGpuPipelineStatus::retry_exhausted,
                fault.hresult,
                std::move(message)));
            set_state(RecoverableGpuPipelineState::failed);
            return false;
        }
        if (wait_before_first_build
            && !wait_or_stop(retry_delay(failed_builds))) {
            return false;
        }
        for (;;) {
            if (stop_requested_.load(std::memory_order_acquire)) return false;
            PipelineEpochTicket build_ticket;
            const auto begun = supervisor_.begin_build(
                retired_ticket, build_ticket);
            if (!begun) {
                set_last_error(make_result(
                    RecoverableGpuPipelineStatus::system_error,
                    E_UNEXPECTED,
                    "epoch supervisor could not begin a replacement graph"));
                set_state(RecoverableGpuPipelineState::failed);
                return false;
            }
            set_state(RecoverableGpuPipelineState::building);

            std::unique_ptr<EpochGraph> replacement;
            RecoverableGpuPipelineResult built = build_epoch(
                build_ticket, replacement);
            if (stop_requested_.load(std::memory_order_acquire)) {
                if (replacement != nullptr) replacement->teardown();
                (void)supervisor_.abandon_build(build_ticket);
                return false;
            }
            if (built) {
                const auto ready = supervisor_.mark_ready(build_ticket);
                if (!ready) {
                    built = make_result(
                        RecoverableGpuPipelineStatus::system_error,
                        E_UNEXPECTED,
                        "epoch supervisor rejected the ready replacement graph");
                } else {
                    set_state(RecoverableGpuPipelineState::ready);
                    built = activate_epoch(*replacement);
                }
            }

            if (built) {
                const auto resumed = supervisor_.resume(build_ticket);
                if (resumed) {
                    install_graph(std::move(replacement));
                    consecutive_retry_count_.store(0, std::memory_order_relaxed);
                    set_last_error(make_result(
                        RecoverableGpuPipelineStatus::ok));
                    set_state(RecoverableGpuPipelineState::running);
                    return true;
                }
                built = make_result(
                    RecoverableGpuPipelineStatus::system_error,
                    E_UNEXPECTED,
                    "epoch supervisor could not resume the replacement graph");
            }

            const bool planar_fallback_selected =
                disable_planar_after_graph_failure(replacement.get(), built);
            if (replacement != nullptr) replacement->teardown();
            const auto abandoned = supervisor_.abandon_build(build_ticket);
            if (!abandoned) {
                set_last_error(make_result(
                    RecoverableGpuPipelineStatus::system_error,
                    E_UNEXPECTED,
                    "epoch supervisor could not abandon a failed replacement graph"));
                set_state(RecoverableGpuPipelineState::failed);
                return false;
            }
            rebuild_failures_.fetch_add(1, std::memory_order_relaxed);
            ++failed_builds;
            consecutive_retry_count_.store(
                failed_builds, std::memory_order_relaxed);
            set_last_error(built);
            set_state(RecoverableGpuPipelineState::retired);
            retired_ticket = build_ticket;

            if (!planar_fallback_selected
                && !retryable_initial_build_error(built)) {
                set_state(RecoverableGpuPipelineState::failed);
                return false;
            }

            if (failed_builds > config_.retry.max_retry_count) {
                std::string message = "GPU graph recovery retries exhausted";
                if (!built.message.empty()) {
                    message += ": ";
                    message += built.message;
                }
                set_last_error(make_result(
                    RecoverableGpuPipelineStatus::retry_exhausted,
                    built.hresult,
                    std::move(message)));
                set_state(RecoverableGpuPipelineState::failed);
                return false;
            }
            if (!wait_or_stop(retry_delay(failed_builds))) return false;
        }
    }

    void control_main() noexcept {
        const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(apartment);
        if (FAILED(apartment)) {
            publish_start(
                make_result(
                    RecoverableGpuPipelineStatus::system_error,
                    apartment,
                    "WinRT control-thread initialization failed"),
                RecoverableGpuPipelineState::failed);
            return;
        }

        try {
            const PipelineEpochTicket initial_ticket = supervisor_.ticket();
            set_state(RecoverableGpuPipelineState::building);
            std::unique_ptr<EpochGraph> initial_graph;
            RecoverableGpuPipelineResult built = build_epoch(
                initial_ticket, initial_graph);
            if (built
                && !stop_requested_.load(std::memory_order_acquire)) {
                built = activate_epoch(*initial_graph);
            }
            PipelineEpochTicket current_ticket = initial_ticket;
            if (!built) {
                const bool planar_fallback_selected =
                    disable_planar_after_graph_failure(initial_graph.get(), built);
                if (initial_graph != nullptr) initial_graph->teardown();
                if (stop_requested_.load(std::memory_order_acquire)) {
                    publish_start(
                        make_result(
                            RecoverableGpuPipelineStatus::invalid_state,
                            E_ABORT,
                            "pipeline startup was stopped"),
                        RecoverableGpuPipelineState::stopped);
                    if (uninitialize) RoUninitialize();
                    return;
                }
                rebuild_failures_.fetch_add(1, std::memory_order_relaxed);
                if (!planar_fallback_selected
                    && !retryable_initial_build_error(built)) {
                    publish_start(built, RecoverableGpuPipelineState::failed);
                    if (uninitialize) RoUninitialize();
                    return;
                }
                consecutive_retry_count_.store(1, std::memory_order_relaxed);
                const DetectedFault initial_fault{
                    built.status == RecoverableGpuPipelineStatus::device_lost
                        ? RecoverableGpuPipelineTestFaultKind::device_lost
                        : RecoverableGpuPipelineTestFaultKind::capture_error,
                    built.hresult,
                    built.message,
                    false};
                if (!recover_from_fault(
                        initial_ticket, initial_fault, 1, true)) {
                    if (stop_requested_.load(std::memory_order_acquire)) {
                        publish_start(
                            make_result(
                                RecoverableGpuPipelineStatus::invalid_state,
                                E_ABORT,
                                "pipeline startup was stopped"),
                            RecoverableGpuPipelineState::stopped);
                    } else {
                        publish_start(
                            last_error(),
                            RecoverableGpuPipelineState::failed);
                    }
                    if (uninitialize) RoUninitialize();
                    return;
                }
                current_ticket = supervisor_.ticket();
                publish_start(
                    make_result(RecoverableGpuPipelineStatus::ok),
                    RecoverableGpuPipelineState::running);
            } else {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    initial_graph->teardown();
                    publish_start(
                        make_result(
                            RecoverableGpuPipelineStatus::invalid_state,
                            E_ABORT,
                            "pipeline startup was stopped"),
                        RecoverableGpuPipelineState::stopped);
                    if (uninitialize) RoUninitialize();
                    return;
                }
                install_graph(std::move(initial_graph));
                publish_start(
                    make_result(RecoverableGpuPipelineStatus::ok),
                    RecoverableGpuPipelineState::running);
            }

            while (wait_or_stop(config_.retry.monitor_interval_ms)) {
                acknowledge_first_recovered_frame(current_ticket);
                const DetectedFault fault = detect_fault(current_ticket);
                if (fault.kind == RecoverableGpuPipelineTestFaultKind::none) {
                    continue;
                }
                if (fault.kind
                    == RecoverableGpuPipelineTestFaultKind::target_closed) {
                    set_last_error(make_result(
                        RecoverableGpuPipelineStatus::target_closed,
                        fault.hresult,
                        fault.message.empty()
                            ? "capture target closed"
                            : fault.message));
                    teardown_active_graph();
                    set_state(RecoverableGpuPipelineState::failed);
                    break;
                }
                if (!recover_from_fault(current_ticket, fault)) break;
                current_ticket = supervisor_.ticket();
            }
        } catch (const std::bad_alloc&) {
            RecoverableGpuPipelineResult error;
            error.status = RecoverableGpuPipelineStatus::out_of_memory;
            error.hresult = E_OUTOFMEMORY;
            publish_control_failure(error);
        } catch (...) {
            RecoverableGpuPipelineResult error;
            error.status = RecoverableGpuPipelineStatus::system_error;
            error.hresult = E_FAIL;
            publish_control_failure(error);
        }

        teardown_active_graph();
        if (stop_requested_.load(std::memory_order_acquire)) {
            set_state(RecoverableGpuPipelineState::stopped);
        }
        if (uninitialize) RoUninitialize();
    }

    void join_control_thread(bool mark_stopped) noexcept {
        std::lock_guard lock(lifecycle_mutex_);
        if (control_thread_.joinable()) {
            if (control_thread_.get_id() == std::this_thread::get_id()) {
                control_thread_.detach();
            } else {
                control_thread_.join();
            }
        }
        if (mark_stopped) set_state(RecoverableGpuPipelineState::stopped);
    }

    TargetKind target_kind_ = TargetKind::window;
    HWND window_ = nullptr;
    HMONITOR monitor_ = nullptr;
    RecoverableGpuPipelineConfig config_{};
    EncodedPacketCallback callback_ = nullptr;
    void* callback_context_ = nullptr;
    RecoverableGpuPipelineTestHook hook_{};
    PipelineEpochSupervisor supervisor_{};

    mutable std::mutex graph_mutex_;
    std::unique_ptr<EpochGraph> graph_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    RecoverableGpuPipelineState state_ =
        RecoverableGpuPipelineState::stopped;
    RecoverableGpuPipelineResult last_error_{};
    RecoverableGpuPipelineResult start_result_{};
    bool start_complete_ = false;
    std::mutex lifecycle_mutex_;
    std::thread control_thread_;
    bool started_once_ = false;
    std::atomic<bool> stop_requested_{false};
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::atomic<std::uint64_t> rebuild_attempts_{0};
    std::atomic<std::uint64_t> rebuild_failures_{0};
    std::atomic<std::uint64_t> automatic_planar_fallbacks_{0};
    std::atomic<std::uint32_t> consecutive_retry_count_{0};
    std::atomic<bool> automatic_planar_disabled_{false};
    std::atomic<HRESULT> last_device_removed_reason_{S_OK};
    static thread_local Impl* active_callback_owner_;
};

thread_local RecoverableGpuPipeline::Impl*
    RecoverableGpuPipeline::Impl::active_callback_owner_ = nullptr;

RecoverableGpuPipeline::RecoverableGpuPipeline() noexcept = default;
RecoverableGpuPipeline::~RecoverableGpuPipeline() { stop(); }

RecoverableGpuPipeline::RecoverableGpuPipeline(
    RecoverableGpuPipeline&& other) noexcept {
    impl_.store(
        other.impl_.exchange({}, std::memory_order_acq_rel),
        std::memory_order_release);
}

RecoverableGpuPipeline& RecoverableGpuPipeline::operator=(
    RecoverableGpuPipeline&& other) noexcept {
    if (this != &other) {
        stop();
        impl_.store(
            other.impl_.exchange({}, std::memory_order_acq_rel),
            std::memory_order_release);
    }
    return *this;
}

RecoverableGpuPipelineResult RecoverableGpuPipeline::create_for_window(
    HWND window,
    const RecoverableGpuPipelineConfig& config,
    EncodedPacketCallback callback,
    void* callback_context,
    RecoverableGpuPipeline& output) noexcept {
    try {
        if (window == nullptr) {
            return make_result(
                RecoverableGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "capture window is null");
        }
        RecoverableGpuPipelineConfig normalized;
        DXGI_FORMAT bus_format = DXGI_FORMAT_UNKNOWN;
        const RecoverableGpuPipelineResult valid = normalize_config(
            config, callback, normalized, bus_format);
        if (!valid) return valid;
        if (normalized.monitor_backend
            == RecoverableMonitorCaptureBackend::desktop_duplication) {
            return make_result(
                RecoverableGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "Desktop Duplication is only available for monitor capture");
        }
        auto implementation = std::make_shared<Impl>(
            Impl::TargetKind::window,
            window,
            nullptr,
            std::move(normalized),
            callback,
            callback_context);
        output.stop();
        output.impl_.store(std::move(implementation), std::memory_order_release);
        return make_result(RecoverableGpuPipelineStatus::ok);
    } catch (const std::bad_alloc&) {
        return make_result(
            RecoverableGpuPipelineStatus::out_of_memory,
            E_OUTOFMEMORY,
            "recoverable pipeline allocation failed");
    } catch (...) {
        return make_result(
            RecoverableGpuPipelineStatus::system_error,
            E_FAIL,
            "unknown recoverable window pipeline creation failure");
    }
}

RecoverableGpuPipelineResult RecoverableGpuPipeline::create_for_monitor(
    HMONITOR monitor,
    const RecoverableGpuPipelineConfig& config,
    EncodedPacketCallback callback,
    void* callback_context,
    RecoverableGpuPipeline& output) noexcept {
    try {
        if (monitor == nullptr) {
            return make_result(
                RecoverableGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "capture monitor is null");
        }
        RecoverableGpuPipelineConfig normalized;
        DXGI_FORMAT bus_format = DXGI_FORMAT_UNKNOWN;
        const RecoverableGpuPipelineResult valid = normalize_config(
            config, callback, normalized, bus_format);
        if (!valid) return valid;
        if (normalized.monitor_backend
                == RecoverableMonitorCaptureBackend::desktop_duplication
            && (normalized.capture.require_border
                || normalized.capture.include_secondary_windows
                || normalized.capture.min_update_interval_us != 0)) {
            return make_result(
                RecoverableGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "Desktop Duplication cannot satisfy the requested WGC-only options");
        }
        auto implementation = std::make_shared<Impl>(
            Impl::TargetKind::monitor,
            nullptr,
            monitor,
            std::move(normalized),
            callback,
            callback_context);
        output.stop();
        output.impl_.store(std::move(implementation), std::memory_order_release);
        return make_result(RecoverableGpuPipelineStatus::ok);
    } catch (const std::bad_alloc&) {
        return make_result(
            RecoverableGpuPipelineStatus::out_of_memory,
            E_OUTOFMEMORY,
            "recoverable pipeline allocation failed");
    } catch (...) {
        return make_result(
            RecoverableGpuPipelineStatus::system_error,
            E_FAIL,
            "unknown recoverable monitor pipeline creation failure");
    }
}

RecoverableGpuPipelineResult RecoverableGpuPipeline::start() noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    if (!implementation) {
        return make_result(
            RecoverableGpuPipelineStatus::invalid_state,
            E_UNEXPECTED,
            "recoverable GPU pipeline is uninitialized");
    }
    try {
        return implementation->start();
    } catch (const std::bad_alloc&) {
        return make_result(
            RecoverableGpuPipelineStatus::out_of_memory,
            E_OUTOFMEMORY);
    } catch (...) {
        return make_result(
            RecoverableGpuPipelineStatus::system_error,
            E_FAIL);
    }
}

void RecoverableGpuPipeline::stop() noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    if (implementation) implementation->stop();
}

RecoverableGpuPipelineSnapshot RecoverableGpuPipeline::snapshot() const noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    return implementation
        ? implementation->snapshot()
        : RecoverableGpuPipelineSnapshot{};
}

RecoverableGpuPipelineResult RecoverableGpuPipeline::last_error() const noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    return implementation
        ? implementation->last_error()
        : make_result(
            RecoverableGpuPipelineStatus::invalid_state,
            E_UNEXPECTED,
            "recoverable GPU pipeline is uninitialized");
}

bool RecoverableGpuPipeline::initialized() const noexcept {
    return impl_.load(std::memory_order_acquire) != nullptr;
}

bool RecoverableGpuPipeline::running() const noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    return implementation && implementation->running();
}

const char* recoverable_gpu_pipeline_status_string(
    RecoverableGpuPipelineStatus status) noexcept {
    switch (status) {
    case RecoverableGpuPipelineStatus::ok: return "ok";
    case RecoverableGpuPipelineStatus::invalid_argument:
        return "invalid argument";
    case RecoverableGpuPipelineStatus::invalid_state: return "invalid state";
    case RecoverableGpuPipelineStatus::unsupported: return "unsupported";
    case RecoverableGpuPipelineStatus::target_closed: return "target closed";
    case RecoverableGpuPipelineStatus::build_failed: return "build failed";
    case RecoverableGpuPipelineStatus::device_lost: return "device lost";
    case RecoverableGpuPipelineStatus::retry_exhausted:
        return "retries exhausted";
    case RecoverableGpuPipelineStatus::out_of_memory: return "out of memory";
    case RecoverableGpuPipelineStatus::system_error: return "system error";
    default: return "unknown recoverable pipeline status";
    }
}

const char* recoverable_gpu_pipeline_state_string(
    RecoverableGpuPipelineState state) noexcept {
    switch (state) {
    case RecoverableGpuPipelineState::stopped: return "stopped";
    case RecoverableGpuPipelineState::starting: return "starting";
    case RecoverableGpuPipelineState::running: return "running";
    case RecoverableGpuPipelineState::quiescing: return "quiescing";
    case RecoverableGpuPipelineState::retired: return "retired";
    case RecoverableGpuPipelineState::building: return "building";
    case RecoverableGpuPipelineState::ready: return "ready";
    case RecoverableGpuPipelineState::stopping: return "stopping";
    case RecoverableGpuPipelineState::failed: return "failed";
    default: return "unknown recoverable pipeline state";
    }
}

} // namespace fluxcap::gpu
