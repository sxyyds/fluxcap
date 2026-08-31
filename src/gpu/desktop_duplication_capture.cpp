#include "desktop_duplication_capture.hpp"

#include "desktop_duplication_policy.hpp"
#include "session_event_monitor.hpp"
#include "shared_frame_bus.hpp"
#include "side_data_geometry.hpp"
#include "win32_cursor_shape.hpp"

#include <d3d10.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;

WgcResult make_result(
    WgcStatus status,
    HRESULT hresult = S_OK,
    std::string message = {}) {
    if (message.empty()) message = wgc_status_string(status);
    return {status, hresult, std::move(message)};
}

WgcResult exception_result() noexcept {
    try {
        throw;
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

WgcResult gpu_result(const GpuError& error, const char* stage) {
    WgcStatus status = WgcStatus::capture_error;
    switch (error.status) {
    case GpuStatus::invalid_argument:
        status = WgcStatus::invalid_argument;
        break;
    case GpuStatus::unsupported:
        status = WgcStatus::not_supported;
        break;
    case GpuStatus::timeout:
        status = WgcStatus::no_buffer;
        break;
    case GpuStatus::out_of_memory:
        status = WgcStatus::out_of_memory;
        break;
    case GpuStatus::device_lost:
        status = WgcStatus::d3d_error;
        break;
    default:
        break;
    }
    std::string message = stage != nullptr ? stage : "GPU operation";
    message += ": ";
    message += error.what();
    return make_result(
        status,
        FAILED(error.hresult) ? error.hresult : E_FAIL,
        std::move(message));
}

bool planar_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010;
}

GpuPixelFormat gpu_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return GpuPixelFormat::bgra8;
    case DXGI_FORMAT_NV12: return GpuPixelFormat::nv12;
    case DXGI_FORMAT_P010: return GpuPixelFormat::p010;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return GpuPixelFormat::rgba16_float;
    default: return GpuPixelFormat::bgra8;
    }
}

GpuError transform_error(
    GpuStatus status, HRESULT hresult, const char* message) noexcept {
    GpuError result;
    result.status = status;
    result.hresult = hresult;
    if (message != nullptr) {
        (void)strncpy_s(
            result.message.data(), result.message.size(), message, _TRUNCATE);
    }
    return result;
}

GpuError transform_ok() noexcept {
    return transform_error(GpuStatus::ok, S_OK, "ok");
}

bool same_device(ID3D11Device* left, ID3D11Device* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> left_identity;
    ComPtr<IUnknown> right_identity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity)))
        && left_identity.Get() == right_identity.Get();
}

bool legacy_color_space_contract(
    DXGI_FORMAT input_format,
    DXGI_COLOR_SPACE_TYPE input_color,
    DXGI_FORMAT output_format,
    DXGI_COLOR_SPACE_TYPE output_color) noexcept {
    if (input_format != DXGI_FORMAT_B8G8R8A8_UNORM
        || input_color != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        return false;
    }
    if (output_format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        return output_color == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
    if (output_format == DXGI_FORMAT_NV12
        || output_format == DXGI_FORMAT_P010) {
        return output_color == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
            || output_color == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
    }
    return false;
}

DXGI_FORMAT capture_format(WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_B8G8R8A8_UNORM;
}

DXGI_COLOR_SPACE_TYPE capture_color(WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

DXGI_COLOR_SPACE_TYPE resolved_bus_color(
    const SharedFrameBusConfig& config,
    WgcPixelFormat input) noexcept {
    if (config.color_space != DXGI_COLOR_SPACE_CUSTOM) {
        return config.color_space;
    }
    if (config.format == DXGI_FORMAT_P010
        && input == WgcPixelFormat::rgba16_float) {
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    }
    if (planar_format(config.format)) {
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }
    return capture_color(input);
}

bool valid_options(const WgcCaptureOptions& options) noexcept {
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
        && options.capture_epoch_nonce != 0
        && !options.require_border
        && !options.include_secondary_windows
        && options.min_update_interval_us == 0
        && options.frame_timeout_ms != 0
        && options.frame_timeout_ms <= 1'000
        && options.access_lost_retry_initial_ms <= 60'000
        && options.access_lost_retry_max_ms <= 600'000
        && options.session_retry_interval_ms <= 60'000
        && options.idle_republish_interval_ms <= 10'000
        && options.gdi_poll_interval_ms != 0
        && options.gdi_poll_interval_ms <= 1'000;
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

bool resolve_mailbox(
    const WgcMailboxConfig& config,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint64_t generation,
    WgcMailboxFrameInfo& output) noexcept {
    output = {};
    output.source_width = source_width;
    output.source_height = source_height;
    output.generation = generation;
    if (config.mode == WgcMailboxMode::full_frame) {
        output.width = source_width;
        output.height = source_height;
        return source_width != 0 && source_height != 0;
    }
    if (config.width == 0 || config.height == 0
        || config.width > source_width || config.height > source_height) {
        return false;
    }
    output.width = config.width;
    output.height = config.height;
    if (config.mode == WgcMailboxMode::centered_region) {
        output.x = (source_width - config.width) / 2;
        output.y = (source_height - config.height) / 2;
        return true;
    }
    if (config.x > source_width - config.width
        || config.y > source_height - config.height) {
        return false;
    }
    output.x = config.x;
    output.y = config.y;
    return true;
}

bool same_luid(const LUID& left, const LUID& right) noexcept {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

bool native_rect(const RECT& input, WgcRect& output) noexcept {
    output = {};
    const std::int64_t width =
        static_cast<std::int64_t>(input.right) - input.left;
    const std::int64_t height =
        static_cast<std::int64_t>(input.bottom) - input.top;
    if (input.left < 0 || input.top < 0 || width <= 0 || height <= 0
        || width > std::numeric_limits<std::uint32_t>::max()
        || height > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    output = {
        input.left,
        input.top,
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height)};
    return true;
}

bool intersect_rect(
    const WgcRect& input,
    const WgcMailboxFrameInfo& mailbox,
    WgcRect& output) noexcept {
    const std::int64_t roi_left = mailbox.x;
    const std::int64_t roi_top = mailbox.y;
    const std::int64_t roi_right = roi_left + mailbox.width;
    const std::int64_t roi_bottom = roi_top + mailbox.height;
    const std::int64_t input_right =
        static_cast<std::int64_t>(input.x) + input.width;
    const std::int64_t input_bottom =
        static_cast<std::int64_t>(input.y) + input.height;
    const std::int64_t left = std::max<std::int64_t>(input.x, roi_left);
    const std::int64_t top = std::max<std::int64_t>(input.y, roi_top);
    const std::int64_t right = std::min(input_right, roi_right);
    const std::int64_t bottom = std::min(input_bottom, roi_bottom);
    if (left >= right || top >= bottom) return false;
    output.x = static_cast<std::int32_t>(left - roi_left);
    output.y = static_cast<std::int32_t>(top - roi_top);
    output.width = static_cast<std::uint32_t>(right - left);
    output.height = static_cast<std::uint32_t>(bottom - top);
    return true;
}

bool scale_cursor_shape(
    WgcCursorShape& shape,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t destination_width,
    std::uint32_t destination_height) {
    internal::SideDataGeometry geometry;
    geometry.source_width = source_width;
    geometry.source_height = source_height;
    geometry.output_width = destination_width;
    geometry.output_height = destination_height;
    WgcCursorShape output;
    if (!internal::scale_cursor_shape(shape, geometry, output)) return false;
    output.sequence = internal::desktop_duplication_cursor_shape_key(output);
    shape = std::move(output);
    return true;
}

class AcquiredFrame final {
public:
    explicit AcquiredFrame(IDXGIOutputDuplication* duplication) noexcept
        : duplication_(duplication) {}
    ~AcquiredFrame() {
        if (duplication_ != nullptr) (void)duplication_->ReleaseFrame();
    }
    AcquiredFrame(const AcquiredFrame&) = delete;
    AcquiredFrame& operator=(const AcquiredFrame&) = delete;
private:
    IDXGIOutputDuplication* duplication_ = nullptr;
};

class RotatedDesktopTransform final {
public:
    struct CachedInputView final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorInputView> view;
    };

    struct CachedOutputView final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorOutputView> view;
    };

    GpuError initialize(
        ID3D11Device* source_device,
        std::uint32_t source_input_width,
        std::uint32_t source_input_height,
        DXGI_FORMAT source_input_format,
        DXGI_COLOR_SPACE_TYPE source_input_color,
        const WgcRect& source_input_region,
        std::uint32_t source_output_width,
        std::uint32_t source_output_height,
        DXGI_FORMAT source_output_format,
        DXGI_COLOR_SPACE_TYPE source_output_color,
        DXGI_MODE_ROTATION source_rotation) {
        if (source_device == nullptr || source_input_width == 0
            || source_input_height == 0 || source_output_width == 0
            || source_output_height == 0
            || source_input_width
                > static_cast<std::uint32_t>(
                    std::numeric_limits<LONG>::max())
            || source_input_height
                > static_cast<std::uint32_t>(
                    std::numeric_limits<LONG>::max())
            || source_output_width
                > static_cast<std::uint32_t>(
                    std::numeric_limits<LONG>::max())
            || source_output_height
                > static_cast<std::uint32_t>(
                    std::numeric_limits<LONG>::max())
            || source_input_format == DXGI_FORMAT_UNKNOWN
            || source_output_format == DXGI_FORMAT_UNKNOWN
            || source_input_color == DXGI_COLOR_SPACE_CUSTOM
            || source_input_color == DXGI_COLOR_SPACE_RESERVED
            || source_output_color == DXGI_COLOR_SPACE_CUSTOM
            || source_output_color == DXGI_COLOR_SPACE_RESERVED
            || !internal::desktop_duplication_rect_in_bounds(
                source_input_region,
                source_input_width,
                source_input_height)) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "invalid rotated Desktop Duplication transform configuration");
        }
        switch (source_rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
            rotation = D3D11_VIDEO_PROCESSOR_ROTATION_90;
            break;
        case DXGI_MODE_ROTATION_ROTATE180:
            rotation = D3D11_VIDEO_PROCESSOR_ROTATION_180;
            break;
        case DXGI_MODE_ROTATION_ROTATE270:
            rotation = D3D11_VIDEO_PROCESSOR_ROTATION_270;
            break;
        default:
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "rotated Desktop Duplication transform requires a rotation");
        }

        device = source_device;
        device->GetImmediateContext(&context);
        HRESULT hr = device.As(&video_device);
        if (SUCCEEDED(hr)) hr = context.As(&video_context);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "D3D11 video processing is unavailable for display rotation");
        }
        (void)context.As(&video_context1);

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {60, 1};
        content.InputWidth = source_input_width;
        content.InputHeight = source_input_height;
        content.OutputFrameRate = content.InputFrameRate;
        content.OutputWidth = source_output_width;
        content.OutputHeight = source_output_height;
        content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        hr = video_device->CreateVideoProcessorEnumerator(
            &content, &enumerator);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "display-rotation video processor enumeration failed");
        }

        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        hr = enumerator->GetVideoProcessorCaps(&caps);
        if (FAILED(hr)
            || (caps.FeatureCaps
                & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) == 0
            || caps.RateConversionCapsCount == 0) {
            return transform_error(
                GpuStatus::unsupported,
                FAILED(hr) ? hr : E_NOINTERFACE,
                "the video processor does not support display rotation");
        }
        UINT input_support = 0;
        UINT output_support = 0;
        hr = enumerator->CheckVideoProcessorFormat(
            source_input_format, &input_support);
        if (SUCCEEDED(hr)) {
            hr = enumerator->CheckVideoProcessorFormat(
                source_output_format, &output_support);
        }
        if (FAILED(hr)
            || (input_support
                & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0
            || (output_support
                & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
            return transform_error(
                GpuStatus::unsupported,
                FAILED(hr) ? hr : E_NOINTERFACE,
                "rotated display format conversion is unsupported");
        }

        ComPtr<ID3D11VideoProcessorEnumerator1> enumerator1;
        const HRESULT enumerator1_result = enumerator.As(&enumerator1);
        if (SUCCEEDED(enumerator1_result)) {
            BOOL supported = FALSE;
            hr = enumerator1->CheckVideoProcessorFormatConversion(
                source_input_format,
                source_input_color,
                source_output_format,
                source_output_color,
                &supported);
            if (FAILED(hr) || supported == FALSE) {
                return transform_error(
                    GpuStatus::unsupported,
                    FAILED(hr) ? hr : E_NOINTERFACE,
                    "rotated display color-space conversion is unsupported");
            }
        } else if (!legacy_color_space_contract(
                source_input_format,
                source_input_color,
                source_output_format,
                source_output_color)) {
            return transform_error(
                GpuStatus::unsupported,
                enumerator1_result,
                "color-aware rotated display conversion probing is unavailable");
        }
        if (video_context1 == nullptr
            && !legacy_color_space_contract(
                source_input_format,
                source_input_color,
                source_output_format,
                source_output_color)) {
            return transform_error(
                GpuStatus::unsupported,
                E_NOINTERFACE,
                "rotated HDR conversion requires ID3D11VideoContext1");
        }
        hr = video_device->CreateVideoProcessor(
            enumerator.Get(), 0, &processor);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "display-rotation video processor creation failed");
        }

        input_width = source_input_width;
        input_height = source_input_height;
        input_format = source_input_format;
        input_color = source_input_color;
        input_region = source_input_region;
        output_width = source_output_width;
        output_height = source_output_height;
        output_format = source_output_format;
        output_color = source_output_color;
        return transform_ok();
    }

    GpuError process_into(
        ID3D11Texture2D* input,
        ID3D11Texture2D* destination) {
        if (input == nullptr || destination == nullptr || processor == nullptr) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_POINTER,
                "rotated display transform is uninitialized");
        }
        if (input == destination) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "rotated display source and destination alias");
        }
        D3D11_TEXTURE2D_DESC input_description{};
        input->GetDesc(&input_description);
        if (input_description.Width != input_width
            || input_description.Height != input_height
            || input_description.Format != input_format
            || input_description.MipLevels != 1
            || input_description.ArraySize != 1
            || input_description.SampleDesc.Count != 1
            || input_description.SampleDesc.Quality != 0) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "rotation input texture does not match the epoch contract");
        }
        D3D11_TEXTURE2D_DESC output_description{};
        destination->GetDesc(&output_description);
        if (output_description.Width != output_width
            || output_description.Height != output_height
            || output_description.Format != output_format
            || output_description.MipLevels != 1
            || output_description.ArraySize != 1
            || output_description.SampleDesc.Count != 1
            || output_description.SampleDesc.Quality != 0
            || output_description.Usage != D3D11_USAGE_DEFAULT
            || output_description.CPUAccessFlags != 0
            || (output_description.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "rotation output texture does not match the bus contract");
        }
        ComPtr<ID3D11Device> input_device;
        ComPtr<ID3D11Device> output_device;
        input->GetDevice(&input_device);
        destination->GetDevice(&output_device);
        if (!same_device(device.Get(), input_device.Get())
            || !same_device(device.Get(), output_device.Get())) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "rotation textures belong to a different D3D11 device");
        }

        ID3D11VideoProcessorInputView* input_view = nullptr;
        for (auto& cached : input_views) {
            if (cached.texture.Get() == input) {
                input_view = cached.view.Get();
                break;
            }
        }
        if (input_view == nullptr) {
            CachedInputView cached;
            cached.texture = input;
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC description{};
            description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            description.Texture2D.MipSlice = 0;
            description.Texture2D.ArraySlice = 0;
            const HRESULT created =
                video_device->CreateVideoProcessorInputView(
                    input, enumerator.Get(), &description, &cached.view);
            if (FAILED(created)) {
                return transform_error(
                    GpuStatus::unsupported,
                    created,
                    "rotation input view creation failed");
            }
            input_view = cached.view.Get();
            input_views.push_back(std::move(cached));
        }

        ID3D11VideoProcessorOutputView* output_view = nullptr;
        for (auto& cached : output_views) {
            if (cached.texture.Get() == destination) {
                output_view = cached.view.Get();
                break;
            }
        }
        if (output_view == nullptr) {
            CachedOutputView cached;
            cached.texture = destination;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC description{};
            description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            description.Texture2D.MipSlice = 0;
            const HRESULT created =
                video_device->CreateVideoProcessorOutputView(
                    destination,
                    enumerator.Get(),
                    &description,
                    &cached.view);
            if (FAILED(created)) {
                return transform_error(
                    GpuStatus::unsupported,
                    created,
                    "rotation output view creation failed");
            }
            output_view = cached.view.Get();
            output_views.push_back(std::move(cached));
        }

        const RECT source_rect{
            input_region.x,
            input_region.y,
            static_cast<LONG>(input_region.x + input_region.width),
            static_cast<LONG>(input_region.y + input_region.height)};
        const RECT destination_rect{
            0,
            0,
            static_cast<LONG>(output_width),
            static_cast<LONG>(output_height)};
        video_context->VideoProcessorSetStreamSourceRect(
            processor.Get(), 0, TRUE, &source_rect);
        video_context->VideoProcessorSetStreamDestRect(
            processor.Get(), 0, TRUE, &destination_rect);
        video_context->VideoProcessorSetOutputTargetRect(
            processor.Get(), TRUE, &destination_rect);
        video_context->VideoProcessorSetStreamFrameFormat(
            processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context->VideoProcessorSetStreamAutoProcessingMode(
            processor.Get(), 0, FALSE);
        video_context->VideoProcessorSetStreamRotation(
            processor.Get(), 0, TRUE, rotation);
        if (video_context1 != nullptr) {
            video_context1->VideoProcessorSetStreamColorSpace1(
                processor.Get(), 0, input_color);
            video_context1->VideoProcessorSetOutputColorSpace1(
                processor.Get(), output_color);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space{};
            input_space.RGB_Range = 0;
            input_space.YCbCr_Matrix = 1;
            video_context->VideoProcessorSetStreamColorSpace(
                processor.Get(), 0, &input_space);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_space{};
            output_space.YCbCr_Matrix = 1;
            output_space.Nominal_Range = output_color
                    == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            video_context->VideoProcessorSetOutputColorSpace(
                processor.Get(), &output_space);
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view;
        const HRESULT transformed = video_context->VideoProcessorBlt(
            processor.Get(), output_view, 0, 1, &stream);
        if (FAILED(transformed)) {
            const HRESULT removed = device->GetDeviceRemovedReason();
            return transform_error(
                FAILED(removed) ? GpuStatus::device_lost
                                : GpuStatus::system_error,
                FAILED(removed) ? removed : transformed,
                "rotated Desktop Duplication VideoProcessorBlt failed");
        }
        return transform_ok();
    }

private:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoContext1> video_context1;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    std::vector<CachedInputView> input_views;
    std::vector<CachedOutputView> output_views;
    WgcRect input_region{};
    std::uint32_t input_width = 0;
    std::uint32_t input_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    DXGI_FORMAT input_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE input_color = DXGI_COLOR_SPACE_CUSTOM;
    DXGI_COLOR_SPACE_TYPE output_color = DXGI_COLOR_SPACE_CUSTOM;
    D3D11_VIDEO_PROCESSOR_ROTATION rotation =
        D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
};

