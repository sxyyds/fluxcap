#include "desktop_duplication_controller.hpp"
#include "desktop_duplication_capture.hpp"
#include "win32_cursor_shape.hpp"

#include <d3d10.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;

bool controller_planar_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010;
}

GpuPixelFormat controller_gpu_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return GpuPixelFormat::bgra8;
    case DXGI_FORMAT_NV12: return GpuPixelFormat::nv12;
    case DXGI_FORMAT_P010: return GpuPixelFormat::p010;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return GpuPixelFormat::rgba16_float;
    default: return GpuPixelFormat::bgra8;
    }
}

DXGI_FORMAT controller_capture_format(WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_B8G8R8A8_UNORM;
}

DXGI_COLOR_SPACE_TYPE controller_capture_color(
    WgcPixelFormat format) noexcept {
    return format == WgcPixelFormat::rgba16_float
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

DXGI_COLOR_SPACE_TYPE controller_bus_color(
    const SharedFrameBusConfig& config,
    WgcPixelFormat input) noexcept {
    if (config.color_space != DXGI_COLOR_SPACE_CUSTOM) {
        return config.color_space;
    }
    if (config.format == DXGI_FORMAT_P010
        && input == WgcPixelFormat::rgba16_float) {
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    }
    if (controller_planar_format(config.format)) {
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }
    return controller_capture_color(input);
}

bool controller_valid_options(const WgcCaptureOptions& options) noexcept {
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

WgcResult controller_gpu_result(const GpuError& error, const char* stage) {
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
    return make_controller_result(
        status,
        FAILED(error.hresult) ? error.hresult : E_FAIL,
        std::move(message));
}

WgcResult controller_exception_result() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return {WgcStatus::out_of_memory, E_OUTOFMEMORY, {}};
    } catch (const std::exception& error) {
        try {
            return make_controller_result(
                WgcStatus::capture_error, E_FAIL, error.what());
        } catch (...) {
            return {WgcStatus::capture_error, E_FAIL, {}};
        }
    } catch (...) {
        return {WgcStatus::capture_error, E_FAIL, {}};
    }
}

bool controller_rect_dimensions(
    const RECT& rect, std::uint32_t& width, std::uint32_t& height) noexcept {
    const std::int64_t w =
        static_cast<std::int64_t>(rect.right) - rect.left;
    const std::int64_t h =
        static_cast<std::int64_t>(rect.bottom) - rect.top;
    if (w <= 0 || h <= 0
        || w > std::numeric_limits<std::uint32_t>::max()
        || h > std::numeric_limits<std::uint32_t>::max()) {
        width = 0;
        height = 0;
        return false;
    }
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    return true;
}

// DXGI_ERROR_MORE_DATA resizing loop shared by move and dirty rectangles.
template <typename T>
bool controller_read_duplication_metadata(
    IDXGIOutputDuplication* duplication,
    UINT total_metadata_bytes,
    HRESULT (IDXGIOutputDuplication::*reader)(UINT, T*, UINT*),
    std::vector<T>& result) {
    result.clear();
    UINT capacity = total_metadata_bytes;
    UINT written = 0;
    HRESULT hr = S_OK;
    for (;;) {
        const std::size_t count = (
            static_cast<std::size_t>(capacity) + sizeof(T) - 1)
            / sizeof(T);
        result.resize(count);
        hr = (duplication->*reader)(
            static_cast<UINT>(count * sizeof(T)), result.data(), &written);
        if (hr != DXGI_ERROR_MORE_DATA) break;
        capacity = written;
    }
    if (FAILED(hr) || written % sizeof(T) != 0) {
        result.clear();
        return false;
    }
    result.resize(written / sizeof(T));
    return true;
}

} // namespace

WgcResult make_controller_result(
    WgcStatus status, HRESULT hresult, std::string message) {
    if (message.empty()) message = wgc_status_string(status);
    return {status, hresult, std::move(message)};
}

