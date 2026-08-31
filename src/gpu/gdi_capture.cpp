#include "gdi_capture.hpp"
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

bool gdi_valid_mailbox(const WgcMailboxConfig& mailbox) noexcept {
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

bool gdi_resolve_mailbox(
    const WgcMailboxConfig& config,
    std::uint32_t source_width,
    std::uint32_t source_height,
    WgcMailboxFrameInfo& output) noexcept {
    output = {};
    output.source_width = source_width;
    output.source_height = source_height;
    output.generation = 1;
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

WgcResult gdi_exception_result() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return {WgcStatus::out_of_memory, E_OUTOFMEMORY, {}};
    } catch (const std::exception& error) {
        try {
            return make_gdi_result(
                WgcStatus::capture_error, E_FAIL, error.what());
        } catch (...) {
            return {WgcStatus::capture_error, E_FAIL, {}};
        }
    } catch (...) {
        return {WgcStatus::capture_error, E_FAIL, {}};
    }
}

WgcResult gdi_gpu_result(const GpuError& error, const char* stage) {
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
    return make_gdi_result(
        status,
        FAILED(error.hresult) ? error.hresult : E_FAIL,
        std::move(message));
}

} // namespace

WgcResult make_gdi_result(
    WgcStatus status, HRESULT hresult, std::string message) {
    if (message.empty()) message = wgc_status_string(status);
    return {status, hresult, std::move(message)};
}

WgcResult GdiMonitorCaptureState::initialize(
    HMONITOR source_monitor,
    const std::shared_ptr<SharedFrameBusPublisherState>& source_bus,
    const WgcCaptureOptions& source_options,
    const WgcMailboxConfig& source_mailbox) {
    if (source_monitor == nullptr || source_bus == nullptr
        || source_options.pixel_format != WgcPixelFormat::bgra8
        || source_options.buffer_count < 2
        || source_options.buffer_count > 16
        || source_options.gdi_poll_interval_ms == 0
        || source_options.gdi_poll_interval_ms > 1'000
        || !gdi_valid_mailbox(source_mailbox)) {
        return make_gdi_result(
            WgcStatus::invalid_argument,
            E_INVALIDARG,
            "invalid GDI capture configuration");
    }
    if (source_options.require_border
        || source_options.include_secondary_windows
        || source_options.min_update_interval_us != 0
        || source_options.auto_detect_color
        || source_options.allow_format_fallback
        || source_options.damage_mode == WgcDamageMode::native_report_only
        || source_options.damage_mode
            == WgcDamageMode::native_with_inferred_moves) {
        return make_gdi_result(
            WgcStatus::not_supported,
            DXGI_ERROR_UNSUPPORTED,
            "GDI capture cannot provide native damage or WGC-only options");
    }

    MONITORINFOEXW monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoW(source_monitor, &monitor_info)) {
        return make_gdi_result(
            WgcStatus::not_supported,
            HRESULT_FROM_WIN32(GetLastError()),
            "GetMonitorInfoW failed for the requested monitor");
    }
    const std::int64_t width = static_cast<std::int64_t>(
        monitor_info.rcMonitor.right - monitor_info.rcMonitor.left);
    const std::int64_t height = static_cast<std::int64_t>(
        monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top);
    if (width <= 0 || height <= 0
        || width > std::numeric_limits<std::int32_t>::max()
        || height > std::numeric_limits<std::int32_t>::max()) {
        return make_gdi_result(
            WgcStatus::not_supported,
            E_BOUNDS,
            "monitor dimensions are not capturable through GDI");
    }

    monitor = source_monitor;
    options = source_options;
    mailbox_config = source_mailbox;
    epoch = options.capture_epoch;
    epoch_nonce = options.capture_epoch_nonce;
    if (epoch == 0 || epoch_nonce == 0) {
        return make_gdi_result(
            WgcStatus::invalid_argument,
            E_INVALIDARG,
            "GDI capture requires a non-zero epoch");
    }
    bus_state = source_bus;
    bus_config = internal::shared_frame_bus_config(bus_state);
    if (bus_config.format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return make_gdi_result(
            WgcStatus::invalid_argument,
            E_INVALIDARG,
            "GDI capture requires a BGRA8 shared frame bus");
    }
    if (bus_config.color_space != DXGI_COLOR_SPACE_CUSTOM
        && bus_config.color_space
            != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        return make_gdi_result(
            WgcStatus::invalid_argument,
            E_INVALIDARG,
            "GDI capture requires an SDR BT.709 shared frame bus");
    }
    if (!gdi_resolve_mailbox(
            mailbox_config,
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            mailbox)) {
        return make_gdi_result(
            WgcStatus::region_unavailable,
            E_BOUNDS,
            "GDI mailbox is outside the monitor bounds");
    }
    if (mailbox.width != bus_config.width
        || mailbox.height != bus_config.height) {
        return make_gdi_result(
            WgcStatus::invalid_argument,
            E_INVALIDARG,
            "GDI capture cannot scale; mailbox dimensions must match the bus");
    }
    capture_rect = {
        monitor_info.rcMonitor.left + static_cast<LONG>(mailbox.x),
        monitor_info.rcMonitor.top + static_cast<LONG>(mailbox.y),
        monitor_info.rcMonitor.left + static_cast<LONG>(mailbox.x)
            + static_cast<LONG>(mailbox.width),
        monitor_info.rcMonitor.top + static_cast<LONG>(mailbox.y)
            + static_cast<LONG>(mailbox.height)};
    capture_width = mailbox.width;
    capture_height = mailbox.height;

    device = internal::shared_frame_bus_device(bus_state);
    if (device == nullptr) {
        return make_gdi_result(
            WgcStatus::invalid_argument,
            E_UNEXPECTED,
            "shared frame bus has no D3D11 device");
    }
    device->GetImmediateContext(&context);
    if (context == nullptr) {
        return make_gdi_result(
            WgcStatus::d3d_error,
            E_FAIL,
            "D3D11 device has no immediate context");
    }
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread))) {
        (void)multithread->SetMultithreadProtected(TRUE);
    }
    D3D11_TEXTURE2D_DESC staging_description{};
    staging_description.Width = capture_width;
    staging_description.Height = capture_height;
    staging_description.MipLevels = 1;
    staging_description.ArraySize = 1;
    staging_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_description.SampleDesc.Count = 1;
    staging_description.Usage = D3D11_USAGE_DYNAMIC;
    staging_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const HRESULT created = device->CreateTexture2D(
        &staging_description, nullptr, &staging_texture);
    if (FAILED(created)) {
        return make_gdi_result(
            WgcStatus::d3d_error,
            created,
            "GDI staging texture creation failed");
    }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return make_gdi_result(
            WgcStatus::capture_error,
            HRESULT_FROM_WIN32(GetLastError()),
            "performance counter frequency query failed");
    }
    qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    const GpuError reserved = internal::reserve_shared_frame_bus_producer(
        bus_state, bus_producer_token);
    if (!reserved) {
        return make_gdi_result(
            WgcStatus::invalid_state,
            FAILED(reserved.hresult) ? reserved.hresult : E_UNEXPECTED,
            reserved.what());
    }
    return make_gdi_result(WgcStatus::ok);
}