HRESULT compile_cursor_shader(
    const char* source,
    const char* entry,
    const char* target,
    ComPtr<ID3DBlob>& bytecode,
    std::string& diagnostics_out) noexcept {
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &bytecode,
        &diagnostics);
    if (FAILED(hr) && diagnostics != nullptr && diagnostics->GetBufferSize() > 0) {
        try {
            diagnostics_out.assign(
                static_cast<const char*>(diagnostics->GetBufferPointer()),
                strnlen_s(
                    static_cast<const char*>(diagnostics->GetBufferPointer()),
                    diagnostics->GetBufferSize()));
        } catch (...) {
        }
    }
    return hr;
}

// The cursor quad is rasterized over an intermediate copy of the duplicated
// surface. Cursor texel coordinates are interpolated per pixel center as
// exact integers, so every tap is deterministic.
constexpr char cursor_composite_vertex_shader[] = R"hlsl(
struct vs_input {
    float2 position : POSITION;
    float2 cursor_texel : CURSOR_TEXEL;
};
struct ps_input {
    float4 position : SV_Position;
    float2 cursor_texel : CURSOR_TEXEL;
};
ps_input main_vs(vs_input input) {
    ps_input output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.cursor_texel = input.cursor_texel;
    return output;
}
)hlsl";

constexpr char cursor_composite_pixel_shader[] = R"hlsl(
struct ps_input {
    float4 position : SV_Position;
    float2 cursor_texel : CURSOR_TEXEL;
};
Texture2D<float4> cursor_texture : register(t0);
Texture2D<float> cursor_mask : register(t1);
Texture2D<float4> destination_texture : register(t2);
cbuffer cursor_constants : register(b0) {
    uint composite_mode; // 0 color, 1 masked color, 2 monochrome
};
float4 main_ps(ps_input input) : SV_Target {
    float4 destination = destination_texture.Load(
        int3(int2(input.position.xy), 0));
    int2 texel = int2(round(input.cursor_texel));
    if (composite_mode == 2u) {
        // Monochrome GDI rule per channel: out = xor ^ (and & dst).
        float xor_bit = cursor_texture.Load(int3(texel, 0)).r;
        float and_bit = cursor_mask.Load(int3(texel, 0));
        return float4(
            abs(xor_bit - and_bit * destination.rgb),
            destination.a);
    }
    float4 source = cursor_texture.Load(int3(texel, 0));
    if (composite_mode == 1u) {
        // Masked color: alpha is a 1-bit replace mask.
        return source.a >= 0.5f
            ? float4(source.rgb, destination.a)
            : destination;
    }
    return float4(
        source.a * source.rgb + (1.0f - source.a) * destination.rgb,
        destination.a);
}
)hlsl";