WgcResult DesktopDuplicationControllerState::initialize(
    const std::shared_ptr<SharedFrameBusPublisherState>& source_bus,
    const WgcCaptureOptions& source_options) {
    if (source_bus == nullptr || !controller_valid_options(source_options)) {
        return make_controller_result(
            WgcStatus::invalid_argument,
            E_INVALIDARG,
            "invalid Desktop Duplication controller configuration");
    }
    options = source_options;
    bus_state = source_bus;
    epoch = options.capture_epoch;
    epoch_nonce = options.capture_epoch_nonce;
    bus_config = internal::shared_frame_bus_config(bus_state);
    bus_planar = controller_planar_format(bus_config.format);
    device = internal::shared_frame_bus_device(bus_state);
    if (device == nullptr) {
        return make_controller_result(
            WgcStatus::invalid_argument,
            E_UNEXPECTED,
            "shared frame bus has no D3D11 device");
    }
    device->GetImmediateContext(&context);
    if (context == nullptr) {
        return make_controller_result(
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
        return make_controller_result(
            WgcStatus::capture_error,
            HRESULT_FROM_WIN32(GetLastError()),
            "performance counter frequency query failed");
    }
    qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    const GpuError reserved = internal::reserve_shared_frame_bus_producer(
        bus_state, bus_producer_token);
    if (!reserved) {
        return make_controller_result(
            WgcStatus::invalid_state,
            FAILED(reserved.hresult) ? reserved.hresult : E_UNEXPECTED,
            reserved.what());
    }
    previous_bus_sequence = internal::shared_frame_bus_sequence(bus_state);
    if (options.monitor_session_events) {
        session_events.start();
    }
    return create_sessions(nullptr);
}

bool DesktopDuplicationControllerState::wait_retry_delay(
    const std::stop_token* token, std::uint32_t ms) {
    std::uint32_t remaining = ms;
    while (remaining != 0) {
        if (token != nullptr && token->stop_requested()) return false;
        const DWORD slice = remaining < 10 ? remaining : 10;
        Sleep(slice);
        remaining -= slice;
    }
    return !(token != nullptr && token->stop_requested());
}

bool DesktopDuplicationControllerState::full_damage(
    WgcFrameDamage& damage) const {
    damage = {};
    damage.flags = wgc_damage_valid
        | wgc_damage_native
        | wgc_damage_native_move_available
        | wgc_damage_full_frame;
    if (first_frame) damage.flags |= wgc_damage_discontinuity;
    damage.dirty_count = 1;
    const std::uint32_t width =
        static_cast<std::uint32_t>(virtual_bounds.right - virtual_bounds.left);
    const std::uint32_t height =
        static_cast<std::uint32_t>(virtual_bounds.bottom - virtual_bounds.top);
    damage.dirty_rects[0] = {0, 0, width, height};
    return true;
}

WgcResult DesktopDuplicationControllerState::create_sessions(
    const std::stop_token* token) {
    monitors.clear();
    transform = GpuTransform{};
    compose_texture.Reset();

    // Resolve the bus adapter once; every aggregated output must live on it.
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> device_adapter;
    DXGI_ADAPTER_DESC device_adapter_description{};
    HRESULT hr = device.As(&dxgi_device);
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&device_adapter);
    if (SUCCEEDED(hr)) hr = device_adapter->GetDesc(&device_adapter_description);
    if (FAILED(hr)) {
        return make_controller_result(
            WgcStatus::d3d_error,
            hr,
            "failed to resolve the Desktop Duplication controller adapter");
    }
    const LUID adapter_luid = device_adapter_description.AdapterLuid;

    ComPtr<IDXGIFactory1> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return make_controller_result(
            WgcStatus::d3d_error, hr, "DXGI factory creation failed");
    }

    // Live color state from the first DXGI1.6 output decides HDR detection.
    color_state = {};
    bool desktop_hdr = false;
    bool color_probed = false;

    virtual_bounds = {0, 0, 0, 0};
    bool bounds_initialized = false;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) {
            return make_controller_result(
                WgcStatus::d3d_error, hr, "DXGI adapter enumeration failed");
        }
        DXGI_ADAPTER_DESC adapter_description{};
        hr = adapter->GetDesc(&adapter_description);
        if (FAILED(hr)) {
            return make_controller_result(
                WgcStatus::d3d_error, hr, "DXGI adapter description failed");
        }
        if (adapter_description.AdapterLuid.LowPart != adapter_luid.LowPart
            || adapter_description.AdapterLuid.HighPart
                != adapter_luid.HighPart) {
            continue;
        }
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> base_output;
            hr = adapter->EnumOutputs(output_index, &base_output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) {
                return make_controller_result(
                    WgcStatus::d3d_error, hr, "DXGI output enumeration failed");
            }
            DXGI_OUTPUT_DESC description{};
            hr = base_output->GetDesc(&description);
            if (FAILED(hr)) {
                return make_controller_result(
                    WgcStatus::d3d_error,
                    hr,
                    "DXGI output description failed");
            }
            if (!description.AttachedToDesktop) continue;
            if (description.Rotation != DXGI_MODE_ROTATION_IDENTITY
                && description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                return make_controller_result(
                    WgcStatus::not_supported,
                    DXGI_ERROR_UNSUPPORTED,
                    "rotated outputs are unsupported by the Desktop "
                    "Duplication controller");
            }
            if (!color_probed) {
                ComPtr<IDXGIOutput6> output6;
                if (SUCCEEDED(base_output.As(&output6))) {
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
                color_probed = true;
            }

            ControllerMonitorSession session;
            session.desktop_coordinates = description.DesktopCoordinates;
            if (!controller_rect_dimensions(
                    description.DesktopCoordinates,
                    session.width,
                    session.height)) {
                return make_controller_result(
                    WgcStatus::not_supported,
                    E_BOUNDS,
                    "an attached output reported invalid desktop coordinates");
            }
            ComPtr<IDXGIOutput1> output1;
            hr = base_output.As(&output1);
            if (FAILED(hr)) {
                return make_controller_result(
                    WgcStatus::not_supported,
                    hr,
                    "IDXGIOutput1 is unavailable for an attached output");
            }
            session.output = std::move(output1);
            monitors.push_back(std::move(session));
            if (!bounds_initialized) {
                virtual_bounds = description.DesktopCoordinates;
                bounds_initialized = true;
            } else {
                virtual_bounds.left =
                    std::min(virtual_bounds.left, description.DesktopCoordinates.left);
                virtual_bounds.top =
                    std::min(virtual_bounds.top, description.DesktopCoordinates.top);
                virtual_bounds.right = std::max(
                    virtual_bounds.right,
                    description.DesktopCoordinates.right);
                virtual_bounds.bottom = std::max(
                    virtual_bounds.bottom,
                    description.DesktopCoordinates.bottom);
            }
        }
    }
    if (monitors.empty()) {
        target_closed_flag.store(true, std::memory_order_release);
        return make_controller_result(
            WgcStatus::target_closed,
            DXGI_ERROR_NOT_FOUND,
            "no desktop-attached outputs on the bus adapter");
    }
    if (virtual_bounds.left < 0 || virtual_bounds.top < 0) {
        return make_controller_result(
            WgcStatus::not_supported,
            E_BOUNDS,
            "virtual desktop origins must be non-negative");
    }

    resolved_pixel_format =
        internal::desktop_duplication_resolve_pixel_format(
            options.auto_detect_color, desktop_hdr, options.pixel_format);
    bus_color = controller_bus_color(bus_config, resolved_pixel_format);
    const DXGI_FORMAT primary_format =
        controller_capture_format(resolved_pixel_format);
    DXGI_FORMAT negotiation_formats[2]{};
    const UINT format_count =
        internal::desktop_duplication_format_negotiation(
            primary_format,
            options.allow_format_fallback,
            negotiation_formats);

    for (auto& session : monitors) {
        for (UINT attempt = 0;; ++attempt) {
            ComPtr<IDXGIOutput5> output5;
            HRESULT duplicate_hr = session.output.As(&output5);
            if (SUCCEEDED(duplicate_hr)) {
                duplicate_hr = output5->DuplicateOutput1(
                    device.Get(),
                    0,
                    format_count,
                    negotiation_formats,
                    &session.duplication);
            }
            if (SUCCEEDED(duplicate_hr)) break;
            if (duplicate_hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
                && (options.session_retry_limit == 0
                    || attempt < options.session_retry_limit)
                && wait_retry_delay(
                    token, options.session_retry_interval_ms)) {
                continue;
            }
            return make_controller_result(
                duplicate_hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
                    || duplicate_hr == DXGI_ERROR_UNSUPPORTED
                    || duplicate_hr == E_NOINTERFACE
                    ? WgcStatus::not_supported
                    : WgcStatus::d3d_error,
                duplicate_hr,
                "controller DuplicateOutput1 failed");
        }
        DXGI_OUTDUPL_DESC duplication_description{};
        session.duplication->GetDesc(&duplication_description);
        if (duplication_description.ModeDesc.Width != session.width
            || duplication_description.ModeDesc.Height != session.height) {
            return make_controller_result(
                WgcStatus::capture_error,
                E_UNEXPECTED,
                "controller duplication dimensions disagree with the output");
        }
        if (duplication_description.ModeDesc.Format != primary_format) {
            if (internal::desktop_duplication_format_fallback_accepted(
                    duplication_description.ModeDesc.Format,
                    primary_format,
                    options.allow_format_fallback)) {
                resolved_pixel_format = WgcPixelFormat::bgra8;
                bus_color = controller_bus_color(
                    bus_config, resolved_pixel_format);
                format_fallbacks.fetch_add(1, std::memory_order_relaxed);
            } else {
                return make_controller_result(
                    WgcStatus::not_supported,
                    DXGI_ERROR_UNSUPPORTED,
                    "controller did not receive the negotiated pixel format");
            }
        }
    }

    const std::uint32_t virtual_width =
        static_cast<std::uint32_t>(virtual_bounds.right - virtual_bounds.left);
    const std::uint32_t virtual_height =
        static_cast<std::uint32_t>(
            virtual_bounds.bottom - virtual_bounds.top);
    if (!bus_planar
        && (virtual_width != bus_config.width
            || virtual_height != bus_config.height
            || bus_config.format
                != controller_capture_format(resolved_pixel_format)
            || bus_color
                != controller_capture_color(resolved_pixel_format))) {
        return make_controller_result(
            WgcStatus::invalid_argument,
            E_INVALIDARG,
            "controller direct bus format/dimensions do not match the "
            "virtual desktop");
    }
    if (bus_planar && ((bus_config.width | bus_config.height) & 1u) != 0) {
        return make_controller_result(
            WgcStatus::invalid_argument,
            E_INVALIDARG,
            "planar controller bus must be even");
    }

    D3D11_TEXTURE2D_DESC compose_description{};
    compose_description.Width = virtual_width;
    compose_description.Height = virtual_height;
    compose_description.MipLevels = 1;
    compose_description.ArraySize = 1;
    compose_description.Format =
        controller_capture_format(resolved_pixel_format);
    compose_description.SampleDesc.Count = 1;
    compose_description.Usage = D3D11_USAGE_DEFAULT;
    compose_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = device->CreateTexture2D(
        &compose_description, nullptr, &compose_texture);
    if (FAILED(hr)) {
        return make_controller_result(
            WgcStatus::d3d_error,
            hr,
            "controller compose texture creation failed");
    }
    if (bus_planar) {
        GpuTransformConfig transform_config;
        transform_config.input_width = virtual_width;
        transform_config.input_height = virtual_height;
        transform_config.input_region_x = 0;
        transform_config.input_region_y = 0;
        transform_config.input_region_width = virtual_width;
        transform_config.input_region_height = virtual_height;
        transform_config.output_width = bus_config.width;
        transform_config.output_height = bus_config.height;
        transform_config.output_format =
            controller_gpu_format(bus_config.format);
        transform_config.input_format =
            controller_capture_format(resolved_pixel_format);
        transform_config.input_color_space =
            controller_capture_color(resolved_pixel_format);
        transform_config.output_color_space = bus_color;
        transform_config.backend = options.planar_transform_backend;
        transform_config.external_output_only = true;
        const GpuError transformed = GpuTransform::create(
            device.Get(), transform_config, transform);
        if (!transformed) {
            return controller_gpu_result(
                transformed,
                "controller planar transform creation failed");
        }
    }

    first_frame = true;
    if (first_session) {
        first_session = false;
    } else {
        session_rebuilds.fetch_add(1, std::memory_order_relaxed);
        recovery_successes.fetch_add(1, std::memory_order_relaxed);
    }
    return make_controller_result(WgcStatus::ok);
}