WgcResult GdiMonitorCaptureState::build_cursor(
    WgcCursorInfo& cursor, WgcCursorShape& pending) {
    cursor = {};
    pending = {};
    if (!options.include_cursor_metadata) {
        return make_gdi_result(WgcStatus::ok);
    }
    CURSORINFO win32_cursor{sizeof(win32_cursor)};
    if (!GetCursorInfo(&win32_cursor)) {
        return make_gdi_result(WgcStatus::ok);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const bool visible = (win32_cursor.flags & CURSOR_SHOWING) != 0;
    const std::int64_t local_x =
        static_cast<std::int64_t>(win32_cursor.ptScreenPos.x)
        - capture_rect.left;
    const std::int64_t local_y =
        static_cast<std::int64_t>(win32_cursor.ptScreenPos.y)
        - capture_rect.top;
    if (current_cursor_shape.sequence == 0
        || win32_cursor.hCursor != last_win32_cursor) {
        WgcCursorShape sampled;
        if (internal::extract_win32_cursor_shape(
                win32_cursor.hCursor, sampled)) {
            // The GDI path never scales, so the shape is published as-is.
            pending = std::move(sampled);
        }
        last_win32_cursor = win32_cursor.hCursor;
    }
    const bool position_valid = visible
        && local_x >= 0 && local_y >= 0
        && local_x + static_cast<std::int64_t>(
                pending.sequence != 0
                    ? pending.width
                    : current_cursor_shape.width) <= capture_width
        && local_y + static_cast<std::int64_t>(
                pending.sequence != 0
                    ? pending.height
                    : current_cursor_shape.height) <= capture_height;
    if (visible) cursor.flags |= wgc_cursor_visible;
    if (position_valid) {
        cursor.flags |= wgc_cursor_position_valid
            | wgc_cursor_position_estimated;
        cursor.sample_qpc = static_cast<std::uint64_t>(now.QuadPart);
        cursor.screen_x = win32_cursor.ptScreenPos.x;
        cursor.screen_y = win32_cursor.ptScreenPos.y;
        cursor.frame_x = static_cast<std::int32_t>(local_x);
        cursor.frame_y = static_cast<std::int32_t>(local_y);
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
    return make_gdi_result(WgcStatus::ok);
}

WgcResult GdiMonitorCaptureState::capture_once() {
    if (screen_dc == nullptr) {
        screen_dc = GetDC(nullptr);
        if (screen_dc == nullptr) {
            return make_gdi_result(
                WgcStatus::capture_error,
                E_FAIL,
                "GDI screen DC acquisition failed");
        }
        memory_dc = CreateCompatibleDC(screen_dc);
        if (memory_dc == nullptr) {
            return make_gdi_result(
                WgcStatus::capture_error,
                E_FAIL,
                "GDI compatible DC creation failed");
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(capture_width);
        info.bmiHeader.biHeight = -static_cast<LONG>(capture_height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(
            screen_dc, &info, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
        if (dib == nullptr || dib_bits == nullptr) {
            return make_gdi_result(
                WgcStatus::capture_error,
                E_FAIL,
                "GDI DIB section creation failed");
        }
        previous_bmp = static_cast<HBITMAP>(
            SelectObject(memory_dc, dib));
    }

    const BOOL blitted = BitBlt(
        memory_dc,
        0,
        0,
        static_cast<int>(capture_width),
        static_cast<int>(capture_height),
        screen_dc,
        capture_rect.left,
        capture_rect.top,
        SRCCOPY | CAPTUREBLT);
    if (!blitted) {
        return make_gdi_result(
            WgcStatus::capture_error,
            HRESULT_FROM_WIN32(GetLastError()),
            "GDI BitBlt failed");
    }
    received_frames.fetch_add(1, std::memory_order_relaxed);

    WgcCursorInfo cursor;
    WgcCursorShape pending_shape;
    const WgcResult built = build_cursor(cursor, pending_shape);
    if (!built) return built;

    if (options.include_cursor) {
        CURSORINFO win32_cursor{sizeof(win32_cursor)};
        if (GetCursorInfo(&win32_cursor)
            && (win32_cursor.flags & CURSOR_SHOWING) != 0
            && win32_cursor.hCursor != nullptr) {
            ICONINFO icon{};
            if (GetIconInfo(win32_cursor.hCursor, &icon)) {
                const std::int32_t x = win32_cursor.ptScreenPos.x
                    - capture_rect.left
                    - static_cast<std::int32_t>(icon.xHotspot);
                const std::int32_t y = win32_cursor.ptScreenPos.y
                    - capture_rect.top
                    - static_cast<std::int32_t>(icon.yHotspot);
                (void)DrawIcon(
                    memory_dc,
                    x,
                    y,
                    win32_cursor.hCursor);
                if (icon.hbmColor != nullptr) DeleteObject(icon.hbmColor);
                if (icon.hbmMask != nullptr) DeleteObject(icon.hbmMask);
            }
        }
    }

    SharedFrameBusWriteLease lease;
    const GpuError begun = internal::begin_shared_frame_bus_publish(
        bus_state, bus_producer_token, lease);
    if (!begun) {
        if (begun.status == GpuStatus::timeout) {
            skipped_no_buffer.fetch_add(1, std::memory_order_relaxed);
            return make_gdi_result(WgcStatus::ok);
        }
        return gdi_gpu_result(begun, "GDI bus lease failed");
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapped_hr = context->Map(
        staging_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(mapped_hr)) {
        return make_gdi_result(
            WgcStatus::d3d_error,
            mapped_hr,
            "GDI staging texture mapping failed");
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(capture_width) * 4u;
    const auto* source_rows = static_cast<const std::uint8_t*>(dib_bits);
    for (std::uint32_t row = 0; row < capture_height; ++row) {
        const std::uint8_t* source = source_rows + row_bytes * row;
        auto* destination = static_cast<std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(mapped.RowPitch) * row;
        std::memcpy(destination, source, row_bytes);
        // BitBlt leaves alpha zero; the bus contract expects opaque BGRA.
        for (std::uint32_t column = 3; column < row_bytes; column += 4) {
            destination[column] = 255;
        }
    }
    context->Unmap(staging_texture.Get(), 0);
    context->CopySubresourceRegion(
        lease.texture(), 0, 0, 0, 0, staging_texture.Get(), 0, nullptr);
    ingress_copy_submissions.fetch_add(1, std::memory_order_relaxed);

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    SharedFrameBusFrameMetadata metadata;
    metadata.valid_fields = shared_frame_bus_metadata_qpc
        | shared_frame_bus_metadata_source_dimensions
        | shared_frame_bus_metadata_roi
        | shared_frame_bus_metadata_mailbox_generation
        | shared_frame_bus_metadata_color_space;
    metadata.timestamp_qpc = static_cast<std::uint64_t>(now.QuadPart);
    metadata.qpc_frequency = qpc_frequency;
    metadata.mailbox_generation = mailbox.generation;
    metadata.source_width = mailbox.source_width;
    metadata.source_height = mailbox.source_height;
    metadata.roi_x = mailbox.x;
    metadata.roi_y = mailbox.y;
    metadata.roi_width = mailbox.width;
    metadata.roi_height = mailbox.height;
    metadata.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    SharedFrameBusFrameSideData side_data;
    side_data.epoch = epoch;
    side_data.epoch_nonce = epoch_nonce;
    side_data.damage = {};
    // GDI has no OS damage metadata: every frame is a full frame.
    side_data.damage.flags =
        wgc_damage_valid | wgc_damage_full_frame;
    side_data.damage.dirty_count = 1;
    side_data.damage.dirty_rects[0] = {
        0, 0, capture_width, capture_height};
    side_data.cursor = cursor;
    const GpuError committed = internal::commit_shared_frame_bus_publish(
        bus_state,
        bus_producer_token,
        std::move(lease),
        metadata,
        side_data);
    if (!committed) {
        return gdi_gpu_result(committed, "GDI bus commit failed");
    }
    if (pending_shape.sequence != 0) {
        const GpuError published_shape =
            internal::publish_shared_frame_bus_cursor_shape(
                bus_state, bus_producer_token, pending_shape);
        if (!published_shape) {
            return gdi_gpu_result(
                published_shape, "GDI cursor shape publish failed");
        }
        current_cursor_shape = std::move(pending_shape);
        cursor_shape_updates.fetch_add(1, std::memory_order_relaxed);
    }
    published_frames.fetch_add(1, std::memory_order_relaxed);
    full_damage_frames.fetch_add(1, std::memory_order_relaxed);
    if (options.include_cursor_metadata) {
        cursor_metadata_frames.fetch_add(1, std::memory_order_relaxed);
    }
    return make_gdi_result(WgcStatus::ok);
}

WgcResult GdiMonitorCaptureState::start() {
    std::lock_guard lock(lifecycle_mutex);
    if (started.load(std::memory_order_acquire)
        || stopped.load(std::memory_order_acquire)
        || staging_texture == nullptr || bus_producer_token == 0) {
        return make_gdi_result(
            WgcStatus::invalid_state,
            E_UNEXPECTED,
            "GDI capture cannot be started");
    }
    try {
        started.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        worker = std::jthread(
            [this](std::stop_token token) noexcept {
                worker_main(token);
            });
        return make_gdi_result(WgcStatus::ok);
    } catch (...) {
        running.store(false, std::memory_order_release);
        started.store(false, std::memory_order_release);
        return gdi_exception_result();
    }
}

void GdiMonitorCaptureState::stop() noexcept {
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
    if (memory_dc != nullptr && previous_bmp != nullptr) {
        (void)SelectObject(memory_dc, previous_bmp);
    }
    if (dib != nullptr) {
        DeleteObject(dib);
        dib = nullptr;
        dib_bits = nullptr;
    }
    if (memory_dc != nullptr) {
        DeleteDC(memory_dc);
        memory_dc = nullptr;
    }
    if (screen_dc != nullptr) {
        ReleaseDC(nullptr, screen_dc);
        screen_dc = nullptr;
    }
    previous_bmp = nullptr;
    staging_texture.Reset();
    if (bus_state != nullptr && bus_producer_token != 0) {
        internal::release_shared_frame_bus_producer(
            bus_state, bus_producer_token);
        bus_producer_token = 0;
    }
    bus_state.reset();
}

void GdiMonitorCaptureState::worker_main(std::stop_token token) noexcept {
    const auto interval = std::chrono::milliseconds(
        options.gdi_poll_interval_ms);
    auto next_tick = std::chrono::steady_clock::now();
    while (!token.stop_requested()) {
        WgcResult captured;
        try {
            captured = capture_once();
        } catch (...) {
            captured = gdi_exception_result();
        }
        if (!captured) {
            record_error(captured);
            break;
        }
        next_tick += interval;
        std::this_thread::sleep_until(next_tick);
    }
    running.store(false, std::memory_order_release);
}

void GdiMonitorCaptureState::record_error(WgcResult error) noexcept {
    try {
        std::lock_guard lock(error_mutex);
        last_failure = std::move(error);
    } catch (...) {
    }
}

GdiMonitorCapture::GdiMonitorCapture(
    std::shared_ptr<GdiMonitorCaptureState> state) noexcept
    : state_(std::move(state)) {}

GdiMonitorCapture::~GdiMonitorCapture() { stop(); }

GdiMonitorCapture::GdiMonitorCapture(GdiMonitorCapture&&) noexcept = default;

GdiMonitorCapture& GdiMonitorCapture::operator=(
    GdiMonitorCapture&& other) noexcept {
    if (this != &other) {
        stop();
        state_ = std::move(other.state_);
    }
    return *this;
}

WgcResult GdiMonitorCapture::create_for_monitor_to_bus(
    HMONITOR monitor,
    SharedFrameBusPublisher& publisher,
    const WgcCaptureOptions& options,
    const WgcMailboxConfig& mailbox,
    GdiMonitorCapture& output) noexcept {
    try {
        if (publisher.state_ == nullptr) {
            return make_gdi_result(
                WgcStatus::invalid_argument,
                E_UNEXPECTED,
                "shared frame bus publisher is uninitialized");
        }
        auto state = std::make_shared<GdiMonitorCaptureState>();
        const WgcResult initialized =
            state->initialize(monitor, publisher.state_, options, mailbox);
        if (!initialized) return initialized;
        output.stop();
        output = GdiMonitorCapture(std::move(state));
        return make_gdi_result(WgcStatus::ok);
    } catch (...) {
        return gdi_exception_result();
    }
}

WgcResult GdiMonitorCapture::start() noexcept {
    if (state_ == nullptr) {
        return make_gdi_result(WgcStatus::invalid_state, E_UNEXPECTED);
    }
    try {
        return state_->start();
    } catch (...) {
        return gdi_exception_result();
    }
}

void GdiMonitorCapture::stop() noexcept {
    if (state_ != nullptr) state_->stop();
}

WgcMailboxState GdiMonitorCapture::mailbox_state() const noexcept {
    if (state_ == nullptr) return {};
    return {
        state_->mailbox_config,
        state_->mailbox.source_width,
        state_->mailbox.source_height,
        state_->mailbox.generation,
        true};
}

ID3D11Device* GdiMonitorCapture::device() const noexcept {
    return state_ != nullptr ? state_->device.Get() : nullptr;
}

ID3D11DeviceContext* GdiMonitorCapture::context() const noexcept {
    return state_ != nullptr ? state_->context.Get() : nullptr;
}

bool GdiMonitorCapture::running() const noexcept {
    return state_ != nullptr
        && state_->running.load(std::memory_order_acquire);
}

WgcCaptureStats GdiMonitorCapture::stats() const noexcept {
    if (state_ == nullptr) return {};
    WgcCaptureStats output;
    output.received_frames =
        state_->received_frames.load(std::memory_order_relaxed);
    output.published_frames =
        state_->published_frames.load(std::memory_order_relaxed);
    output.skipped_no_buffer =
        state_->skipped_no_buffer.load(std::memory_order_relaxed);
    output.ingress_copy_submissions =
        state_->ingress_copy_submissions.load(std::memory_order_relaxed);
    output.full_damage_frames =
        state_->full_damage_frames.load(std::memory_order_relaxed);
    output.cursor_metadata_frames =
        state_->cursor_metadata_frames.load(std::memory_order_relaxed);
    output.cursor_shape_updates =
        state_->cursor_shape_updates.load(std::memory_order_relaxed);
    output.epoch = state_->epoch;
    output.epoch_nonce = state_->epoch_nonce;
    return output;
}

WgcResult GdiMonitorCapture::last_error() const noexcept {
    if (state_ == nullptr) {
        return make_gdi_result(WgcStatus::invalid_state, E_UNEXPECTED);
    }
    try {
        std::lock_guard lock(state_->error_mutex);
        return state_->last_failure;
    } catch (...) {
        return {WgcStatus::capture_error, E_FAIL, {}};
    }
}

bool GdiMonitorCapture::initialized() const noexcept {
    return state_ != nullptr;
}

} // namespace fluxcap::gpu