class CursorCompositor final {
public:
    GpuError initialize(ID3D11Device* source_device) {
        if (source_device == nullptr) {
            return transform_error(
                GpuStatus::invalid_argument, E_POINTER,
                "cursor compositor requires a D3D11 device");
        }
        device = source_device;
        device->GetImmediateContext(&context);
        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> pixel_bytecode;
        std::string shader_diagnostics;
        HRESULT hr = compile_cursor_shader(
            cursor_composite_vertex_shader,
            "main_vs",
            "vs_5_0",
            vertex_bytecode,
            shader_diagnostics);
        if (SUCCEEDED(hr)) {
            hr = compile_cursor_shader(
                cursor_composite_pixel_shader,
                "main_ps",
                "ps_5_0",
                pixel_bytecode,
                shader_diagnostics);
        }
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                ("cursor composite shader compilation failed: "
                    + shader_diagnostics)
                    .c_str());
        }
        hr = device->CreateVertexShader(
            vertex_bytecode->GetBufferPointer(),
            vertex_bytecode->GetBufferSize(),
            nullptr,
            &vertex_shader);
        if (SUCCEEDED(hr)) {
            hr = device->CreatePixelShader(
                pixel_bytecode->GetBufferPointer(),
                pixel_bytecode->GetBufferSize(),
                nullptr,
                &pixel_shader);
        }
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite shader creation failed");
        }
        D3D11_INPUT_ELEMENT_DESC elements[2]{};
        elements[0].SemanticName = "POSITION";
        elements[0].Format = DXGI_FORMAT_R32G32_FLOAT;
        elements[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elements[1].SemanticName = "CURSOR_TEXEL";
        elements[1].AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;
        elements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
        elements[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        hr = device->CreateInputLayout(
            elements,
            2,
            vertex_bytecode->GetBufferPointer(),
            vertex_bytecode->GetBufferSize(),
            &input_layout);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite input layout creation failed");
        }
        D3D11_BUFFER_DESC vertex_description{};
        vertex_description.Usage = D3D11_USAGE_DYNAMIC;
        vertex_description.ByteWidth = sizeof(CompositeVertex) * 4;
        vertex_description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertex_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(
            &vertex_description, nullptr, &vertex_buffer);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite vertex buffer creation failed");
        }
        D3D11_BUFFER_DESC constant_description{};
        constant_description.Usage = D3D11_USAGE_DYNAMIC;
        constant_description.ByteWidth = 16;
        constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(
            &constant_description, nullptr, &constant_buffer);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite constant buffer creation failed");
        }
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = FALSE;
        hr = device->CreateRasterizerState(
            &rasterizer, &rasterizer_state);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite rasterizer state creation failed");
        }
        // A 1x1 opaque mask stands in whenever the shape has no AND plane.
        D3D11_SUBRESOURCE_DATA mask_data{};
        const std::uint8_t white = 255;
        mask_data.pSysMem = &white;
        mask_data.SysMemPitch = 1;
        D3D11_TEXTURE2D_DESC mask_description{};
        mask_description.Width = 1;
        mask_description.Height = 1;
        mask_description.MipLevels = 1;
        mask_description.ArraySize = 1;
        mask_description.Format = DXGI_FORMAT_R8_UNORM;
        mask_description.SampleDesc.Count = 1;
        mask_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(
            &mask_description, &mask_data, &opaque_mask);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite fallback mask creation failed");
        }
        hr = device->CreateShaderResourceView(
            opaque_mask.Get(), nullptr, &opaque_mask_view);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite fallback mask view creation failed");
        }
        return transform_ok();
    }

    GpuError upload_shape(const WgcCursorShape& shape) {
        if (shape.width == 0 || shape.height == 0 || shape.data.empty()) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "cursor composite shape is empty");
        }
        if (shape.sequence != 0 && shape.sequence == uploaded_sequence
            && cursor_texture != nullptr) {
            return transform_ok();
        }
        mode = static_cast<std::uint32_t>(shape.kind)
                == static_cast<std::uint32_t>(WgcCursorShapeKind::color_bgra8)
            ? 0
            : static_cast<std::uint32_t>(shape.kind)
                    == static_cast<std::uint32_t>(
                        WgcCursorShapeKind::masked_color_bgra8)
                ? 1
                : 2;
        D3D11_TEXTURE2D_DESC color_description{};
        color_description.Width = shape.width;
        color_description.Height = shape.height;
        color_description.MipLevels = 1;
        color_description.ArraySize = 1;
        color_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        color_description.SampleDesc.Count = 1;
        color_description.Usage = D3D11_USAGE_DYNAMIC;
        color_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        color_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device->CreateTexture2D(
            &color_description, nullptr, &cursor_texture);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite texture creation failed");
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(
            cursor_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            cursor_texture.Reset();
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite texture mapping failed");
        }
        const std::uint32_t color_stride = shape.width * 4u;
        if (mode == 2u) {
            // Monochrome: expand the XOR plane into .r, keep alpha opaque.
            const std::uint32_t mask_stride = shape.stride_bytes;
            const std::size_t xor_offset =
                static_cast<std::size_t>(mask_stride) * shape.height;
            for (std::uint32_t row = 0; row < shape.height; ++row) {
                const std::uint8_t* xor_row =
                    shape.data.data() + xor_offset
                    + static_cast<std::size_t>(mask_stride) * row;
                auto* destination_row = static_cast<std::uint8_t*>(
                    mapped.pData)
                    + static_cast<std::size_t>(mapped.RowPitch) * row;
                for (std::uint32_t column = 0; column < shape.width;
                     ++column) {
                    const std::uint32_t byte = column / 8u;
                    const std::uint32_t bit = 7u - (column % 8u);
                    const bool set =
                        (xor_row[byte] & (1u << bit)) != 0;
                    destination_row[column * 4u + 0] = 0;
                    destination_row[column * 4u + 1] = 0;
                    destination_row[column * 4u + 2] = 0;
                    destination_row[column * 4u + 3] = 255;
                    destination_row[column * 4u + 2] = set ? 255 : 0;
                }
            }
        } else {
            for (std::uint32_t row = 0; row < shape.height; ++row) {
                std::copy_n(
                    shape.data.data()
                        + static_cast<std::size_t>(color_stride) * row,
                    color_stride,
                    static_cast<std::uint8_t*>(mapped.pData)
                        + static_cast<std::size_t>(mapped.RowPitch) * row);
            }
        }
        context->Unmap(cursor_texture.Get(), 0);
        hr = device->CreateShaderResourceView(
            cursor_texture.Get(), nullptr, &cursor_view);
        if (FAILED(hr)) {
            cursor_texture.Reset();
            return transform_error(
                GpuStatus::unsupported,
                hr,
                "cursor composite texture view creation failed");
        }

        if (mode == 2u) {
            D3D11_TEXTURE2D_DESC mask_description{};
            mask_description.Width = shape.width;
            mask_description.Height = shape.height;
            mask_description.MipLevels = 1;
            mask_description.ArraySize = 1;
            mask_description.Format = DXGI_FORMAT_R8_UNORM;
            mask_description.SampleDesc.Count = 1;
            mask_description.Usage = D3D11_USAGE_DYNAMIC;
            mask_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            mask_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateTexture2D(
                &mask_description, nullptr, &mask_texture);
            if (FAILED(hr)) {
                return transform_error(
                    GpuStatus::unsupported,
                    hr,
                    "cursor composite mask creation failed");
            }
            hr = context->Map(
                mask_texture.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped);
            if (FAILED(hr)) {
                mask_texture.Reset();
                return transform_error(
                    GpuStatus::unsupported,
                    hr,
                    "cursor composite mask mapping failed");
            }
            for (std::uint32_t row = 0; row < shape.height; ++row) {
                const std::uint8_t* and_row =
                    shape.data.data()
                    + static_cast<std::size_t>(shape.stride_bytes) * row;
                auto* destination_row = static_cast<std::uint8_t*>(
                    mapped.pData)
                    + static_cast<std::size_t>(mapped.RowPitch) * row;
                for (std::uint32_t column = 0; column < shape.width;
                     ++column) {
                    const std::uint32_t byte = column / 8u;
                    const std::uint32_t bit = 7u - (column % 8u);
                    destination_row[column] =
                        (and_row[byte] & (1u << bit)) != 0 ? 255 : 0;
                }
            }
            context->Unmap(mask_texture.Get(), 0);
            hr = device->CreateShaderResourceView(
                mask_texture.Get(), nullptr, &mask_view);
            if (FAILED(hr)) {
                mask_texture.Reset();
                return transform_error(
                    GpuStatus::unsupported,
                    hr,
                    "cursor composite mask view creation failed");
            }
        } else {
            mask_texture.Reset();
            mask_view.Reset();
        }
        uploaded_sequence = shape.sequence;
        shape_width = shape.width;
        shape_height = shape.height;
        return transform_ok();
    }

    // Draws the cursor onto the intermediate target. The shape and position
    // are expressed in logical desktop coordinates; rotation maps the quad
    // into the pre-rotated surface so downstream rotation stays correct.
    GpuError composite(
        ID3D11Texture2D* target,
        const WgcCursorShape& shape,
        std::int32_t logical_x,
        std::int32_t logical_y,
        bool visible,
        DXGI_MODE_ROTATION rotation,
        std::uint32_t rotation_surface_width,
        std::uint32_t rotation_surface_height) {
        if (!visible || shape.width == 0 || shape.height == 0) {
            return transform_ok();
        }
        std::uint32_t logical_width = rotation_surface_width;
        std::uint32_t logical_height = rotation_surface_height;
        if (!internal::desktop_duplication_rotation_dimensions(
                rotation,
                rotation_surface_width,
                rotation_surface_height,
                logical_width,
                logical_height)) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "cursor composite rotation is unknown");
        }
        const GpuError uploaded = upload_shape(shape);
        if (!uploaded) return uploaded;
        if (cursor_view == nullptr
            || (mode == 2u && mask_view == nullptr)) {
            return transform_ok();
        }
        // Clip in logical space, then map the clamped rect into the surface.
        const std::int64_t left = std::max<std::int64_t>(logical_x, 0);
        const std::int64_t top = std::max<std::int64_t>(logical_y, 0);
        const std::int64_t right = std::min<std::int64_t>(
            static_cast<std::int64_t>(logical_x) + shape.width,
            logical_width);
        const std::int64_t bottom = std::min<std::int64_t>(
            static_cast<std::int64_t>(logical_y) + shape.height,
            logical_height);
        if (left >= right || top >= bottom) return transform_ok();
        const WgcRect clipped{
            static_cast<std::int32_t>(left),
            static_cast<std::int32_t>(top),
            static_cast<std::uint32_t>(right - left),
            static_cast<std::uint32_t>(bottom - top)};
        WgcRect surface_rect{};
        const bool rotated = rotation != DXGI_MODE_ROTATION_IDENTITY
            && rotation != DXGI_MODE_ROTATION_UNSPECIFIED;
        if (rotated
            && !internal::desktop_duplication_logical_to_surface_rect(
                clipped,
                rotation_surface_width,
                rotation_surface_height,
                rotation,
                surface_rect)) {
            return transform_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "cursor composite rect cannot map to the rotated surface");
        }
        if (!rotated) surface_rect = clipped;
        const std::int64_t sx0 = surface_rect.x;
        const std::int64_t sy0 = surface_rect.y;
        const std::int64_t sx1 = sx0 + surface_rect.width;
        const std::int64_t sy1 = sy0 + surface_rect.height;
        // Inverse point mapping per surface corner produces the logical
        // corner, and with it the cursor texel offset (minus half a texel so
        // pixel centers sample exact integers).
        const auto inverse_logical = [&](
            std::int64_t sx,
            std::int64_t sy,
            std::int64_t& ox,
            std::int64_t& oy) noexcept {
            switch (rotation) {
            case DXGI_MODE_ROTATION_ROTATE90:
                ox = rotation_surface_height - sy;
                oy = sx;
                break;
            case DXGI_MODE_ROTATION_ROTATE180:
                ox = rotation_surface_width - sx;
                oy = rotation_surface_height - sy;
                break;
            case DXGI_MODE_ROTATION_ROTATE270:
                ox = sy;
                oy = rotation_surface_width - sx;
                break;
            default:
                ox = sx;
                oy = sy;
                break;
            }
        };
        struct Corner final {
            std::int64_t sx;
            std::int64_t sy;
        };
        const Corner corners[4]{
            {sx0, sy0}, {sx1, sy0}, {sx0, sy1}, {sx1, sy1}};
        CompositeVertex vertices[4]{};
        for (std::size_t index = 0; index < 4; ++index) {
            std::int64_t logical_corner_x = 0;
            std::int64_t logical_corner_y = 0;
            inverse_logical(
                corners[index].sx,
                corners[index].sy,
                logical_corner_x,
                logical_corner_y);
            vertices[index].x = static_cast<float>(
                (static_cast<double>(corners[index].sx)
                    / rotation_surface_width)
                    * 2.0
                - 1.0);
            vertices[index].y = static_cast<float>(
                1.0
                - (static_cast<double>(corners[index].sy)
                    / rotation_surface_height)
                    * 2.0);
            vertices[index].u = static_cast<float>(
                logical_corner_x - static_cast<std::int64_t>(logical_x)
                - 0.5);
            vertices[index].v = static_cast<float>(
                logical_corner_y - static_cast<std::int64_t>(logical_y)
                - 0.5);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = context->Map(
            vertex_buffer.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::system_error,
                hr,
                "cursor composite vertex mapping failed");
        }
        std::copy_n(
            vertices,
            4,
            static_cast<CompositeVertex*>(mapped.pData));
        context->Unmap(vertex_buffer.Get(), 0);

        hr = context->Map(
            constant_buffer.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped);
        if (FAILED(hr)) {
            return transform_error(
                GpuStatus::system_error,
                hr,
                "cursor composite constant mapping failed");
        }
        const std::uint32_t constants[4]{mode, 0, 0, 0};
        std::copy_n(constants, 4, static_cast<std::uint32_t*>(mapped.pData));
        context->Unmap(constant_buffer.Get(), 0);

        if (target_view_texture.Get() != target) {
            D3D11_TEXTURE2D_DESC target_description{};
            target->GetDesc(&target_description);
            if (target_description.MipLevels != 1
                || target_description.ArraySize != 1
                || (target_description.BindFlags
                        & D3D11_BIND_SHADER_RESOURCE)
                    == 0
                || (target_description.BindFlags
                        & D3D11_BIND_RENDER_TARGET)
                    == 0) {
                return transform_error(
                    GpuStatus::invalid_argument,
                    E_INVALIDARG,
                    "cursor composite target needs SRV and RTV bindings");
            }
            hr = device->CreateShaderResourceView(
                target, nullptr, &target_view);
            if (SUCCEEDED(hr)) {
                hr = device->CreateRenderTargetView(
                    target, nullptr, &target_rtv);
            }
            if (FAILED(hr)) {
                target_view.Reset();
                target_rtv.Reset();
                target_view_texture.Reset();
                return transform_error(
                    GpuStatus::unsupported,
                    hr,
                    "cursor composite target view creation failed");
            }
            target_view_texture = target;
            target_width = target_description.Width;
            target_height = target_description.Height;
        }
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(target_width);
        viewport.Height = static_cast<float>(target_height);
        viewport.MaxDepth = 1.0f;
        ID3D11ShaderResourceView* views[3]{
            cursor_view.Get(),
            mode == 2u ? mask_view.Get() : opaque_mask_view.Get(),
            target_view.Get()};
        ID3D11Buffer* buffers[2]{vertex_buffer.Get(), constant_buffer.Get()};
        const UINT strides[1]{sizeof(CompositeVertex)};
        const UINT offsets[1]{0};
        context->OMSetRenderTargets(1, &target_rtv, nullptr);
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer_state.Get());
        context->IASetInputLayout(input_layout.Get());
        context->IASetVertexBuffers(
            0, 1, buffers, strides, offsets);
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(vertex_shader.Get(), nullptr, 0);
        context->PSSetShader(pixel_shader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &buffers[1]);
        context->PSSetShaderResources(0, 3, views);
        context->Draw(4, 0);
        ID3D11ShaderResourceView* null_views[3]{nullptr, nullptr, nullptr};
        ID3D11RenderTargetView* null_targets[1]{nullptr};
        context->PSSetShaderResources(0, 3, null_views);
        context->OMSetRenderTargets(1, null_targets, nullptr);
        composited_frames += 1;
        return transform_ok();
    }

    std::uint64_t composited_frames = 0;