WgcResult DesktopDuplicationControllerState::rebuild_topology(
    std::stop_token& token) {
    recovery_attempts.fetch_add(1, std::memory_order_relaxed);
    const RECT previous_bounds = virtual_bounds;
    const std::size_t previous_count = monitors.size();
    if (!wait_retry_delay(
            &token,
            internal::desktop_duplication_backoff_delay_ms(
                1,
                options.access_lost_retry_initial_ms,
                options.access_lost_retry_max_ms))) {
        return make_controller_result(WgcStatus::ok);
    }
    WgcResult rebuilt;
    try {
        rebuilt = create_sessions(&token);
    } catch (...) {
        rebuilt = controller_exception_result();
    }
    if (!rebuilt) return rebuilt;
    if (previous_count != monitors.size()
        || previous_bounds.left != virtual_bounds.left
        || previous_bounds.top != virtual_bounds.top
        || previous_bounds.right != virtual_bounds.right
        || previous_bounds.bottom != virtual_bounds.bottom) {
        topology_changes.fetch_add(1, std::memory_order_relaxed);
        return make_controller_result(
            WgcStatus::not_supported,
            DXGI_ERROR_UNSUPPORTED,
            "virtual desktop topology changed; rebuild the capture epoch");
    }
    first_frame = true;
    return make_controller_result(WgcStatus::ok);
}

WgcResult DesktopDuplicationControllerState::build_cursor(
    WgcCursorInfo& cursor, WgcCursorShape& pending) {
    cursor = {};
    pending = {};
    if (!options.include_cursor_metadata) {
        return make_controller_result(WgcStatus::ok);
    }
    CURSORINFO win32_cursor{sizeof(win32_cursor)};
    if (!GetCursorInfo(&win32_cursor)) {
        return make_controller_result(WgcStatus::ok);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const bool visible = (win32_cursor.flags & CURSOR_SHOWING) != 0;
    const std::int64_t local_x =
        static_cast<std::int64_t>(win32_cursor.ptScreenPos.x)
        - virtual_bounds.left;
    const std::int64_t local_y =
        static_cast<std::int64_t>(win32_cursor.ptScreenPos.y)
        - virtual_bounds.top;
    if (current_cursor_shape.sequence == 0
        || win32_cursor.hCursor != last_win32_cursor) {
        WgcCursorShape sampled;
        if (internal::extract_win32_cursor_shape(
                win32_cursor.hCursor, sampled)) {
            internal::SideDataGeometry geometry;
            geometry.source_width =
                static_cast<std::uint32_t>(
                    virtual_bounds.right - virtual_bounds.left);
            geometry.source_height =
                static_cast<std::uint32_t>(
                    virtual_bounds.bottom - virtual_bounds.top);
            geometry.output_width = bus_config.width;
            geometry.output_height = bus_config.height;
            geometry.planar_420 = bus_planar;
            WgcCursorShape scaled;
            if (internal::scale_cursor_shape(sampled, geometry, scaled)) {
                pending = std::move(scaled);
            }
        }
        last_win32_cursor = win32_cursor.hCursor;
    }
    std::int32_t frame_x = 0;
    std::int32_t frame_y = 0;
    const std::uint32_t virtual_width =
        static_cast<std::uint32_t>(virtual_bounds.right - virtual_bounds.left);
    const std::uint32_t virtual_height =
        static_cast<std::uint32_t>(
            virtual_bounds.bottom - virtual_bounds.top);
    bool position_valid = visible
        && local_x >= std::numeric_limits<std::int32_t>::min()
        && local_x <= std::numeric_limits<std::int32_t>::max()
        && local_y >= std::numeric_limits<std::int32_t>::min()
        && local_y <= std::numeric_limits<std::int32_t>::max()
        && internal::scale_signed_coordinate_nearest(
            static_cast<std::int32_t>(local_x),
            virtual_width,
            bus_config.width,
            frame_x)
        && internal::scale_signed_coordinate_nearest(
            static_cast<std::int32_t>(local_y),
            virtual_height,
            bus_config.height,
            frame_y);
    if (visible) cursor.flags |= wgc_cursor_visible;
    if (position_valid) {
        cursor.flags |= wgc_cursor_position_valid
            | wgc_cursor_position_estimated;
        cursor.sample_qpc = static_cast<std::uint64_t>(now.QuadPart);
        cursor.screen_x = win32_cursor.ptScreenPos.x;
        cursor.screen_y = win32_cursor.ptScreenPos.y;
        cursor.frame_x = frame_x;
        cursor.frame_y = frame_y;
    }
    const WgcCursorShape* shape = pending.sequence != 0
        ? &pending
        : current_cursor_shape.sequence != 0
        ? &current_cursor_shape
        : nullptr;
    if (shape != nullptr) {
        cursor.shape_sequence = shape->sequence;
        cursor.width = shape->width;
        cursor.height = shape->height;
        cursor.hotspot_x = shape->hotspot_x;
        cursor.hotspot_y = shape->hotspot_y;
        if (pending.sequence != 0) {
            cursor.flags |= wgc_cursor_shape_pending;
        }
    }
    return make_controller_result(WgcStatus::ok);
}

WgcResult DesktopDuplicationControllerState::publish_compose(bool any_frame) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const std::uint32_t virtual_width =
        static_cast<std::uint32_t>(virtual_bounds.right - virtual_bounds.left);
    const std::uint32_t virtual_height =
        static_cast<std::uint32_t>(
            virtual_bounds.bottom - virtual_bounds.top);

    SharedFrameBusWriteLease lease;
    const GpuError begun = internal::begin_shared_frame_bus_publish(
        bus_state, bus_producer_token, lease);
    if (!begun) {
        if (begun.status == GpuStatus::timeout) {
            skipped_no_buffer.fetch_add(1, std::memory_order_relaxed);
            first_frame = true;
            return make_controller_result(WgcStatus::ok);
        }
        return controller_gpu_result(
            begun, "controller bus lease failed");
    }

    if (bus_planar) {
        const GpuError converted = transform.process_into(
            compose_texture.Get(), lease.texture());
        if (!converted) {
            return controller_gpu_result(
                converted, "controller planar conversion failed");
        }
        ingress_transform_submissions.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        context->CopySubresourceRegion(
            lease.texture(), 0, 0, 0, 0, compose_texture.Get(), 0, nullptr);
        ingress_copy_submissions.fetch_add(1, std::memory_order_relaxed);
    }

    WgcFrameDamage damage;
    const bool missing_content = std::any_of(
        monitors.begin(), monitors.end(), [](const auto& session) {
            return !session.has_content;
        });
    if (first_frame || missing_content) {
        (void)full_damage(damage);
    } else if (!any_frame) {
        // Heartbeat republish of identical content: valid but empty.
        damage = {};
        damage.flags = wgc_damage_valid;
    } else if (pending_damage_overflow) {
        (void)full_damage(damage);
        damage.flags |= wgc_damage_overflow;
    } else {
        damage = pending_damage;
    }
    pending_damage = {};
    pending_damage_overflow = false;
    WgcCursorInfo cursor;
    WgcCursorShape pending_shape;
    const WgcResult built = build_cursor(cursor, pending_shape);
    if (!built) return built;

    SharedFrameBusFrameMetadata metadata;
    metadata.valid_fields = shared_frame_bus_metadata_qpc
        | shared_frame_bus_metadata_source_dimensions
        | shared_frame_bus_metadata_roi
        | shared_frame_bus_metadata_mailbox_generation
        | shared_frame_bus_metadata_color_space;
    metadata.timestamp_qpc = static_cast<std::uint64_t>(now.QuadPart);
    metadata.qpc_frequency = qpc_frequency;
    metadata.mailbox_generation = 1;
    metadata.source_width = virtual_width;
    metadata.source_height = virtual_height;
    metadata.roi_x = 0;
    metadata.roi_y = 0;
    metadata.roi_width = virtual_width;
    metadata.roi_height = virtual_height;
    metadata.color_space = bus_color;
    SharedFrameBusFrameSideData side_data;
    side_data.epoch = epoch;
    side_data.epoch_nonce = epoch_nonce;
    side_data.damage = damage;
    side_data.cursor = cursor;

    const std::uint64_t committed_sequence = lease.sequence();
    const GpuError committed = internal::commit_shared_frame_bus_publish(
        bus_state,
        bus_producer_token,
        std::move(lease),
        metadata,
        side_data);
    if (!committed) {
        return controller_gpu_result(
            committed, "controller bus commit failed");
    }
    if (pending_shape.sequence != 0) {
        const GpuError published_shape =
            internal::publish_shared_frame_bus_cursor_shape(
                bus_state, bus_producer_token, pending_shape);
        if (!published_shape) {
            return controller_gpu_result(
                published_shape,
                "controller cursor shape publish failed");
        }
        current_cursor_shape = std::move(pending_shape);
        cursor_shape_updates.fetch_add(1, std::memory_order_relaxed);
    }
    heartbeat_metadata = metadata;
    heartbeat_metadata_valid = true;
    last_publish_qpc = static_cast<std::uint64_t>(now.QuadPart);
    previous_bus_sequence = committed_sequence;
    first_frame = false;
    published_frames.fetch_add(1, std::memory_order_relaxed);
    if ((damage.flags & wgc_damage_native) != 0) {
        native_damage_frames.fetch_add(1, std::memory_order_relaxed);
    }
    if ((damage.flags & wgc_damage_full_frame) != 0) {
        full_damage_frames.fetch_add(1, std::memory_order_relaxed);
    }
    if (options.include_cursor_metadata) {
        cursor_metadata_frames.fetch_add(1, std::memory_order_relaxed);
    }
    return make_controller_result(WgcStatus::ok);
}