private:
    struct CompositeVertex final {
        float x = 0.0f;
        float y = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11InputLayout> input_layout;
    ComPtr<ID3D11Buffer> vertex_buffer;
    ComPtr<ID3D11Buffer> constant_buffer;
    ComPtr<ID3D11RasterizerState> rasterizer_state;
    ComPtr<ID3D11Texture2D> cursor_texture;
    ComPtr<ID3D11ShaderResourceView> cursor_view;
    ComPtr<ID3D11Texture2D> mask_texture;
    ComPtr<ID3D11ShaderResourceView> mask_view;
    ComPtr<ID3D11Texture2D> opaque_mask;
    ComPtr<ID3D11ShaderResourceView> opaque_mask_view;
    ComPtr<ID3D11Texture2D> target_view_texture;
    ComPtr<ID3D11ShaderResourceView> target_view;
    ComPtr<ID3D11RenderTargetView> target_rtv;
    std::uint64_t uploaded_sequence = 0;
    std::uint32_t shape_width = 0;
    std::uint32_t shape_height = 0;
    std::uint32_t target_width = 0;
    std::uint32_t target_height = 0;
    std::uint32_t mode = 0;
};

} // namespace

namespace internal {

HRESULT find_monitor_output(
    HMONITOR monitor,
    IDXGIAdapter1** adapter_output,
    IDXGIOutput1** output_output,
    DXGI_OUTPUT_DESC* description_output) noexcept {
    if (adapter_output != nullptr) *adapter_output = nullptr;
    if (output_output != nullptr) *output_output = nullptr;
    if (description_output != nullptr) *description_output = {};
    if (monitor == nullptr) return E_INVALIDARG;

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return hr;
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> base_output;
            hr = adapter->EnumOutputs(output_index, &base_output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) return hr;
            DXGI_OUTPUT_DESC description{};
            hr = base_output->GetDesc(&description);
            if (FAILED(hr)) return hr;
            if (description.Monitor != monitor || !description.AttachedToDesktop) {
                continue;
            }
            ComPtr<IDXGIOutput1> output;
            hr = base_output.As(&output);
            if (FAILED(hr)) return hr;
            if (description_output != nullptr) *description_output = description;
            if (adapter_output != nullptr) {
                *adapter_output = adapter.Detach();
            }
            if (output_output != nullptr) {
                *output_output = output.Detach();
            }
            return S_OK;
        }
    }
    return DXGI_ERROR_NOT_FOUND;
}

} // namespace internal

struct DesktopDuplicationCaptureState final {
    ~DesktopDuplicationCaptureState() { stop(); }

    WgcResult initialize(
        HMONITOR source_monitor,
        const std::shared_ptr<SharedFrameBusPublisherState>& source_bus,
        const WgcCaptureOptions& source_options,
        const WgcMailboxConfig& source_mailbox) {
        if (source_monitor == nullptr || source_bus == nullptr
            || !valid_options(source_options) || !valid_mailbox(source_mailbox)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "invalid Desktop Duplication capture configuration");
        }