WgcResult DesktopDuplicationControllerState::publish_heartbeat() {
    if (options.idle_republish_interval_ms == 0
        || !heartbeat_metadata_valid) {
        return make_controller_result(WgcStatus::ok);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (!internal::desktop_duplication_heartbeat_due(
            last_publish_qpc,
            static_cast<std::uint64_t>(now.QuadPart),
            qpc_frequency,
            options.idle_republish_interval_ms)) {
        return make_controller_result(WgcStatus::ok);
    }
    const WgcResult published = publish_compose(false);
    if (!published) return published;
    idle_republished_frames.fetch_add(1, std::memory_order_relaxed);
    return make_controller_result(WgcStatus::ok);
}

WgcResult DesktopDuplicationControllerState::process_iteration(
    std::stop_token& token) {
    const std::uint32_t per_output_timeout = std::max<std::uint32_t>(
        1, options.frame_timeout_ms
            / static_cast<std::uint32_t>(monitors.size()));
    bool any_frame = false;
    pending_damage = {};
    pending_damage.flags = wgc_damage_valid | wgc_damage_native;
    pending_damage_overflow = false;
    const std::uint32_t virtual_width =
        static_cast<std::uint32_t>(virtual_bounds.right - virtual_bounds.left);
    const std::uint32_t virtual_height =
        static_cast<std::uint32_t>(
            virtual_bounds.bottom - virtual_bounds.top);

    for (auto& session : monitors) {
        DXGI_OUTDUPL_FRAME_INFO frame{};
        ComPtr<IDXGIResource> resource;
        const HRESULT acquired = session.duplication->AcquireNextFrame(
            per_output_timeout, &frame, &resource);
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (acquired == DXGI_ERROR_ACCESS_LOST) {
            return rebuild_topology(token);
        }
        if (FAILED(acquired)) {
            return make_controller_result(
                WgcStatus::d3d_error,
                acquired,
                "controller frame acquisition failed");
        }
        struct FrameRelease final {
            IDXGIOutputDuplication* duplication;
            ~FrameRelease() {
                if (duplication != nullptr) (void)duplication->ReleaseFrame();
            }
        } release{session.duplication.Get()};
        received_frames.fetch_add(1, std::memory_order_relaxed);
        if (frame.AccumulatedFrames > 1) {
            dropped_at_source.fetch_add(
                frame.AccumulatedFrames - 1, std::memory_order_relaxed);
            first_frame = true;
        }
        if (frame.ProtectedContentMaskedOut) {
            protected_content_frames.fetch_add(
                1, std::memory_order_relaxed);
        }
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT queried = resource.As(&texture);
        if (FAILED(queried)) {
            return make_controller_result(
                WgcStatus::d3d_error,
                queried,
                "controller duplication resource is not a D3D11 texture");
        }
        D3D11_TEXTURE2D_DESC texture_description{};
        texture->GetDesc(&texture_description);
        if (texture_description.Width != session.width
            || texture_description.Height != session.height
            || texture_description.Format
                != controller_capture_format(resolved_pixel_format)) {
            return make_controller_result(
                WgcStatus::d3d_error,
                E_UNEXPECTED,
                "controller texture changed its epoch contract");
        }
        context->CopySubresourceRegion(
            compose_texture.Get(),
            0,
            session.desktop_coordinates.left - virtual_bounds.left,
            session.desktop_coordinates.top - virtual_bounds.top,
            0,
            texture.Get(),
            0,
            nullptr);
        ingress_copy_submissions.fetch_add(1, std::memory_order_relaxed);
        session.has_content = true;
        any_frame = true;

        // Accumulate native dirty rectangles and move destinations into
        // virtual desktop coordinates. The compose texture already holds
        // post-move pixels, so moves only need their destinations reported.
        const auto accumulate_local_rect = [&](const RECT& local) {
            const std::int64_t width =
                static_cast<std::int64_t>(local.right) - local.left;
            const std::int64_t height =
                static_cast<std::int64_t>(local.bottom) - local.top;
            if (local.left < 0 || local.top < 0 || width <= 0 || height <= 0) {
                pending_damage_overflow = true;
                return;
            }
            WgcRect virtual_rect{};
            if (!internal::desktop_duplication_offset_rect(
                    WgcRect{
                        local.left,
                        local.top,
                        static_cast<std::uint32_t>(width),
                        static_cast<std::uint32_t>(height)},
                    session.desktop_coordinates.left - virtual_bounds.left,
                    session.desktop_coordinates.top - virtual_bounds.top,
                    virtual_rect)) {
                pending_damage_overflow = true;
                return;
            }
            // Clip to the owning monitor so damage never crosses outputs.
            WgcRect mapped;
            if (!internal::desktop_duplication_scale_rect(
                    virtual_rect,
                    virtual_width,
                    virtual_height,
                    bus_config.width,
                    bus_config.height,
                    bus_planar,
                    mapped)
                || pending_damage.dirty_count >= wgc_max_dirty_rects) {
                pending_damage_overflow = true;
                return;
            }
            pending_damage.dirty_rects[pending_damage.dirty_count++] =
                mapped;
        };
        std::vector<DXGI_OUTDUPL_MOVE_RECT> frame_moves;
        std::vector<RECT> frame_dirty;
        if (controller_read_duplication_metadata(
                session.duplication.Get(),
                frame.TotalMetadataBufferSize,
                &IDXGIOutputDuplication::GetFrameMoveRects,
                frame_moves)) {
            for (const auto& move : frame_moves) {
                accumulate_local_rect(move.DestinationRect);
            }
        } else {
            pending_damage_overflow = true;
        }
        if (controller_read_duplication_metadata(
                session.duplication.Get(),
                frame.TotalMetadataBufferSize,
                &IDXGIOutputDuplication::GetFrameDirtyRects,
                frame_dirty)) {
            for (const auto& dirty : frame_dirty) {
                accumulate_local_rect(dirty);
            }
        } else {
            pending_damage_overflow = true;
        }
    }
    if (!any_frame) {
        return publish_heartbeat();
    }
    return publish_compose(true);
}

WgcResult DesktopDuplicationControllerState::start() {
    std::lock_guard lock(lifecycle_mutex);
    if (started.load(std::memory_order_acquire)
        || stopped.load(std::memory_order_acquire)
        || compose_texture == nullptr || bus_producer_token == 0) {
        return make_controller_result(
            WgcStatus::invalid_state,
            E_UNEXPECTED,
            "Desktop Duplication controller cannot be started");
    }
    try {
        started.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        worker = std::jthread(
            [this](std::stop_token token) noexcept {
                worker_main(token);
            });
        return make_controller_result(WgcStatus::ok);
    } catch (...) {
        running.store(false, std::memory_order_release);
        started.store(false, std::memory_order_release);
        return controller_exception_result();
    }
}

void DesktopDuplicationControllerState::stop() noexcept {
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
    monitors.clear();
    compose_texture.Reset();
    transform = GpuTransform{};
    session_events.stop();
    if (bus_state != nullptr && bus_producer_token != 0) {
        internal::release_shared_frame_bus_producer(
            bus_state, bus_producer_token);
        bus_producer_token = 0;
    }
    bus_state.reset();
}

void DesktopDuplicationControllerState::worker_main(
    std::stop_token token) noexcept {
    std::uint64_t observed_session_generation = session_events.generation();
    while (!token.stop_requested()) {
        if (options.monitor_session_events) {
            const std::uint64_t generation = session_events.generation();
            if (generation != observed_session_generation) {
                observed_session_generation = generation;
                session_event_notifications.fetch_add(
                    1, std::memory_order_relaxed);
                first_frame = true;
                last_win32_cursor = nullptr;
            }
        }
        WgcResult processed;
        try {
            processed = process_iteration(token);
        } catch (...) {
            processed = controller_exception_result();
        }
        if (!processed) {
            record_error(processed);
            break;
        }
    }
    running.store(false, std::memory_order_release);
}

void DesktopDuplicationControllerState::record_error(
    WgcResult error) noexcept {
    try {
        std::lock_guard lock(error_mutex);
        last_failure = std::move(error);
    } catch (...) {
    }
}

} // namespace fluxcap::gpu

namespace fluxcap::gpu {

DesktopDuplicationController::DesktopDuplicationController(
    std::shared_ptr<DesktopDuplicationControllerState> state) noexcept
    : state_(std::move(state)) {}

DesktopDuplicationController::~DesktopDuplicationController() { stop(); }

DesktopDuplicationController::DesktopDuplicationController(
    DesktopDuplicationController&&) noexcept = default;

DesktopDuplicationController& DesktopDuplicationController::operator=(
    DesktopDuplicationController&& other) noexcept {
    if (this != &other) {
        stop();
        state_ = std::move(other.state_);
    }
    return *this;
}

WgcResult DesktopDuplicationController::create_for_bus(
    SharedFrameBusPublisher& publisher,
    const WgcCaptureOptions& options,
    DesktopDuplicationController& output) noexcept {
    try {
        if (publisher.state_ == nullptr) {
            return make_controller_result(
                WgcStatus::invalid_argument,
                E_UNEXPECTED,
                "shared frame bus publisher is uninitialized");
        }
        auto state = std::make_shared<DesktopDuplicationControllerState>();
        const WgcResult initialized =
            state->initialize(publisher.state_, options);
        if (!initialized) return initialized;
        output.stop();
        output = DesktopDuplicationController(std::move(state));
        return make_controller_result(WgcStatus::ok);
    } catch (...) {
        return controller_exception_result();
    }
}

WgcResult DesktopDuplicationController::start() noexcept {
    if (state_ == nullptr) {
        return make_controller_result(
            WgcStatus::invalid_state, E_UNEXPECTED);
    }
    try {
        return state_->start();
    } catch (...) {
        return controller_exception_result();
    }
}

void DesktopDuplicationController::stop() noexcept {
    if (state_ != nullptr) state_->stop();
}

ID3D11Device* DesktopDuplicationController::device() const noexcept {
    return state_ != nullptr ? state_->device.Get() : nullptr;
}

ID3D11DeviceContext* DesktopDuplicationController::context() const noexcept {
    return state_ != nullptr ? state_->context.Get() : nullptr;
}

bool DesktopDuplicationController::running() const noexcept {
    return state_ != nullptr
        && state_->running.load(std::memory_order_acquire);
}

bool DesktopDuplicationController::target_closed() const noexcept {
    return state_ != nullptr
        && state_->target_closed_flag.load(std::memory_order_acquire);
}

WgcCaptureStats DesktopDuplicationController::stats() const noexcept {
    if (state_ == nullptr) return {};
    WgcCaptureStats output;
    output.received_frames =
        state_->received_frames.load(std::memory_order_relaxed);
    output.published_frames =
        state_->published_frames.load(std::memory_order_relaxed);
    output.skipped_no_buffer =
        state_->skipped_no_buffer.load(std::memory_order_relaxed);
    output.dropped_at_source =
        state_->dropped_at_source.load(std::memory_order_relaxed);
    output.size_changes =
        state_->topology_changes.load(std::memory_order_relaxed);
    output.ingress_copy_submissions =
        state_->ingress_copy_submissions.load(std::memory_order_relaxed);
    output.ingress_transform_submissions =
        state_->ingress_transform_submissions.load(std::memory_order_relaxed);
    output.native_damage_frames =
        state_->native_damage_frames.load(std::memory_order_relaxed);
    output.full_damage_frames =
        state_->full_damage_frames.load(std::memory_order_relaxed);
    output.cursor_metadata_frames =
        state_->cursor_metadata_frames.load(std::memory_order_relaxed);
    output.cursor_shape_updates =
        state_->cursor_shape_updates.load(std::memory_order_relaxed);
    output.recovery_attempts =
        state_->recovery_attempts.load(std::memory_order_relaxed);
    output.recovery_successes =
        state_->recovery_successes.load(std::memory_order_relaxed);
    output.epoch = state_->epoch;
    output.epoch_nonce = state_->epoch_nonce;
    output.session_rebuilds =
        state_->session_rebuilds.load(std::memory_order_relaxed);
    output.protected_content_frames =
        state_->protected_content_frames.load(std::memory_order_relaxed);
    output.idle_republished_frames =
        state_->idle_republished_frames.load(std::memory_order_relaxed);
    output.session_events =
        state_->session_event_notifications.load(std::memory_order_relaxed);
    output.format_fallbacks =
        state_->format_fallbacks.load(std::memory_order_relaxed);
    return output;
}

std::uint32_t DesktopDuplicationController::monitor_count() const noexcept {
    return state_ != nullptr
        ? static_cast<std::uint32_t>(state_->monitors.size())
        : 0;
}

DesktopDuplicationColorState
DesktopDuplicationController::desktop_color_state() const noexcept {
    if (state_ == nullptr) return {};
    return state_->color_state;
}

WgcResult DesktopDuplicationController::last_error() const noexcept {
    if (state_ == nullptr) {
        return make_controller_result(
            WgcStatus::invalid_state, E_UNEXPECTED);
    }
    try {
        std::lock_guard lock(state_->error_mutex);
        return state_->last_failure;
    } catch (...) {
        return {WgcStatus::capture_error, E_FAIL, {}};
    }
}

bool DesktopDuplicationController::initialized() const noexcept {
    return state_ != nullptr;
}

} // namespace fluxcap::gpu