        monitor = source_monitor;
        bus_state = source_bus;
        options = source_options;
        mailbox_config = source_mailbox;
        epoch = options.capture_epoch;
        epoch_nonce = options.capture_epoch_nonce;
        bus_config = internal::shared_frame_bus_config(bus_state);
        bus_planar = planar_format(bus_config.format);
        device = internal::shared_frame_bus_device(bus_state);
        if (device == nullptr) {
            return make_result(
                WgcStatus::invalid_argument,
                E_UNEXPECTED,
                "shared frame bus has no D3D11 device");
        }
        device->GetImmediateContext(&context);
        if (context == nullptr) {
            return make_result(
                WgcStatus::d3d_error,
                E_FAIL,
                "D3D11 device has no immediate context");
        }
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread))) {
            (void)multithread->SetMultithreadProtected(TRUE);
        }
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            return make_result(
                WgcStatus::capture_error,
                HRESULT_FROM_WIN32(GetLastError()),
                "performance counter frequency query failed");
        }
        qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
        const GpuError reserved = internal::reserve_shared_frame_bus_producer(
            bus_state, bus_producer_token);
        if (!reserved) {
            return make_result(
                WgcStatus::invalid_state,
                FAILED(reserved.hresult) ? reserved.hresult : E_UNEXPECTED,
                reserved.what());
        }
        previous_bus_sequence = internal::shared_frame_bus_sequence(bus_state);
        if (options.monitor_session_events) {
            session_events.start();
        }
        return create_duplication_session(nullptr);
    }

    // Sleeps in small slices so worker shutdown stays responsive. Returns
    // false when the stop token fired during the wait.
    bool wait_retry_delay(const std::stop_token* token, std::uint32_t ms) {
        std::uint32_t remaining = ms;
        while (remaining != 0) {
            if (token != nullptr && token->stop_requested()) return false;
            const DWORD slice = remaining < 10 ? remaining : 10;
            Sleep(slice);
            remaining -= slice;
        }
        return !(token != nullptr && token->stop_requested());
    }

    // (Re)creates the duplication session, rotation/planar transforms and
    // cursor-composite intermediate for the current desktop mode. Called once
    // from initialize() and again from the worker on DXGI_ERROR_ACCESS_LOST.
    WgcResult create_duplication_session(const std::stop_token* token) {
        output.Reset();
        duplication.Reset();
        transform = GpuTransform{};
        rotated_transform.reset();
        cursor_intermediate.Reset();

        ComPtr<IDXGIAdapter1> output_adapter;
        HRESULT hr = internal::find_monitor_output(
            monitor, &output_adapter, &output, &output_description);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            target_closed_flag.store(true, std::memory_order_release);
            return make_result(
                WgcStatus::target_closed,
                hr,
                "monitor was detached from the desktop");
        }
        if (FAILED(hr)) {
            return make_result(
                WgcStatus::not_supported,
                hr,
                "monitor is not attached to a duplicable DXGI output");
        }
        rotation = output_description.Rotation;

        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> device_adapter;
        DXGI_ADAPTER_DESC output_adapter_description{};
        DXGI_ADAPTER_DESC device_adapter_description{};
        hr = device.As(&dxgi_device);
        if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&device_adapter);
        if (SUCCEEDED(hr)) hr = output_adapter->GetDesc(&output_adapter_description);
        if (SUCCEEDED(hr)) hr = device_adapter->GetDesc(&device_adapter_description);
        if (FAILED(hr)) {
            return make_result(
                WgcStatus::d3d_error,
                hr,
                "failed to resolve the Desktop Duplication adapter");
        }
        if (!same_luid(
                output_adapter_description.AdapterLuid,
                device_adapter_description.AdapterLuid)) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "shared frame bus device is not on the monitor adapter");
        }

        // Live desktop color space decides the effective pixel format when
        // auto_detect_color is enabled. A change across a rebuild cannot be
        // applied to the fixed bus slot: fail so the epoch is rebuilt.
        color_state = {};
        bool desktop_hdr = false;
        ComPtr<IDXGIOutput6> output6;
        if (SUCCEEDED(output.As(&output6))) {
            DXGI_OUTPUT_DESC1 description1{};
            if (SUCCEEDED(output6->GetDesc1(&description1))) {
                color_state.known = true;
                color_state.color_space = description1.ColorSpace;
                desktop_hdr =
                    internal::desktop_duplication_hdr_color_space(
                        description1.ColorSpace);
                color_state.hdr = desktop_hdr;
            }
        }
        resolved_pixel_format = internal::desktop_duplication_resolve_pixel_format(
            options.auto_detect_color, desktop_hdr, options.pixel_format);
        if (!first_session
            && resolved_pixel_format != epoch_pixel_format) {
            return make_result(
                WgcStatus::not_supported,
                DXGI_ERROR_UNSUPPORTED,
                "desktop color space changed; rebuild the capture epoch "
                "with the new pixel format");
        }
        if (first_session) epoch_pixel_format = resolved_pixel_format;
        bus_color = resolved_bus_color(bus_config, resolved_pixel_format);

        const DXGI_FORMAT primary_format =
            capture_format(resolved_pixel_format);
        DXGI_FORMAT negotiation_formats[2]{};
        const UINT format_count =
            internal::desktop_duplication_format_negotiation(
                primary_format,
                options.allow_format_fallback,
                negotiation_formats);
        for (UINT attempt = 0;; ++attempt) {
            ComPtr<IDXGIOutput5> output5;
            HRESULT duplicate_hr = output.As(&output5);
            if (SUCCEEDED(duplicate_hr)) {
                duplicate_hr = output5->DuplicateOutput1(
                    device.Get(),
                    0,
                    format_count,
                    negotiation_formats,
                    &duplication);
            }
            if (FAILED(duplicate_hr)
                && resolved_pixel_format == WgcPixelFormat::bgra8
                && format_count == 1) {
                duplicate_hr = output->DuplicateOutput(
                    device.Get(), &duplication);
            }
            if (SUCCEEDED(duplicate_hr)) break;
            if (duplicate_hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
                && (options.session_retry_limit == 0
                    || attempt < options.session_retry_limit)
                && wait_retry_delay(token, options.session_retry_interval_ms)) {
                continue;
            }
            return make_result(
                duplicate_hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
                    || duplicate_hr == DXGI_ERROR_UNSUPPORTED
                    || duplicate_hr == E_NOINTERFACE
                    ? WgcStatus::not_supported
                    : WgcStatus::d3d_error,
                duplicate_hr,
                duplicate_hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
                    ? "DXGI output duplication creation failed: session "
                      "limit retries exhausted"
                    : "DXGI output duplication creation failed");
        }
        duplication->GetDesc(&duplication_description);
        const auto normalized_rotation = [](DXGI_MODE_ROTATION value) noexcept {
            return value == DXGI_MODE_ROTATION_UNSPECIFIED
                ? DXGI_MODE_ROTATION_IDENTITY
                : value;
        };
        if (normalized_rotation(rotation)
            != normalized_rotation(duplication_description.Rotation)) {
            return make_result(
                WgcStatus::d3d_error,
                DXGI_ERROR_ACCESS_LOST,
                "Desktop Duplication output rotation changed during creation");
        }
        rotation = duplication_description.Rotation;
        const DXGI_FORMAT actual_format =
            duplication_description.ModeDesc.Format;
        if (actual_format != primary_format) {
            if (internal::desktop_duplication_format_fallback_accepted(
                    actual_format,
                    primary_format,
                    options.allow_format_fallback)) {
                resolved_pixel_format = WgcPixelFormat::bgra8;
                bus_color =
                    resolved_bus_color(bus_config, resolved_pixel_format);
                format_fallbacks.fetch_add(1, std::memory_order_relaxed);
            } else {
                return make_result(
                    WgcStatus::not_supported,
                    DXGI_ERROR_UNSUPPORTED,
                    "Desktop Duplication did not provide the requested pixel format");
            }
        }
        surface_width = duplication_description.ModeDesc.Width;
        surface_height = duplication_description.ModeDesc.Height;
        if (!internal::desktop_duplication_rotation_dimensions(
                rotation,
                surface_width,
                surface_height,
                source_width,
                source_height)) {
            return make_result(
                WgcStatus::not_supported,
                DXGI_ERROR_UNSUPPORTED,
                "Desktop Duplication reported an unknown output rotation");
        }
        const std::int64_t desktop_width =
            static_cast<std::int64_t>(output_description.DesktopCoordinates.right)
            - output_description.DesktopCoordinates.left;
        const std::int64_t desktop_height =
            static_cast<std::int64_t>(output_description.DesktopCoordinates.bottom)
            - output_description.DesktopCoordinates.top;
        if (desktop_width != source_width || desktop_height != source_height) {
            return make_result(
                WgcStatus::capture_error,
                E_UNEXPECTED,
                "Desktop Duplication rotation dimensions disagree with the output");
        }
        const WgcMailboxFrameInfo previous_mailbox = mailbox;
        if (!resolve_mailbox(
                mailbox_config,
                source_width,
                source_height,
                mailbox_generation,
                mailbox)) {
            return make_result(
                WgcStatus::region_unavailable,
                E_BOUNDS,
                "Desktop Duplication mailbox is outside the logical monitor bounds");
        }
        if (!first_session
            && (previous_mailbox.x != mailbox.x
                || previous_mailbox.y != mailbox.y
                || previous_mailbox.width != mailbox.width
                || previous_mailbox.height != mailbox.height)) {
            mailbox_generation += 1;
        }
        rotated = rotation != DXGI_MODE_ROTATION_IDENTITY
            && rotation != DXGI_MODE_ROTATION_UNSPECIFIED;
        const DXGI_FORMAT resolved_format =
            capture_format(resolved_pixel_format);
        if ((!bus_planar
                && (mailbox.width != bus_config.width
                    || mailbox.height != bus_config.height))) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "Desktop Duplication mailbox dimensions do not match the shared frame bus");
        }
        if (!bus_planar
            && (bus_config.format != resolved_format
                || bus_color != capture_color(resolved_pixel_format))) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                "Desktop Duplication direct bus format/color does not match capture");
        }
        const bool invalid_planar_dimensions = bus_planar
            && ((bus_config.width | bus_config.height) & 1u) != 0;
        internal::SideDataGeometry cursor_geometry;
        cursor_geometry.source_width = mailbox.width;
        cursor_geometry.source_height = mailbox.height;
        cursor_geometry.output_width = bus_config.width;
        cursor_geometry.output_height = bus_config.height;
        cursor_geometry.planar_420 = bus_planar;
        const bool position_dependent_cursor_shape =
            options.include_cursor_metadata
            && !internal::phase_invariant_cursor_geometry(cursor_geometry);
        const bool transform_target_missing = (bus_planar || rotated)
            && (bus_config.bind_flags & D3D11_BIND_RENDER_TARGET) == 0;
        if (invalid_planar_dimensions
            || position_dependent_cursor_shape
            || transform_target_missing) {
            return make_result(
                WgcStatus::invalid_argument,
                E_INVALIDARG,
                position_dependent_cursor_shape
                    ? "fractionally scaled independent cursor shape is position dependent"
                : bus_planar
                    ? "planar Desktop Duplication bus must be even and render-target bound"
                    : "rotated Desktop Duplication bus must be render-target bound");
        }
        if (rotated && bus_planar
            && options.planar_transform_backend
                == GpuTransformBackend::deterministic_planar) {
            return make_result(
                WgcStatus::not_supported,
                E_NOINTERFACE,
                "deterministic planar Desktop Duplication rotation is unavailable");
        }
        if (rotated) {
            const WgcRect logical_region{
                static_cast<std::int32_t>(mailbox.x),
                static_cast<std::int32_t>(mailbox.y),
                mailbox.width,
                mailbox.height};
            if (!internal::desktop_duplication_logical_to_surface_rect(
                    logical_region,
                    surface_width,
                    surface_height,
                    rotation,
                    surface_mailbox)) {
                return make_result(
                    WgcStatus::region_unavailable,
                    E_BOUNDS,
                    "Desktop Duplication mailbox cannot be mapped to the rotated surface");
            }
            rotated_transform = std::make_unique<RotatedDesktopTransform>();
            const GpuError transformed = rotated_transform->initialize(
                device.Get(),
                surface_width,
                surface_height,
                resolved_format,
                capture_color(resolved_pixel_format),
                surface_mailbox,
                bus_config.width,
                bus_config.height,
                bus_config.format,
                bus_color,
                rotation);
            if (!transformed) {
                return gpu_result(
                    transformed,
                    "Desktop Duplication rotation transform creation failed");
            }
        } else if (bus_planar) {
            GpuTransformConfig transform_config;
            transform_config.input_width = surface_width;
            transform_config.input_height = surface_height;
            transform_config.input_region_x = mailbox.x;
            transform_config.input_region_y = mailbox.y;
            transform_config.input_region_width = mailbox.width;
            transform_config.input_region_height = mailbox.height;
            transform_config.output_width = bus_config.width;
            transform_config.output_height = bus_config.height;
            transform_config.output_format = gpu_format(bus_config.format);
            transform_config.input_format = resolved_format;
            transform_config.input_color_space =
                capture_color(resolved_pixel_format);
            transform_config.output_color_space = bus_color;
            transform_config.backend = options.planar_transform_backend;
            transform_config.external_output_only = true;
            const GpuError transformed = GpuTransform::create(
                device.Get(), transform_config, transform);
            if (!transformed) {
                return gpu_result(
                    transformed,
                    "Desktop Duplication planar transform creation failed");
            }
        }

        if (options.include_cursor) {
            if (!cursor_compositor_ready) {
                const GpuError compositor =
                    cursor_compositor.initialize(device.Get());
                if (!compositor) {
                    return gpu_result(
                        compositor,
                        "Desktop Duplication cursor compositor creation failed");
                }
                cursor_compositor_ready = true;
            }
            D3D11_TEXTURE2D_DESC intermediate_description{};
            intermediate_description.Width = surface_width;
            intermediate_description.Height = surface_height;
            intermediate_description.MipLevels = 1;
            intermediate_description.ArraySize = 1;
            intermediate_description.Format = resolved_format;
            intermediate_description.SampleDesc.Count = 1;
            intermediate_description.Usage = D3D11_USAGE_DEFAULT;
            intermediate_description.BindFlags =
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            hr = device->CreateTexture2D(
                &intermediate_description, nullptr, &cursor_intermediate);
            if (FAILED(hr)) {
                return make_result(
                    WgcStatus::d3d_error,
                    hr,
                    "Desktop Duplication cursor intermediate creation failed");
            }
        }

        if (first_session) {
            first_session = false;
        } else {
            session_rebuilds.fetch_add(1, std::memory_order_relaxed);
            recovery_successes.fetch_add(1, std::memory_order_relaxed);
        }
        return make_result(WgcStatus::ok);
    }

    WgcResult start() {
        std::lock_guard lock(lifecycle_mutex);
        if (started.load(std::memory_order_acquire)
            || stopped.load(std::memory_order_acquire)
            || duplication == nullptr || bus_producer_token == 0) {
            return make_result(
                WgcStatus::invalid_state,
                E_UNEXPECTED,
                "Desktop Duplication capture cannot be started");
        }
        try {
            started.store(true, std::memory_order_release);
            running.store(true, std::memory_order_release);
            worker = std::jthread(
                [this](std::stop_token token) noexcept { worker_main(token); });
            return make_result(WgcStatus::ok);
        } catch (...) {
            running.store(false, std::memory_order_release);
            started.store(false, std::memory_order_release);
            return exception_result();
        }
    }

    void stop() noexcept {
        std::jthread old_worker;
        {
            std::lock_guard lock(lifecycle_mutex);
            if (stopped.exchange(true, std::memory_order_acq_rel)) return;
            running.store(false, std::memory_order_release);
            if (worker.joinable()) worker.request_stop();
            old_worker = std::move(worker);
        }
        if (old_worker.joinable()
            && old_worker.get_id() != std::this_thread::get_id()) {
            old_worker.join();
        }
        duplication.Reset();
        output.Reset();
        transform = GpuTransform{};
        rotated_transform.reset();
        cursor_intermediate.Reset();
        heartbeat_texture.Reset();
        heartbeat_metadata_valid = false;
        session_events.stop();
        if (bus_state != nullptr && bus_producer_token != 0) {
            internal::release_shared_frame_bus_producer(
                bus_state, bus_producer_token);
            bus_producer_token = 0;
        }
        bus_state.reset();
    }

    WgcResult read_move_rects(
        UINT metadata_bytes,
        std::vector<DXGI_OUTDUPL_MOVE_RECT>& result) {
        result.clear();
        if (metadata_bytes == 0) return make_result(WgcStatus::ok);
        UINT capacity = metadata_bytes;
        UINT written = 0;
        HRESULT hr = S_OK;
        for (;;) {
            const std::size_t count = (
                static_cast<std::size_t>(capacity)
                    + sizeof(DXGI_OUTDUPL_MOVE_RECT) - 1)
                / sizeof(DXGI_OUTDUPL_MOVE_RECT);
            result.resize(count);
            const UINT buffer_bytes = static_cast<UINT>(
                count * sizeof(DXGI_OUTDUPL_MOVE_RECT));
            hr = duplication->GetFrameMoveRects(
                buffer_bytes, result.data(), &written);
            if (hr != DXGI_ERROR_MORE_DATA) break;
            capacity = written;
        }
        if (FAILED(hr) || written % sizeof(DXGI_OUTDUPL_MOVE_RECT) != 0) {
            return make_result(
                WgcStatus::capture_error,
                FAILED(hr) ? hr : E_UNEXPECTED,
                "GetFrameMoveRects failed");
        }
        result.resize(written / sizeof(DXGI_OUTDUPL_MOVE_RECT));
        return make_result(WgcStatus::ok);
    }

    WgcResult read_dirty_rects(
        UINT metadata_bytes,
        std::vector<RECT>& result) {
        result.clear();
        if (metadata_bytes == 0) return make_result(WgcStatus::ok);
        UINT capacity = metadata_bytes;
        UINT written = 0;
        HRESULT hr = S_OK;
        for (;;) {
            const std::size_t count = (
                static_cast<std::size_t>(capacity) + sizeof(RECT) - 1)
                / sizeof(RECT);
            result.resize(count);
            const UINT buffer_bytes = static_cast<UINT>(count * sizeof(RECT));
            hr = duplication->GetFrameDirtyRects(
                buffer_bytes, result.data(), &written);
            if (hr != DXGI_ERROR_MORE_DATA) break;
            capacity = written;
        }
        if (FAILED(hr) || written % sizeof(RECT) != 0) {
            return make_result(
                WgcStatus::capture_error,
                FAILED(hr) ? hr : E_UNEXPECTED,
                "GetFrameDirtyRects failed");
        }
        result.resize(written / sizeof(RECT));
        return make_result(WgcStatus::ok);
    }

    void full_damage(WgcFrameDamage& damage, bool overflow) const noexcept {
        internal::desktop_duplication_full_damage(
            damage, bus_config.width, bus_config.height, overflow);
    }

    WgcResult build_damage(
        const DXGI_OUTDUPL_FRAME_INFO& frame,
        std::uint64_t sequence,
        WgcFrameDamage& damage) {
        if (options.damage_mode == WgcDamageMode::disabled
            || first_frame || force_full_next) {
            full_damage(damage, false);
            if (first_frame || force_full_next) {
                damage.flags |= wgc_damage_discontinuity;
            }
            return make_result(WgcStatus::ok);
        }

        // The VideoProcessor scaling/filtering kernel is selected by the
        // driver. Until the deterministic planar backend is active, no
        // source-space projection can prove final-byte coverage. Publishing a
        // full frame also fail-closes move rectangles for this frame.
        const bool scaled_output = mailbox.width != bus_config.width
            || mailbox.height != bus_config.height;
        if (bus_planar
            && scaled_output
            && !transform.spatially_deterministic()) {
            full_damage(damage, false);
            return make_result(WgcStatus::ok);
        }

        std::vector<DXGI_OUTDUPL_MOVE_RECT> moves;
        std::vector<RECT> dirties;
        WgcResult read = read_move_rects(
            frame.TotalMetadataBufferSize, moves);
        if (!read) return read;
        read = read_dirty_rects(frame.TotalMetadataBufferSize, dirties);
        if (!read) return read;

        damage = {};
        damage.flags = wgc_damage_valid
            | wgc_damage_native
            | wgc_damage_native_move_available;
        bool overflow = false;
        const auto append_dirty = [&](const WgcRect& logical_local) {
            WgcRect mapped;
            bool mapped_ok = false;
            if (bus_planar && transform.spatially_deterministic()) {
                internal::SideDataGeometry geometry;
                geometry.source_width = mailbox.width;
                geometry.source_height = mailbox.height;
                geometry.output_width = bus_config.width;
                geometry.output_height = bus_config.height;
                geometry.planar_420 = true;
                const internal::SideDataMapStatus status =
                    internal::map_local_deterministic_planar_damage_rect(
                        logical_local, geometry, mapped);
                if (status == internal::SideDataMapStatus::empty) return;
                mapped_ok = status == internal::SideDataMapStatus::mapped;
            } else {
                mapped_ok = internal::desktop_duplication_scale_rect(
                    logical_local,
                    mailbox.width,
                    mailbox.height,
                    bus_config.width,
                    bus_config.height,
                    bus_planar,
                    mapped);
            }
            if (!mapped_ok) {
                overflow = true;
                return;
            }
            if (damage.dirty_count >= wgc_max_dirty_rects) {
                overflow = true;
                return;
            }
            damage.dirty_rects[damage.dirty_count++] = mapped;
        };
        for (const auto& source : moves) {
            WgcRect surface_destination;
            if (!native_rect(source.DestinationRect, surface_destination)) {
                overflow = true;
                break;
            }
            WgcRect surface_source{
                source.SourcePoint.x,
                source.SourcePoint.y,
                surface_destination.width,
                surface_destination.height};
            WgcRect logical_destination;
            WgcRect logical_source;
            if (!internal::desktop_duplication_surface_to_logical_rect(
                    surface_destination,
                    surface_width,
                    surface_height,
                    rotation,
                    logical_destination)
                || !internal::desktop_duplication_surface_to_logical_rect(
                    surface_source,
                    surface_width,
                    surface_height,
                    rotation,
                    logical_source)) {
                overflow = true;
                break;
            }
            WgcRect destination;
            if (!intersect_rect(logical_destination, mailbox, destination)) {
                continue;
            }
            const std::int64_t clipped_source_x =
                static_cast<std::int64_t>(logical_source.x)
                + static_cast<std::int64_t>(destination.x + mailbox.x)
                - logical_destination.x;
            const std::int64_t clipped_source_y =
                static_cast<std::int64_t>(logical_source.y)
                + static_cast<std::int64_t>(destination.y + mailbox.y)
                - logical_destination.y;
            const std::int64_t relative_source_x = clipped_source_x - mailbox.x;
            const std::int64_t relative_source_y = clipped_source_y - mailbox.y;
            if (relative_source_x < 0 || relative_source_y < 0
                || relative_source_x + destination.width > mailbox.width
                || relative_source_y + destination.height > mailbox.height) {
                // A move whose source is outside the selected ROI cannot be
                // replayed from the previous bus texture. Treat its destination
                // as dirty while retaining native provenance.
                append_dirty(destination);
                if (overflow) break;
                continue;
            }
            const WgcRect local_source{
                static_cast<std::int32_t>(relative_source_x),
                static_cast<std::int32_t>(relative_source_y),
                destination.width,
                destination.height};
            if (scaled_output) {
                if (bus_planar && transform.spatially_deterministic()) {
                    const internal::SideDataGeometry geometry{
                        mailbox.width,
                        mailbox.height,
                        bus_config.width,
                        bus_config.height,
                        true};
                    internal::DeterministicPlanarMoveMapping mapped;
                    const internal::SideDataMapStatus status =
                        internal::map_local_deterministic_planar_move(
                            local_source, destination, geometry, mapped);
                    if (status == internal::SideDataMapStatus::empty) {
                        continue;
                    }
                    if (status == internal::SideDataMapStatus::mapped) {
                        if (!internal::append_deterministic_planar_move_mapping(
                                mapped, damage)) {
                            full_damage(damage, true);
                            return make_result(WgcStatus::ok);
                        }
                        continue;
                    }
                }
                // Fractional/odd displacement, an empty chroma-aligned
                // interior, or a driver-selected scaled kernel cannot be
                // replayed exactly. Retain native provenance but publish the
                // deterministic destination support as dirty.
                append_dirty(destination);
                if (overflow) break;
                continue;
            }
            WgcMoveRect candidate;
            if (!internal::desktop_duplication_scale_move_exact(
                    local_source,
                    destination,
                    mailbox.width,
                    mailbox.height,
                    bus_config.width,
                    bus_config.height,
                    candidate)
                || (bus_planar
                    && !internal::desktop_duplication_planar_move_aligned(
                        candidate))) {
                append_dirty(destination);
                if (overflow) break;
                continue;
            }
            if (damage.move_count == wgc_max_move_rects) {
                    overflow = true;
                    break;
            }
            damage.move_rects[damage.move_count++] = candidate;
        }
        for (const RECT& source : dirties) {
            if (overflow) break;
            WgcRect surface_dirty;
            if (!native_rect(source, surface_dirty)) {
                overflow = true;
                break;
            }
            WgcRect logical_dirty;
            if (!internal::desktop_duplication_surface_to_logical_rect(
                    surface_dirty,
                    surface_width,
                    surface_height,
                    rotation,
                    logical_dirty)) {
                overflow = true;
                break;
            }
            WgcRect dirty;
            if (!intersect_rect(logical_dirty, mailbox, dirty)) continue;
            append_dirty(dirty);
        }
        if (overflow || (damage.move_count != 0
                && (previous_bus_sequence == 0
                    || previous_bus_sequence >= sequence))) {
            full_damage(damage, true);
        } else if (damage.move_count != 0) {
            damage.base_sequence = previous_bus_sequence;
        }
        (void)frame;
        return make_result(WgcStatus::ok);
    }

    WgcResult read_pointer_shape(
        UINT advertised_bytes,
        WgcCursorShape& shape) {
        UINT required = advertised_bytes;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
        if (required == 0) {
            return make_result(
                WgcStatus::capture_error,
                E_UNEXPECTED,
                "Desktop Duplication reported an empty pointer shape");
        }
        std::vector<std::uint8_t> bytes(required);
        UINT written = 0;
        HRESULT hr = duplication->GetFramePointerShape(
            required, bytes.data(), &written, &info);
        if (hr == DXGI_ERROR_MORE_DATA && written > required) {
            required = written;
            bytes.resize(required);
            hr = duplication->GetFramePointerShape(
                required, bytes.data(), &written, &info);
        }
        if (FAILED(hr) || written == 0 || written > bytes.size()) {
            return make_result(
                WgcStatus::capture_error,
                FAILED(hr) ? hr : E_UNEXPECTED,
                "GetFramePointerShape failed");
        }
        bytes.resize(written);

        shape = {};
        shape.width = info.Width;
        shape.height = info.Height;
        shape.hotspot_x = static_cast<std::uint32_t>(std::max<LONG>(0, info.HotSpot.x));
        shape.hotspot_y = static_cast<std::uint32_t>(std::max<LONG>(0, info.HotSpot.y));
        if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
            if ((shape.height & 1u) != 0) {
                return make_result(
                    WgcStatus::capture_error,
                    E_UNEXPECTED,
                    "monochrome pointer shape has an odd mask height");
            }
            shape.height /= 2;
            shape.kind = WgcCursorShapeKind::monochrome_and_xor;
            shape.stride_bytes = info.Pitch;
            const std::uint64_t expected =
                static_cast<std::uint64_t>(info.Pitch) * shape.height * 2u;
            if (expected != bytes.size()) {
                return make_result(
                    WgcStatus::capture_error,
                    E_UNEXPECTED,
                    "monochrome pointer shape byte count is inconsistent");
            }
            shape.data = std::move(bytes);
        } else if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR
            || info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
            shape.kind = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR
                ? WgcCursorShapeKind::color_bgra8
                : WgcCursorShapeKind::masked_color_bgra8;
            shape.stride_bytes = shape.width * 4u;
            const std::uint64_t source_size =
                static_cast<std::uint64_t>(info.Pitch) * shape.height;
            if (info.Pitch < shape.stride_bytes || source_size != bytes.size()) {
                return make_result(
                    WgcStatus::capture_error,
                    E_UNEXPECTED,
                    "color pointer shape pitch is inconsistent");
            }
            shape.data.resize(
                static_cast<std::size_t>(shape.stride_bytes) * shape.height);
            for (std::uint32_t row = 0; row < shape.height; ++row) {
                std::copy_n(
                    bytes.data() + static_cast<std::size_t>(row) * info.Pitch,
                    shape.stride_bytes,
                    shape.data.data()
                        + static_cast<std::size_t>(row) * shape.stride_bytes);
            }
        } else {
            return make_result(
                WgcStatus::not_supported,
                DXGI_ERROR_UNSUPPORTED,
                "unknown Desktop Duplication pointer shape type");
        }
        if (shape.width == 0 || shape.height == 0
            || shape.width > 256 || shape.height > 256
            || shape.hotspot_x >= shape.width || shape.hotspot_y >= shape.height
            || shape.data.size() > shared_frame_bus_max_cursor_shape_bytes) {
            return make_result(
                WgcStatus::not_supported,
                E_BOUNDS,
                "Desktop Duplication pointer shape exceeds the bus contract");
        }
        const std::uint32_t source_hotspot_x = shape.hotspot_x;
        const std::uint32_t source_hotspot_y = shape.hotspot_y;
        // Keep the unscaled shape for GPU cursor compositing; the bus
        // sidecar receives the mailbox-scaled copy below.
        composite_shape = shape;
        if (!scale_cursor_shape(
                shape,
                mailbox.width,
                mailbox.height,
                bus_config.width,
                bus_config.height)) {
            return make_result(
                WgcStatus::not_supported,
                E_BOUNDS,
                "scaled Desktop Duplication pointer shape exceeds the bus contract");
        }
        cursor_source_hotspot_x = source_hotspot_x;
        cursor_source_hotspot_y = source_hotspot_y;
        cursor_source_hotspot_known = true;
        return make_result(WgcStatus::ok);
    }

    WgcResult build_cursor(
        const DXGI_OUTDUPL_FRAME_INFO& frame,
        WgcCursorInfo& cursor,
        WgcCursorShape& pending_shape,
        bool& has_pending_shape,
        bool allow_win32_position_refresh = false) {
        cursor = {};
        pending_shape = {};
        has_pending_shape = false;
        bool shape_unavailable = false;
        if (!options.include_cursor_metadata && !options.include_cursor) {
            return make_result(WgcStatus::ok);
        }

        if (frame.LastMouseUpdateTime.QuadPart != 0) {
            pointer_known = true;
            pointer_visible = frame.PointerPosition.Visible != FALSE;
            pointer_position = frame.PointerPosition.Position;
            pointer_sample_qpc = static_cast<std::uint64_t>(
                frame.LastMouseUpdateTime.QuadPart);
            pointer_position_estimated = false;
            native_pointer_position_seen = true;
        }
        if (frame.PointerShapeBufferSize != 0) {
            WgcResult read = read_pointer_shape(
                frame.PointerShapeBufferSize, pending_shape);
            if (!read) return read;
            has_pending_shape = true;
            native_pointer_shape_seen = true;
        }

        // Some display drivers composite the pointer into the duplicated image
        // and never return a DXGI shape buffer. Preserve the independent cursor
        // contract with Win32 shape/hotspot sampling and mark only the fallback
        // position as estimated. Once DXGI supplies a native shape it remains
        // authoritative for the rest of this duplication epoch. Heartbeat
        // republishes pass allow_win32_position_refresh so a static desktop
        // still reports fresh cursor coordinates.
        CURSORINFO win32_cursor{sizeof(win32_cursor)};
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (GetCursorInfo(&win32_cursor)) {
            if (!native_pointer_position_seen || allow_win32_position_refresh) {
                pointer_known = true;
                pointer_visible = (win32_cursor.flags & CURSOR_SHOWING) != 0;
                pointer_position.x = win32_cursor.ptScreenPos.x
                    - output_description.DesktopCoordinates.left;
                pointer_position.y = win32_cursor.ptScreenPos.y
                    - output_description.DesktopCoordinates.top;
                pointer_sample_qpc = static_cast<std::uint64_t>(now.QuadPart);
                pointer_position_estimated = true;
            }
            const std::uint64_t refresh_ticks =
                (qpc_frequency
                    * options.cursor_shape_refresh_interval_ms) / 1'000u;
            const bool refresh_shape =
                internal::desktop_duplication_win32_shape_fallback_allowed(
                    native_pointer_shape_seen, has_pending_shape)
                && (current_cursor_shape.sequence == 0
                    || win32_cursor.hCursor != last_win32_cursor
                    || last_cursor_refresh_qpc == 0
                    || static_cast<std::uint64_t>(now.QuadPart)
                        - last_cursor_refresh_qpc >= refresh_ticks);
            if (refresh_shape) {
                if (win32_cursor.hCursor != last_win32_cursor) {
                    cursor_source_hotspot_known = false;
                }
                WgcCursorShape sampled;
                if (internal::extract_win32_cursor_shape(
                        win32_cursor.hCursor, sampled)) {
                    const std::uint32_t source_hotspot_x = sampled.hotspot_x;
                    const std::uint32_t source_hotspot_y = sampled.hotspot_y;
                    const WgcCursorShape unscaled_sampled = sampled;
                    if (!scale_cursor_shape(
                            sampled,
                            mailbox.width,
                            mailbox.height,
                            bus_config.width,
                            bus_config.height)) {
                        shape_unavailable = true;
                    } else {
                        composite_shape = unscaled_sampled;
                        cursor_source_hotspot_x = source_hotspot_x;
                        cursor_source_hotspot_y = source_hotspot_y;
                        cursor_source_hotspot_known = true;
                        const bool changed =
                            current_cursor_shape.sequence == 0
                            || sampled.kind != current_cursor_shape.kind
                            || sampled.width != current_cursor_shape.width
                            || sampled.height != current_cursor_shape.height
                            || sampled.hotspot_x
                                != current_cursor_shape.hotspot_x
                            || sampled.hotspot_y
                                != current_cursor_shape.hotspot_y
                            || sampled.stride_bytes
                                != current_cursor_shape.stride_bytes
                            || sampled.data != current_cursor_shape.data;
                        if (changed) {
                            pending_shape = std::move(sampled);
                            has_pending_shape = true;
                        }
                        last_win32_cursor = win32_cursor.hCursor;
                        last_cursor_refresh_qpc =
                            static_cast<std::uint64_t>(now.QuadPart);
                    }
                } else {
                    // Do not attach a stale shape sequence to a frame after a
                    // refresh attempt observed a shape it could not extract.
                    shape_unavailable = true;
                }
            }
        }

        const WgcCursorShape* shape = shape_unavailable
            ? nullptr
            : has_pending_shape
            ? &pending_shape
            : current_cursor_shape.sequence != 0
            ? &current_cursor_shape
            : nullptr;
        // Public cursor coordinates name the hot spot. DXGI reports the
        // shape's top-left, while GetCursorInfo already reports the hot spot.
        // DXGI also documents Position as invalid while the pointer is hidden.
        std::int32_t hotspot_position_x = pointer_position.x;
        std::int32_t hotspot_position_y = pointer_position.y;
        bool position_available = pointer_known && pointer_visible;
        if (position_available && !pointer_position_estimated) {
            if (cursor_source_hotspot_known && !shape_unavailable) {
                position_available =
                    internal::desktop_duplication_pointer_hotspot(
                        pointer_position.x,
                        pointer_position.y,
                        cursor_source_hotspot_x,
                        cursor_source_hotspot_y,
                        hotspot_position_x,
                        hotspot_position_y);
            } else {
                position_available = false;
            }
        }
        const std::int64_t hotspot_x = hotspot_position_x;
        const std::int64_t hotspot_y = hotspot_position_y;
        const std::int64_t screen_x =
            static_cast<std::int64_t>(
                output_description.DesktopCoordinates.left) + hotspot_x;
        const std::int64_t screen_y =
            static_cast<std::int64_t>(
                output_description.DesktopCoordinates.top) + hotspot_y;
        std::int32_t frame_x = 0;
        std::int32_t frame_y = 0;
        if (position_available
            && (screen_x < std::numeric_limits<std::int32_t>::min()
                || screen_x > std::numeric_limits<std::int32_t>::max()
                || screen_y < std::numeric_limits<std::int32_t>::min()
                || screen_y > std::numeric_limits<std::int32_t>::max()
                || !internal::scale_signed_coordinate_nearest(
                    hotspot_x - mailbox.x,
                    mailbox.width,
                    bus_config.width,
                    frame_x)
                || !internal::scale_signed_coordinate_nearest(
                    hotspot_y - mailbox.y,
                    mailbox.height,
                    bus_config.height,
                    frame_y))) {
            position_available = false;
        }
        if (pointer_visible) cursor.flags |= wgc_cursor_visible;
        if (position_available) {
            cursor.flags |= wgc_cursor_position_valid;
            if (pointer_position_estimated) {
                cursor.flags |= wgc_cursor_position_estimated;
            }
            cursor.sample_qpc = pointer_sample_qpc;
            cursor.screen_x = static_cast<std::int32_t>(screen_x);
            cursor.screen_y = static_cast<std::int32_t>(screen_y);
            cursor.frame_x = frame_x;
            cursor.frame_y = frame_y;
        }
        if (shape != nullptr) {
            cursor.shape_sequence = shape->sequence;
            cursor.width = shape->width;
            cursor.height = shape->height;
            cursor.hotspot_x = shape->hotspot_x;
            cursor.hotspot_y = shape->hotspot_y;
        }
        if (has_pending_shape) cursor.flags |= wgc_cursor_shape_pending;
        return make_result(WgcStatus::ok);
    }

    WgcResult process_frame(
        const DXGI_OUTDUPL_FRAME_INFO& frame,
        ID3D11Texture2D* source) {
        D3D11_TEXTURE2D_DESC source_description{};
        if (source == nullptr) {
            return make_result(
                WgcStatus::d3d_error, E_POINTER,
                "Desktop Duplication returned a null texture");
        }
        source->GetDesc(&source_description);
        if (source_description.Width != surface_width
            || source_description.Height != surface_height
            || source_description.Format
                != capture_format(resolved_pixel_format)
            || source_description.MipLevels != 1
            || source_description.ArraySize != 1
            || source_description.SampleDesc.Count != 1) {
            return make_result(
                WgcStatus::d3d_error, E_UNEXPECTED,
                "Desktop Duplication texture changed its epoch contract");
        }
        SharedFrameBusWriteLease lease;
        const GpuError begun = internal::begin_shared_frame_bus_publish(
            bus_state, bus_producer_token, lease);
        if (!begun) {
            if (begun.status == GpuStatus::timeout) {
                skipped_no_buffer.fetch_add(1, std::memory_order_relaxed);
                force_full_next = true;
                return make_result(WgcStatus::ok);
            }
            return gpu_result(begun, "Desktop Duplication bus lease failed");
        }

        WgcFrameDamage damage;
        WgcResult built = build_damage(frame, lease.sequence(), damage);
        if (!built) return built;
        WgcCursorInfo cursor;
        WgcCursorShape pending_shape;
        bool has_pending_shape = false;
        built = build_cursor(frame, cursor, pending_shape, has_pending_shape);
        if (!built) return built;
        if (!options.include_cursor_metadata) {
            // Compositing-only mode must not leak cursor side data.
            cursor = {};
            pending_shape = {};
            has_pending_shape = false;
        }

        // Cursor compositing runs on an intermediate copy of the full
        // surface; every downstream path consumes the composited texture.
        // DXGI-native positions name the shape's top-left while the Win32
        // fallback names the hot spot; normalize to the top-left here.
        ID3D11Texture2D* frame_source = source;
        if (options.include_cursor && cursor_intermediate != nullptr) {
            context->CopySubresourceRegion(
                cursor_intermediate.Get(), 0, 0, 0, 0, source, 0, nullptr);
            ingress_copy_submissions.fetch_add(1, std::memory_order_relaxed);
            const std::int32_t cursor_top_left_x = pointer_position_estimated
                ? pointer_position.x
                    - static_cast<std::int32_t>(composite_shape.hotspot_x)
                : pointer_position.x;
            const std::int32_t cursor_top_left_y = pointer_position_estimated
                ? pointer_position.y
                    - static_cast<std::int32_t>(composite_shape.hotspot_y)
                : pointer_position.y;
            const GpuError composited = cursor_compositor.composite(
                cursor_intermediate.Get(),
                composite_shape,
                cursor_top_left_x,
                cursor_top_left_y,
                pointer_known && pointer_visible,
                rotation,
                surface_width,
                surface_height);
            if (!composited) {
                return gpu_result(
                    composited,
                    "Desktop Duplication cursor compositing failed");
            }
            cursor_composited_frames.fetch_add(
                1, std::memory_order_relaxed);
            frame_source = cursor_intermediate.Get();
        }

        if (rotated) {
            const GpuError converted = rotated_transform->process_into(
                frame_source, lease.texture());
            if (!converted) {
                return gpu_result(
                    converted,
                    "Desktop Duplication rotated frame conversion failed");
            }
            ingress_transform_submissions.fetch_add(
                1, std::memory_order_relaxed);
        } else if (bus_planar) {
            const GpuError converted = transform.process_into(
                frame_source, lease.texture());
            if (!converted) {
                return gpu_result(
                    converted,
                    "Desktop Duplication planar frame conversion failed");
            }
            ingress_transform_submissions.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            D3D11_BOX source_box{};
            source_box.left = mailbox.x;
            source_box.top = mailbox.y;
            source_box.right = mailbox.x + mailbox.width;
            source_box.bottom = mailbox.y + mailbox.height;
            source_box.front = 0;
            source_box.back = 1;
            context->CopySubresourceRegion(
                lease.texture(), 0, 0, 0, 0, frame_source, 0, &source_box);
            ingress_copy_submissions.fetch_add(1, std::memory_order_relaxed);
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        SharedFrameBusFrameMetadata metadata;
        metadata.valid_fields = shared_frame_bus_metadata_qpc
            | shared_frame_bus_metadata_source_dimensions
            | shared_frame_bus_metadata_roi
            | shared_frame_bus_metadata_mailbox_generation
            | shared_frame_bus_metadata_color_space;
        metadata.timestamp_qpc = frame.LastPresentTime.QuadPart > 0
            ? static_cast<std::uint64_t>(frame.LastPresentTime.QuadPart)
            : static_cast<std::uint64_t>(now.QuadPart);
        metadata.qpc_frequency = qpc_frequency;
        metadata.mailbox_generation = mailbox_generation;
        metadata.source_width = source_width;
        metadata.source_height = source_height;
        metadata.roi_x = mailbox.x;
        metadata.roi_y = mailbox.y;
        metadata.roi_width = mailbox.width;
        metadata.roi_height = mailbox.height;
        metadata.color_space = bus_color;
        SharedFrameBusFrameSideData side_data;
        side_data.epoch = epoch;
        side_data.epoch_nonce = epoch_nonce;
        side_data.damage = damage;
        side_data.cursor = cursor;

        if (options.idle_republish_interval_ms != 0) {
            if (heartbeat_texture == nullptr) {
                D3D11_TEXTURE2D_DESC heartbeat_description{};
                heartbeat_description.Width = bus_config.width;
                heartbeat_description.Height = bus_config.height;
                heartbeat_description.MipLevels = 1;
                heartbeat_description.ArraySize = 1;
                heartbeat_description.Format = bus_config.format;
                heartbeat_description.SampleDesc.Count = 1;
                heartbeat_description.Usage = D3D11_USAGE_DEFAULT;
                const HRESULT created = device->CreateTexture2D(
                    &heartbeat_description,
                    nullptr,
                    &heartbeat_texture);
                if (FAILED(created)) {
                    return make_result(
                        WgcStatus::d3d_error,
                        created,
                        "Desktop Duplication heartbeat texture creation failed");
                }
            }
            context->CopySubresourceRegion(
                heartbeat_texture.Get(),
                0,
                0,
                0,
                0,
                lease.texture(),
                0,
                nullptr);
            // The heartbeat snapshot is a FluxCap-explicit GPU copy and must
            // stay observable in the ingress counters.
            ingress_copy_submissions.fetch_add(1, std::memory_order_relaxed);
        }

        const std::uint64_t committed_sequence = lease.sequence();
        const GpuError committed = internal::commit_shared_frame_bus_publish(
            bus_state,
            bus_producer_token,
            std::move(lease),
            metadata,
            side_data);
        if (!committed) {
            return gpu_result(committed, "Desktop Duplication bus commit failed");
        }
        if (has_pending_shape) {
            const GpuError published_shape =
                internal::publish_shared_frame_bus_cursor_shape(
                    bus_state, bus_producer_token, pending_shape);
            if (!published_shape) {
                return gpu_result(
                    published_shape,
                    "Desktop Duplication cursor shape publish failed");
            }
            current_cursor_shape = std::move(pending_shape);
            cursor_shape_updates.fetch_add(1, std::memory_order_relaxed);
        }
        heartbeat_metadata = metadata;
        heartbeat_metadata_valid = true;
        last_publish_qpc = static_cast<std::uint64_t>(now.QuadPart);
        previous_bus_sequence = committed_sequence;
        first_frame = false;
        force_full_next = false;
        published_frames.fetch_add(1, std::memory_order_relaxed);
        if (frame.LastPresentTime.QuadPart > 0) {
            desktop_present_frames.fetch_add(1, std::memory_order_relaxed);
        }
        if ((damage.flags & wgc_damage_native) != 0) {
            native_damage_frames.fetch_add(1, std::memory_order_relaxed);
        }
        if ((damage.flags & wgc_damage_full_frame) != 0) {
            full_damage_frames.fetch_add(1, std::memory_order_relaxed);
        }
        if (options.include_cursor_metadata) {
            cursor_metadata_frames.fetch_add(1, std::memory_order_relaxed);
        }
        return make_result(WgcStatus::ok);
    }

    // Re-publishes the last committed frame while the desktop is static so
    // downstream encoders keep receiving frames at a bounded cadence. The
    // sidecar carries a valid-but-empty damage report (identical content) and
    // a Win32-refreshed cursor position.
    WgcResult publish_heartbeat() {
        if (options.idle_republish_interval_ms == 0
            || !heartbeat_metadata_valid
            || heartbeat_texture == nullptr) {
            return make_result(WgcStatus::ok);
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (!internal::desktop_duplication_heartbeat_due(
                last_publish_qpc,
                static_cast<std::uint64_t>(now.QuadPart),
                qpc_frequency,
                options.idle_republish_interval_ms)) {
            return make_result(WgcStatus::ok);
        }
        SharedFrameBusWriteLease lease;
        const GpuError begun = internal::begin_shared_frame_bus_publish(
            bus_state, bus_producer_token, lease);
        if (!begun) {
            if (begun.status == GpuStatus::timeout) {
                return make_result(WgcStatus::ok);
            }
            return gpu_result(begun, "Desktop Duplication heartbeat lease failed");
        }
        context->CopySubresourceRegion(
            lease.texture(), 0, 0, 0, 0, heartbeat_texture.Get(), 0, nullptr);
        ingress_copy_submissions.fetch_add(1, std::memory_order_relaxed);
        WgcCursorInfo cursor;
        WgcCursorShape pending_shape;
        bool has_pending_shape = false;
        DXGI_OUTDUPL_FRAME_INFO empty{};
        const WgcResult built = build_cursor(
            empty, cursor, pending_shape, has_pending_shape, true);
        if (!built) return built;
        if (!options.include_cursor_metadata) {
            pending_shape = {};
            has_pending_shape = false;
            cursor = {};
        }
        SharedFrameBusFrameSideData side_data;
        side_data.epoch = epoch;
        side_data.epoch_nonce = epoch_nonce;
        side_data.damage = {};
        side_data.damage.flags = wgc_damage_valid;
        side_data.cursor = cursor;
        const std::uint64_t committed_sequence = lease.sequence();
        const GpuError committed = internal::commit_shared_frame_bus_publish(
            bus_state,
            bus_producer_token,
            std::move(lease),
            heartbeat_metadata,
            side_data);
        if (!committed) {
            return gpu_result(
                committed, "Desktop Duplication heartbeat commit failed");
        }
        if (has_pending_shape) {
            const GpuError published_shape =
                internal::publish_shared_frame_bus_cursor_shape(
                    bus_state, bus_producer_token, pending_shape);
            if (!published_shape) {
                return gpu_result(
                    published_shape,
                    "Desktop Duplication heartbeat cursor shape publish failed");
            }
            current_cursor_shape = std::move(pending_shape);
            cursor_shape_updates.fetch_add(1, std::memory_order_relaxed);
        }
        previous_bus_sequence = committed_sequence;
        last_publish_qpc = static_cast<std::uint64_t>(now.QuadPart);
        idle_republished_frames.fetch_add(1, std::memory_order_relaxed);
        return make_result(WgcStatus::ok);
    }

    void worker_main(std::stop_token token) noexcept {
        std::uint32_t access_lost_attempts = 0;
        std::uint64_t observed_session_generation =
            session_events.generation();
        while (!token.stop_requested()) {
            if (options.monitor_session_events) {
                const std::uint64_t generation = session_events.generation();
                if (generation != observed_session_generation) {
                    observed_session_generation = generation;
                    session_event_notifications.fetch_add(
                        1, std::memory_order_relaxed);
                    // Lock/RDP/power transitions invalidate stale cursor
                    // caches and any partial damage continuity.
                    force_full_next = true;
                    last_win32_cursor = nullptr;
                    last_cursor_refresh_qpc = 0;
                }
            }
            DXGI_OUTDUPL_FRAME_INFO frame{};
            ComPtr<IDXGIResource> resource;
            const HRESULT acquired = duplication->AcquireNextFrame(
                options.frame_timeout_ms, &frame, &resource);
            if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
                WgcResult heartbeat;
                try {
                    heartbeat = publish_heartbeat();
                } catch (...) {
                    heartbeat = exception_result();
                }
                if (!heartbeat) {
                    record_error(heartbeat);
                    break;
                }
                continue;
            }
            if (acquired == DXGI_ERROR_ACCESS_LOST) {
                if (options.access_lost_retry_limit != 0
                    && access_lost_attempts
                        >= options.access_lost_retry_limit) {
                    record_error(make_result(
                        WgcStatus::d3d_error,
                        acquired,
                        "Desktop Duplication access was lost and the "
                        "rebuild limit was exhausted"));
                    break;
                }
                access_lost_attempts += 1;
                recovery_attempts.fetch_add(1, std::memory_order_relaxed);
                if (!wait_retry_delay(
                        &token,
                        internal::desktop_duplication_backoff_delay_ms(
                            access_lost_attempts,
                            options.access_lost_retry_initial_ms,
                            options.access_lost_retry_max_ms))) {
                    break;
                }
                WgcResult rebuilt;
                try {
                    rebuilt = create_duplication_session(&token);
                } catch (...) {
                    rebuilt = exception_result();
                }
                if (rebuilt) {
                    access_lost_attempts = 0;
                    force_full_next = true;
                    continue;
                }
                record_error(rebuilt);
                break;
            }
            if (FAILED(acquired)) {
                record_error(make_result(
                    WgcStatus::d3d_error,
                    acquired,
                    "Desktop Duplication frame acquisition failed"));
                break;
            }
            AcquiredFrame release(duplication.Get());
            received_frames.fetch_add(1, std::memory_order_relaxed);
            if (frame.AccumulatedFrames > 1) {
                dropped_at_source.fetch_add(
                    frame.AccumulatedFrames - 1,
                    std::memory_order_relaxed);
                force_full_next = true;
            }
            if (frame.ProtectedContentMaskedOut) {
                protected_content_frames.fetch_add(
                    1, std::memory_order_relaxed);
            }
            ComPtr<ID3D11Texture2D> texture;
            const HRESULT queried = resource.As(&texture);
            if (FAILED(queried)) {
                record_error(make_result(
                    WgcStatus::d3d_error,
                    queried,
                    "Desktop Duplication resource is not a D3D11 texture"));
                break;
            }
            WgcResult processed;
            try {
                processed = process_frame(frame, texture.Get());
            } catch (...) {
                processed = exception_result();
            }
            if (!processed) {
                record_error(processed);
                break;
            }
            access_lost_attempts = 0;
        }
        running.store(false, std::memory_order_release);
    }

    void record_error(WgcResult error) noexcept {
        try {
            std::lock_guard lock(error_mutex);
            last_failure = std::move(error);
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

    HMONITOR monitor = nullptr;
    WgcCaptureOptions options{};
    WgcMailboxConfig mailbox_config{};
    WgcMailboxFrameInfo mailbox{};
    SharedFrameBusConfig bus_config{};
    std::shared_ptr<SharedFrameBusPublisherState> bus_state;
    std::uint64_t bus_producer_token = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> duplication;
    DXGI_OUTPUT_DESC output_description{};
    DXGI_OUTDUPL_DESC duplication_description{};
    GpuTransform transform;
    std::unique_ptr<RotatedDesktopTransform> rotated_transform;
    DXGI_COLOR_SPACE_TYPE bus_color = DXGI_COLOR_SPACE_CUSTOM;
    WgcRect surface_mailbox{};
    std::uint32_t surface_width = 0;
    std::uint32_t surface_height = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint64_t qpc_frequency = 0;
    std::uint64_t mailbox_generation = 1;
    std::uint64_t epoch = 1;
    std::uint64_t epoch_nonce = 1;
    std::uint64_t previous_bus_sequence = 0;
    WgcCursorShape current_cursor_shape{};
    POINT pointer_position{};
    std::uint32_t cursor_source_hotspot_x = 0;
    std::uint32_t cursor_source_hotspot_y = 0;
    std::uint64_t pointer_sample_qpc = 0;
    bool pointer_known = false;
    bool pointer_visible = false;
    bool pointer_position_estimated = false;
    bool cursor_source_hotspot_known = false;
    bool native_pointer_position_seen = false;
    bool native_pointer_shape_seen = false;
    HCURSOR last_win32_cursor = nullptr;
    std::uint64_t last_cursor_refresh_qpc = 0;
    bool bus_planar = false;
    bool rotated = false;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
    bool first_frame = true;
    bool force_full_next = true;
    WgcPixelFormat resolved_pixel_format = WgcPixelFormat::bgra8;
    WgcPixelFormat epoch_pixel_format = WgcPixelFormat::bgra8;
    bool first_session = true;
    DesktopDuplicationColorState color_state{};
    CursorCompositor cursor_compositor;
    bool cursor_compositor_ready = false;
    ComPtr<ID3D11Texture2D> cursor_intermediate;
    WgcCursorShape composite_shape{};
    ComPtr<ID3D11Texture2D> heartbeat_texture;
    SharedFrameBusFrameMetadata heartbeat_metadata{};
    bool heartbeat_metadata_valid = false;
    std::uint64_t last_publish_qpc = 0;
    internal::SessionEventMonitor session_events;

    mutable std::mutex lifecycle_mutex;
    mutable std::mutex error_mutex;
    std::jthread worker;
    WgcResult last_failure{};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> running{false};
    std::atomic<bool> target_closed_flag{false};
    std::atomic<std::uint64_t> received_frames{0};
    std::atomic<std::uint64_t> published_frames{0};
    std::atomic<std::uint64_t> skipped_no_buffer{0};
    std::atomic<std::uint64_t> dropped_at_source{0};
    std::atomic<std::uint64_t> ingress_copy_submissions{0};
    std::atomic<std::uint64_t> ingress_transform_submissions{0};
    std::atomic<std::uint64_t> native_damage_frames{0};
    std::atomic<std::uint64_t> full_damage_frames{0};
    std::atomic<std::uint64_t> cursor_metadata_frames{0};
    std::atomic<std::uint64_t> cursor_shape_updates{0};
    std::atomic<std::uint64_t> desktop_present_frames{0};
    std::atomic<std::uint64_t> session_rebuilds{0};
    std::atomic<std::uint64_t> recovery_attempts{0};
    std::atomic<std::uint64_t> recovery_successes{0};
    std::atomic<std::uint64_t> protected_content_frames{0};
    std::atomic<std::uint64_t> idle_republished_frames{0};
    std::atomic<std::uint64_t> session_event_notifications{0};
    std::atomic<std::uint64_t> format_fallbacks{0};
    std::atomic<std::uint64_t> cursor_composited_frames{0};
};

DesktopDuplicationCapture::DesktopDuplicationCapture(
    std::shared_ptr<DesktopDuplicationCaptureState> state) noexcept
    : state_(std::move(state)) {}

DesktopDuplicationCapture::~DesktopDuplicationCapture() { stop(); }

DesktopDuplicationCapture::DesktopDuplicationCapture(
    DesktopDuplicationCapture&&) noexcept = default;

DesktopDuplicationCapture& DesktopDuplicationCapture::operator=(
    DesktopDuplicationCapture&& other) noexcept {
    if (this != &other) {
        stop();
        state_ = std::move(other.state_);
    }
    return *this;
}

WgcResult DesktopDuplicationCapture::create_for_monitor_to_bus(
    HMONITOR monitor,
    SharedFrameBusPublisher& publisher,
    const WgcCaptureOptions& options,
    const WgcMailboxConfig& mailbox,
    DesktopDuplicationCapture& output) noexcept {
    try {
        if (publisher.state_ == nullptr) {
            return make_result(
                WgcStatus::invalid_argument,
                E_UNEXPECTED,
                "shared frame bus publisher is uninitialized");
        }
        auto state = std::make_shared<DesktopDuplicationCaptureState>();
        WgcResult initialized = state->initialize(
            monitor, publisher.state_, options, mailbox);
        if (!initialized) return initialized;
        output.stop();
        output = DesktopDuplicationCapture(std::move(state));
        return make_result(WgcStatus::ok);
    } catch (...) {
        return exception_result();
    }
}

WgcResult DesktopDuplicationCapture::start() noexcept {
    if (state_ == nullptr) {
        return make_result(WgcStatus::invalid_state, E_UNEXPECTED);
    }
    try {
        return state_->start();
    } catch (...) {
        return exception_result();
    }
}

void DesktopDuplicationCapture::stop() noexcept {
    if (state_ != nullptr) state_->stop();
}

WgcMailboxState DesktopDuplicationCapture::mailbox_state() const noexcept {
    if (state_ == nullptr) return {};
    return {
        state_->mailbox_config,
        state_->source_width,
        state_->source_height,
        state_->mailbox_generation,
        true};
}

ID3D11Device* DesktopDuplicationCapture::device() const noexcept {
    return state_ != nullptr ? state_->device.Get() : nullptr;
}

ID3D11DeviceContext* DesktopDuplicationCapture::context() const noexcept {
    return state_ != nullptr ? state_->context.Get() : nullptr;
}

bool DesktopDuplicationCapture::running() const noexcept {
    return state_ != nullptr
        && state_->running.load(std::memory_order_acquire);
}

bool DesktopDuplicationCapture::target_closed() const noexcept {
    return state_ != nullptr
        && state_->target_closed_flag.load(std::memory_order_acquire);
}

WgcCaptureStats DesktopDuplicationCapture::stats() const noexcept {
    if (state_ == nullptr) return {};
    WgcCaptureStats output;
    output.received_frames = state_->received_frames.load(
        std::memory_order_relaxed);
    output.published_frames = state_->published_frames.load(
        std::memory_order_relaxed);
    output.skipped_no_buffer = state_->skipped_no_buffer.load(
        std::memory_order_relaxed);
    output.dropped_at_source = state_->dropped_at_source.load(
        std::memory_order_relaxed);
    output.ingress_copy_submissions =
        state_->ingress_copy_submissions.load(std::memory_order_relaxed);
    output.ingress_transform_submissions =
        state_->ingress_transform_submissions.load(std::memory_order_relaxed);
    output.native_damage_frames = state_->native_damage_frames.load(
        std::memory_order_relaxed);
    output.full_damage_frames = state_->full_damage_frames.load(
        std::memory_order_relaxed);
    output.cursor_metadata_frames = state_->cursor_metadata_frames.load(
        std::memory_order_relaxed);
    output.cursor_shape_updates = state_->cursor_shape_updates.load(
        std::memory_order_relaxed);
    output.epoch = state_->epoch;
    output.epoch_nonce = state_->epoch_nonce;
    output.session_rebuilds = state_->session_rebuilds.load(
        std::memory_order_relaxed);
    output.recovery_attempts = state_->recovery_attempts.load(
        std::memory_order_relaxed);
    output.recovery_successes = state_->recovery_successes.load(
        std::memory_order_relaxed);
    output.protected_content_frames = state_->protected_content_frames.load(
        std::memory_order_relaxed);
    output.idle_republished_frames = state_->idle_republished_frames.load(
        std::memory_order_relaxed);
    output.session_events = state_->session_event_notifications.load(
        std::memory_order_relaxed);
    output.format_fallbacks = state_->format_fallbacks.load(
        std::memory_order_relaxed);
    return output;
}

std::uint64_t DesktopDuplicationCapture::desktop_present_frames() const noexcept {
    return state_ != nullptr
        ? state_->desktop_present_frames.load(std::memory_order_relaxed) : 0;
}

DesktopDuplicationColorState DesktopDuplicationCapture::desktop_color_state()
    const noexcept {
    if (state_ == nullptr) return {};
    return state_->color_state;
}

WgcResult DesktopDuplicationCapture::last_error() const noexcept {
    return state_ != nullptr
        ? state_->read_error()
        : make_result(WgcStatus::invalid_state, E_UNEXPECTED);
}

bool DesktopDuplicationCapture::initialized() const noexcept {
    return state_ != nullptr;
}

} // namespace fluxcap::gpu
