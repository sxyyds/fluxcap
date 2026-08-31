#include <fluxcap/gpu.hpp>

#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <windows.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
namespace gpu = fluxcap::gpu;

constexpr wchar_t kPreviewClass[] = L"FluxCapGpuPreviewWindow";
constexpr UINT kNativeSizeCommand = 1001;
constexpr UINT k320SizeCommand = 1002;
constexpr UINT k640SizeCommand = 1003;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require_hresult(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        fail(std::string(operation) + " failed (hr=" + std::to_string(hr) + ")");
    }
}

double milliseconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
}

HWND parse_window(const wchar_t* text) {
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 0);
    if (end == text || *end != L'\0'
        || value > std::numeric_limits<std::uintptr_t>::max()) {
        fail("invalid HWND; use decimal or 0x-prefixed hexadecimal input");
    }
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
}

std::size_t parse_index(const wchar_t* text, const char* name) {
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (end == text || *end != L'\0'
        || value > std::numeric_limits<std::size_t>::max()) {
        fail(std::string(name) + " must be a non-negative integer");
    }
    return static_cast<std::size_t>(value);
}

struct MonitorEntry final {
    HMONITOR handle = nullptr;
    MONITORINFOEXW info{};
};

BOOL CALLBACK collect_monitor(
    HMONITOR monitor, HDC, LPRECT, LPARAM context) noexcept {
    auto* monitors = reinterpret_cast<std::vector<MonitorEntry>*>(context);
    MonitorEntry entry;
    entry.handle = monitor;
    entry.info.cbSize = sizeof(entry.info);
    if (GetMonitorInfoW(monitor, &entry.info)) {
        monitors->push_back(entry);
    }
    return TRUE;
}

std::vector<MonitorEntry> enumerate_monitors() {
    std::vector<MonitorEntry> monitors;
    if (!EnumDisplayMonitors(
            nullptr,
            nullptr,
            &collect_monitor,
            reinterpret_cast<LPARAM>(&monitors))
        || monitors.empty()) {
        fail("no active display monitor is available");
    }
    std::sort(monitors.begin(), monitors.end(), [](const auto& left, const auto& right) {
        const bool left_primary = (left.info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        const bool right_primary = (right.info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        if (left_primary != right_primary) return left_primary;
        if (left.info.rcMonitor.top != right.info.rcMonitor.top) {
            return left.info.rcMonitor.top < right.info.rcMonitor.top;
        }
        return left.info.rcMonitor.left < right.info.rcMonitor.left;
    });
    return monitors;
}

std::wstring monitor_name(const MonitorEntry& monitor) {
    const LONG width = monitor.info.rcMonitor.right - monitor.info.rcMonitor.left;
    const LONG height = monitor.info.rcMonitor.bottom - monitor.info.rcMonitor.top;
    wchar_t dimensions[64]{};
    (void)swprintf_s(dimensions, L"%ld x %ld", width, height);
    std::wstring name = L"Screen ";
    name += monitor.info.szDevice;
    name += L" (";
    name += dimensions;
    name += L")";
    return name;
}

enum class CaptureSourceKind { monitor, window };

struct CaptureSource final {
    CaptureSourceKind kind = CaptureSourceKind::monitor;
    HMONITOR monitor = nullptr;
    HWND window = nullptr;
    std::wstring name;
};

struct CaptureSize final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool native() const noexcept {
        return width == 0 && height == 0;
    }
};

struct PreviewOptions final {
    CaptureSource source;
    CaptureSize size;
};

CaptureSize parse_capture_size(std::wstring_view text) {
    if (text == L"320" || text == L"320x320" || text == L"320X320") {
        return {320, 320};
    }
    if (text == L"640" || text == L"640x640" || text == L"640X640") {
        return {640, 640};
    }
    fail("capture size must be 320, 320x320, 640, or 640x640");
}

CaptureSource select_monitor(std::size_t index) {
    const auto monitors = enumerate_monitors();
    if (index >= monitors.size()) {
        fail("monitor index is out of range; use --list-monitors");
    }
    return {
        CaptureSourceKind::monitor,
        monitors[index].handle,
        nullptr,
        monitor_name(monitors[index])};
}

CaptureSource select_window(const wchar_t* text) {
    const HWND window = parse_window(text);
    if (!IsWindow(window)) fail("capture target HWND is invalid");
    wchar_t title[512]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    std::wstring name = L"Window ";
    name += title[0] == L'\0' ? L"(untitled)" : title;
    return {CaptureSourceKind::window, nullptr, window, std::move(name)};
}

void print_usage() {
    std::wcout
        << L"Usage:\n"
        << L"  fluxcap_gpu_preview [--size 320|640]\n"
        << L"  fluxcap_gpu_preview --monitor N [--size 320|640]\n"
        << L"  fluxcap_gpu_preview --window HWND [--size 320|640]\n"
        << L"  fluxcap_gpu_preview --list-monitors List active displays\n"
        << L"Frames flow through the direct GPU bus without an FPS cap.\n"
        << L"Fixed 320/640 modes use a centered WGC ROI.\n";
}

void print_monitors() {
    const auto monitors = enumerate_monitors();
    for (std::size_t index = 0; index < monitors.size(); ++index) {
        std::wcout << index << L": " << monitor_name(monitors[index]);
        if ((monitors[index].info.dwFlags & MONITORINFOF_PRIMARY) != 0) {
            std::wcout << L" [primary]";
        }
        std::wcout << L'\n';
    }
}

PreviewOptions parse_options(int argc, wchar_t* argv[]) {
    PreviewOptions options;
    bool source_selected = false;
    bool size_selected = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--monitor") {
            if (source_selected || index + 1 >= argc) {
                fail("--monitor requires one index and cannot be combined with --window");
            }
            options.source = select_monitor(parse_index(argv[++index], "monitor index"));
            source_selected = true;
        } else if (argument == L"--window") {
            if (source_selected || index + 1 >= argc) {
                fail("--window requires one HWND and cannot be combined with --monitor");
            }
            options.source = select_window(argv[++index]);
            source_selected = true;
        } else if (argument == L"--size") {
            if (size_selected || index + 1 >= argc) {
                fail("--size requires exactly one value");
            }
            options.size = parse_capture_size(argv[++index]);
            size_selected = true;
        } else {
            fail("invalid arguments; use --help for usage");
        }
    }
    if (!source_selected) options.source = select_monitor(0);
    return options;
}

struct PreviewMetrics final {
    double source_fps = 0.0;
    double bus_fps = 0.0;
    double preview_fps = 0.0;
    double acquire_ms = 0.0;
    double release_ms = 0.0;
    double draw_ms = 0.0;
    double present_ms = 0.0;
    double total_ms = 0.0;
    std::uint64_t sequence = 0;
    std::uint64_t skipped = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
};

struct RenderTiming final {
    double draw_ms = 0.0;
    double present_ms = 0.0;
    bool presented = false;
    bool occluded = false;
};

struct WindowState final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool minimized = false;
    bool resize_pending = true;
    bool dpi_pending = true;
    CaptureSize capture_size;
    bool capture_epoch_pending = true;
    HMENU menu = nullptr;
    HMENU size_menu = nullptr;
    std::uint32_t menu_source_width = 0;
    std::uint32_t menu_source_height = 0;
};

class ScopedWindow final {
public:
    explicit ScopedWindow(HWND window = nullptr) noexcept : window_(window) {}
    ~ScopedWindow() { reset(); }
    ScopedWindow(const ScopedWindow&) = delete;
    ScopedWindow& operator=(const ScopedWindow&) = delete;

    [[nodiscard]] HWND get() const noexcept { return window_; }

    void reset(HWND replacement = nullptr) noexcept {
        if (window_ != nullptr && IsWindow(window_)) DestroyWindow(window_);
        window_ = replacement;
    }

private:
    HWND window_ = nullptr;
};

class CaptureFps final {
public:
    void reset() noexcept { timestamps_.clear(); }

    void add(Clock::time_point timestamp) {
        timestamps_.push_back(timestamp);
        trim(timestamp);
    }

    double value(Clock::time_point now) {
        trim(now);
        if (timestamps_.size() < 2) return 0.0;
        const double elapsed = std::chrono::duration<double>(
            timestamps_.back() - timestamps_.front()).count();
        return elapsed > 0.0
            ? static_cast<double>(timestamps_.size() - 1) / elapsed
            : 0.0;
    }

private:
    void trim(Clock::time_point now) {
        const auto cutoff = now - std::chrono::seconds(1);
        while (!timestamps_.empty() && timestamps_.front() < cutoff) {
            timestamps_.pop_front();
        }
    }

    std::deque<Clock::time_point> timestamps_;
};

class CounterFps final {
public:
    void reset() noexcept { samples_.clear(); }

    double value(Clock::time_point now, std::uint64_t counter) {
        if (samples_.empty()
            || samples_.back().counter != counter
            || now - samples_.back().timestamp >= std::chrono::milliseconds(250)) {
            samples_.push_back({now, counter});
        }
        const auto cutoff = now - std::chrono::seconds(1);
        while (samples_.size() > 1 && samples_[1].timestamp <= cutoff) {
            samples_.pop_front();
        }
        if (samples_.size() < 2) return 0.0;
        const double elapsed = std::chrono::duration<double>(
            samples_.back().timestamp - samples_.front().timestamp).count();
        return elapsed > 0.0
            ? static_cast<double>(
                samples_.back().counter - samples_.front().counter) / elapsed
            : 0.0;
    }

private:
    struct Sample final {
        Clock::time_point timestamp;
        std::uint64_t counter = 0;
    };
    std::deque<Sample> samples_;
};

class PreviewRenderer final {
public:
    ~PreviewRenderer() {
        if (frame_latency_handle_ != nullptr) CloseHandle(frame_latency_handle_);
    }

    void initialize(HWND window, ID3D11Device* device) {
        if (window == nullptr || device == nullptr) fail("preview renderer input is null");
        window_ = window;
        device_ = device;

        ComPtr<IDXGIDevice1> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        require_hresult(device_.As(&dxgi_device), "D3D11 device DXGI query");
        require_hresult(dxgi_device->GetAdapter(&adapter), "DXGI adapter query");
        require_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)), "DXGI factory query");

        RECT client{};
        GetClientRect(window_, &client);
        width_ = static_cast<std::uint32_t>(std::max<LONG>(1, client.right));
        height_ = static_cast<std::uint32_t>(std::max<LONG>(1, client.bottom));

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width_;
        description.Height = height_;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        ComPtr<IDXGISwapChain1> swap_chain1;
        HRESULT hr = factory->CreateSwapChainForHwnd(
            device_.Get(), window_, &description, nullptr, nullptr, &swap_chain1);
        if (FAILED(hr)) {
            description.Flags = 0;
            require_hresult(
                factory->CreateSwapChainForHwnd(
                    device_.Get(), window_, &description, nullptr, nullptr, &swap_chain1),
                "preview swap chain creation");
        }
        require_hresult(swap_chain1.As(&swap_chain_), "IDXGISwapChain2 query");
        (void)factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
        swap_chain_flags_ = description.Flags;
        if ((swap_chain_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
            (void)swap_chain_->SetMaximumFrameLatency(1);
            frame_latency_handle_ = swap_chain_->GetFrameLatencyWaitableObject();
        }

        D2D1_FACTORY_OPTIONS factory_options{};
        require_hresult(
            D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory1),
                &factory_options,
                reinterpret_cast<void**>(d2d_factory_.GetAddressOf())),
            "D2D factory creation");
        require_hresult(
            d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_),
            "D2D device creation");
        require_hresult(
            d2d_device_->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context_),
            "D2D context creation");
        d2d_context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        d2d_context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        require_hresult(
            DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(write_factory_.GetAddressOf())),
            "DirectWrite factory creation");
        require_hresult(
            d2d_context_->CreateSolidColorBrush(
                D2D1_COLOR_F{0.02F, 0.025F, 0.03F, 0.82F}, &panel_brush_),
            "overlay panel brush creation");
        require_hresult(
            d2d_context_->CreateSolidColorBrush(
                D2D1_COLOR_F{0.96F, 0.97F, 0.98F, 1.0F}, &text_brush_),
            "overlay text brush creation");
        require_hresult(
            d2d_context_->CreateSolidColorBrush(
                D2D1_COLOR_F{0.25F, 0.92F, 0.58F, 1.0F}, &accent_brush_),
            "overlay accent brush creation");
        require_hresult(
            d2d_context_->CreateSolidColorBrush(
                D2D1_COLOR_F{0.66F, 0.70F, 0.75F, 1.0F}, &muted_brush_),
            "overlay muted brush creation");

        update_dpi();
        create_back_buffer_target();
    }

    void resize(std::uint32_t width, std::uint32_t height) {
        if (width == 0 || height == 0 || swap_chain_ == nullptr) return;
        if (width == width_ && height == height_) return;
        d2d_context_->SetTarget(nullptr);
        target_bitmap_.Reset();
        require_hresult(
            swap_chain_->ResizeBuffers(
                0, width, height, DXGI_FORMAT_UNKNOWN, swap_chain_flags_),
            "preview swap chain resize");
        width_ = width;
        height_ = height;
        create_back_buffer_target();
    }

    void update_dpi() {
        const UINT dpi = GetDpiForWindow(window_);
        dpi_scale_ = static_cast<float>(dpi == 0 ? 96 : dpi) / 96.0F;
        create_text_formats();
    }

    [[nodiscard]] bool ready_to_present() const noexcept {
        return frame_latency_handle_ == nullptr
            || WaitForSingleObject(frame_latency_handle_, 0) == WAIT_OBJECT_0;
    }

    RenderTiming render(
        ID3D11Texture2D* texture,
        const gpu::WgcFrameInfo& frame,
        const PreviewMetrics& metrics) {
        if (texture == nullptr || target_bitmap_ == nullptr) {
            fail("preview render target is unavailable");
        }
        ID2D1Bitmap1* source = source_bitmap(texture, frame);
        if (source == nullptr) fail("preview source bitmap is unavailable");

        const auto draw_begin = Clock::now();
        d2d_context_->BeginDraw();
        d2d_context_->SetTransform(D2D1_MATRIX_3X2_F{1, 0, 0, 1, 0, 0});
        d2d_context_->Clear(D2D1_COLOR_F{0.018F, 0.021F, 0.026F, 1.0F});

        const float client_width = static_cast<float>(width_);
        const float client_height = static_cast<float>(height_);
        const float source_width = static_cast<float>(frame.width);
        const float source_height = static_cast<float>(frame.height);
        const float scale = std::min(
            client_width / source_width,
            client_height / source_height);
        const float draw_width = source_width * scale;
        const float draw_height = source_height * scale;
        const float left = (client_width - draw_width) * 0.5F;
        const float top = (client_height - draw_height) * 0.5F;
        const D2D1_RECT_F destination{
            left, top, left + draw_width, top + draw_height};
        d2d_context_->DrawBitmap(
            source,
            destination,
            1.0F,
            D2D1_INTERPOLATION_MODE_LINEAR,
            nullptr,
            nullptr);

        draw_overlay(metrics);
        const HRESULT draw_result = d2d_context_->EndDraw();
        if (draw_result == D2DERR_RECREATE_TARGET) {
            create_back_buffer_target();
        } else {
            require_hresult(draw_result, "preview D2D draw");
        }
        const auto draw_end = Clock::now();

        const auto present_begin = Clock::now();
        const HRESULT present_result = swap_chain_->Present(
            0, DXGI_PRESENT_DO_NOT_WAIT);
        const auto present_end = Clock::now();
        if (present_result == DXGI_ERROR_WAS_STILL_DRAWING) {
            return {
                milliseconds(draw_end - draw_begin),
                milliseconds(present_end - present_begin),
                false,
                false};
        }
        if (present_result == DXGI_ERROR_DEVICE_REMOVED
            || present_result == DXGI_ERROR_DEVICE_RESET) {
            require_hresult(present_result, "preview present device");
        }
        if (FAILED(present_result)) require_hresult(present_result, "preview present");
        return {
            milliseconds(draw_end - draw_begin),
            milliseconds(present_end - present_begin),
            present_result == S_OK,
            present_result == DXGI_STATUS_OCCLUDED};
    }

private:
    struct CachedBitmap final {
        ID3D11Texture2D* identity = nullptr;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID2D1Bitmap1> bitmap;
    };

    void create_back_buffer_target() {
        d2d_context_->SetTarget(nullptr);
        target_bitmap_.Reset();
        ComPtr<IDXGISurface> surface;
        require_hresult(
            swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface)),
            "preview back buffer query");
        D2D1_BITMAP_PROPERTIES1 properties{};
        properties.pixelFormat = {
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_IGNORE};
        properties.dpiX = 96.0F;
        properties.dpiY = 96.0F;
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET
            | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        require_hresult(
            d2d_context_->CreateBitmapFromDxgiSurface(
                surface.Get(), &properties, &target_bitmap_),
            "preview target bitmap creation");
        d2d_context_->SetTarget(target_bitmap_.Get());
    }

    ID2D1Bitmap1* source_bitmap(
        ID3D11Texture2D* texture,
        const gpu::WgcFrameInfo& frame) {
        if (frame.width != cached_source_width_
            || frame.height != cached_source_height_
            || frame.format != cached_source_format_) {
            source_bitmaps_.clear();
            cached_source_width_ = frame.width;
            cached_source_height_ = frame.height;
            cached_source_format_ = frame.format;
        }
        for (auto& cached : source_bitmaps_) {
            if (cached.identity == texture) return cached.bitmap.Get();
        }

        ComPtr<IDXGISurface> surface;
        require_hresult(texture->QueryInterface(IID_PPV_ARGS(&surface)),
            "capture texture DXGI surface query");
        D2D1_BITMAP_PROPERTIES1 properties{};
        properties.pixelFormat = {frame.format, D2D1_ALPHA_MODE_IGNORE};
        properties.dpiX = 96.0F;
        properties.dpiY = 96.0F;
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

        CachedBitmap cached;
        cached.identity = texture;
        cached.texture = texture;
        require_hresult(
            d2d_context_->CreateBitmapFromDxgiSurface(
                surface.Get(), &properties, &cached.bitmap),
            "capture source bitmap creation");
        source_bitmaps_.push_back(std::move(cached));
        if (source_bitmaps_.size() > 16) source_bitmaps_.erase(source_bitmaps_.begin());
        return source_bitmaps_.back().bitmap.Get();
    }

    void create_text_formats() {
        label_format_.Reset();
        fps_format_.Reset();
        detail_format_.Reset();
        require_hresult(
            write_factory_->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                11.0F * dpi_scale_,
                L"zh-CN",
                &label_format_),
            "preview label format creation");
        require_hresult(
            write_factory_->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                27.0F * dpi_scale_,
                L"zh-CN",
                &fps_format_),
            "preview FPS format creation");
        require_hresult(
            write_factory_->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                12.5F * dpi_scale_,
                L"zh-CN",
                &detail_format_),
            "preview detail format creation");
        label_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fps_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        detail_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    void draw_overlay(const PreviewMetrics& metrics) {
        const float margin = 14.0F * dpi_scale_;
        const float panel_width = std::min(
            600.0F * dpi_scale_,
            std::max(0.0F, static_cast<float>(width_) - margin * 2.0F));
        const float panel_height = 184.0F * dpi_scale_;
        const D2D1_ROUNDED_RECT panel{
            D2D1_RECT_F{
                margin,
                margin,
                margin + panel_width,
                margin + panel_height},
            6.0F * dpi_scale_,
            6.0F * dpi_scale_};
        d2d_context_->FillRoundedRectangle(panel, panel_brush_.Get());

        constexpr wchar_t label[] = L"FLUXCAP  LIVE PREVIEW";
        const D2D1_RECT_F label_rect{
            margin + 16.0F * dpi_scale_,
            margin + 10.0F * dpi_scale_,
            margin + panel_width - 12.0F * dpi_scale_,
            margin + 29.0F * dpi_scale_};
        d2d_context_->DrawTextW(
            label,
            static_cast<UINT32>(std::size(label) - 1),
            label_format_.Get(),
            label_rect,
            muted_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);

        wchar_t fps_text[64]{};
        (void)swprintf_s(fps_text, L"%.1f WGC FPS", metrics.source_fps);
        const D2D1_RECT_F fps_rect{
            margin + 15.0F * dpi_scale_,
            margin + 28.0F * dpi_scale_,
            margin + 430.0F * dpi_scale_,
            margin + 68.0F * dpi_scale_};
        d2d_context_->DrawTextW(
            fps_text,
            static_cast<UINT32>(wcslen(fps_text)),
            fps_format_.Get(),
            fps_rect,
            accent_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);

        wchar_t detail_text[448]{};
        (void)swprintf_s(
            detail_text,
            L"Bus %.1f FPS DIRECT    Preview %.1f FPS\n"
            L"Acquire %.3f ms    Release %.3f ms\n"
            L"Draw %.3f ms    Present %.3f ms    Total %.3f ms\n"
            L"Source %u x %u    Bus %u x %u\n"
            L"Sequence #%llu    Skipped %llu",
            metrics.bus_fps,
            metrics.preview_fps,
            metrics.acquire_ms,
            metrics.release_ms,
            metrics.draw_ms,
            metrics.present_ms,
            metrics.total_ms,
            metrics.source_width,
            metrics.source_height,
            metrics.output_width,
            metrics.output_height,
            static_cast<unsigned long long>(metrics.sequence),
            static_cast<unsigned long long>(metrics.skipped));
        const D2D1_RECT_F detail_rect{
            margin + 16.0F * dpi_scale_,
            margin + 69.0F * dpi_scale_,
            margin + panel_width - 12.0F * dpi_scale_,
            margin + panel_height - 8.0F * dpi_scale_};
        d2d_context_->DrawTextW(
            detail_text,
            static_cast<UINT32>(wcslen(detail_text)),
            detail_format_.Get(),
            detail_rect,
            text_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
    }

    HWND window_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<IDXGISwapChain2> swap_chain_;
    HANDLE frame_latency_handle_ = nullptr;
    UINT swap_chain_flags_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    float dpi_scale_ = 1.0F;

    ComPtr<ID2D1Factory1> d2d_factory_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<ID2D1DeviceContext> d2d_context_;
    ComPtr<ID2D1Bitmap1> target_bitmap_;
    ComPtr<ID2D1SolidColorBrush> panel_brush_;
    ComPtr<ID2D1SolidColorBrush> text_brush_;
    ComPtr<ID2D1SolidColorBrush> accent_brush_;
    ComPtr<ID2D1SolidColorBrush> muted_brush_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<IDWriteTextFormat> label_format_;
    ComPtr<IDWriteTextFormat> fps_format_;
    ComPtr<IDWriteTextFormat> detail_format_;
    std::vector<CachedBitmap> source_bitmaps_;
    std::uint32_t cached_source_width_ = 0;
    std::uint32_t cached_source_height_ = 0;
    DXGI_FORMAT cached_source_format_ = DXGI_FORMAT_UNKNOWN;
};

UINT capture_size_command(const CaptureSize& size) noexcept {
    if (size.native()) return kNativeSizeCommand;
    if (size.width == 320 && size.height == 320) return k320SizeCommand;
    return k640SizeCommand;
}

void update_size_menu(WindowState& state) noexcept {
    if (state.size_menu == nullptr) return;
    (void)CheckMenuRadioItem(
        state.size_menu,
        kNativeSizeCommand,
        k640SizeCommand,
        capture_size_command(state.capture_size),
        MF_BYCOMMAND);
}

void request_capture_size(WindowState& state, CaptureSize size) noexcept {
    if (state.capture_size.width == size.width
        && state.capture_size.height == size.height) {
        return;
    }
    state.capture_size = size;
    state.capture_epoch_pending = true;
    update_size_menu(state);
}

bool capture_size_fits(
    const CaptureSize& size,
    std::uint32_t source_width,
    std::uint32_t source_height) noexcept {
    return size.native()
        || (size.width <= source_width && size.height <= source_height);
}

void update_size_availability(
    WindowState& state,
    HWND window,
    std::uint32_t source_width,
    std::uint32_t source_height) noexcept {
    if (state.size_menu == nullptr
        || (state.menu_source_width == source_width
            && state.menu_source_height == source_height)) {
        return;
    }
    state.menu_source_width = source_width;
    state.menu_source_height = source_height;
    const UINT enabled_320 = source_width >= 320 && source_height >= 320
        ? MF_ENABLED : MF_GRAYED;
    const UINT enabled_640 = source_width >= 640 && source_height >= 640
        ? MF_ENABLED : MF_GRAYED;
    (void)EnableMenuItem(
        state.size_menu, k320SizeCommand, MF_BYCOMMAND | enabled_320);
    (void)EnableMenuItem(
        state.size_menu, k640SizeCommand, MF_BYCOMMAND | enabled_640);
    (void)DrawMenuBar(window);
}

HMENU create_preview_menu(WindowState& state) {
    HMENU menu = CreateMenu();
    HMENU sizes = CreatePopupMenu();
    if (menu == nullptr || sizes == nullptr) {
        if (sizes != nullptr) DestroyMenu(sizes);
        if (menu != nullptr) DestroyMenu(menu);
        fail("preview menu creation failed");
    }
    if (!AppendMenuW(sizes, MF_STRING, kNativeSizeCommand, L"Native screen")
        || !AppendMenuW(sizes, MF_STRING, k320SizeCommand, L"320 x 320 center ROI")
        || !AppendMenuW(sizes, MF_STRING, k640SizeCommand, L"640 x 640 center ROI")) {
        DestroyMenu(sizes);
        DestroyMenu(menu);
        fail("preview menu creation failed");
    }
    (void)EnableMenuItem(
        sizes, k320SizeCommand, MF_BYCOMMAND | MF_GRAYED);
    (void)EnableMenuItem(
        sizes, k640SizeCommand, MF_BYCOMMAND | MF_GRAYED);
    if (!AppendMenuW(
            menu,
            MF_POPUP,
            reinterpret_cast<UINT_PTR>(sizes),
            L"Capture size")) {
        DestroyMenu(sizes);
        DestroyMenu(menu);
        fail("preview menu creation failed");
    }
    state.menu = menu;
    state.size_menu = sizes;
    update_size_menu(state);
    return menu;
}

LRESULT CALLBACK preview_window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* state = reinterpret_cast<WindowState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_COMMAND:
        if (state != nullptr) {
            switch (LOWORD(wparam)) {
            case kNativeSizeCommand:
                request_capture_size(*state, {});
                return 0;
            case k320SizeCommand:
                request_capture_size(*state, {320, 320});
                return 0;
            case k640SizeCommand:
                request_capture_size(*state, {640, 640});
                return 0;
            default:
                break;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    case WM_SIZE:
        if (state != nullptr) {
            state->width = LOWORD(lparam);
            state->height = HIWORD(lparam);
            state->minimized = wparam == SIZE_MINIMIZED
                || state->width == 0 || state->height == 0;
            state->resize_pending = !state->minimized;
        }
        return 0;
    case WM_DPICHANGED:
        if (state != nullptr) state->dpi_pending = true;
        if (const auto* suggested = reinterpret_cast<const RECT*>(lparam)) {
            SetWindowPos(
                window,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (auto* limits = reinterpret_cast<MINMAXINFO*>(lparam)) {
            limits->ptMinTrackSize.x = 520;
            limits->ptMinTrackSize.y = 320;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state != nullptr && state->menu != nullptr) {
            (void)SetMenu(window, nullptr);
            DestroyMenu(state->menu);
            state->menu = nullptr;
            state->size_menu = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

HWND create_preview_window(WindowState& state, std::wstring_view target_name) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &preview_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kPreviewClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (RegisterClassW(&window_class) == 0
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fail("preview window class registration failed");
    }

    std::wstring title = L"FluxCap Preview";
    if (!target_name.empty()) title += L" - " + std::wstring(target_name);
    RECT bounds{0, 0, 1100, 700};
    HMENU menu = create_preview_menu(state);
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, TRUE, 0);
    HWND window = CreateWindowExW(
        0,
        kPreviewClass,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        nullptr,
        menu,
        window_class.hInstance,
        &state);
    if (window == nullptr) {
        if (state.menu != nullptr) DestroyMenu(state.menu);
        state.menu = nullptr;
        state.size_menu = nullptr;
        fail("preview window creation failed");
    }
    return window;
}

class DirectBusCapture final {
public:
    ~DirectBusCapture() { close_epoch(false); }

    void initialize(
        const CaptureSource& source,
        const gpu::WgcCaptureOptions& options,
        const CaptureSize& size) {
        source_ = source;
        options_ = options;
        create_epoch(size);
    }

    void reconfigure(const CaptureSize& size) { create_epoch(size); }

    [[nodiscard]] ID3D11Device* device() const noexcept {
        return device_.Get();
    }

    [[nodiscard]] gpu::GpuError acquire_latest(
        std::uint32_t timeout_ms,
        gpu::SharedFrameBusFrameLease& output) noexcept {
        return consumer_.acquire_latest(timeout_ms, output);
    }

    [[nodiscard]] gpu::GpuError release(
        gpu::SharedFrameBusFrameLease& lease) noexcept {
        return consumer_.release(lease);
    }

    [[nodiscard]] gpu::WgcCaptureStats capture_stats() const noexcept {
        return capture_.stats();
    }

    [[nodiscard]] gpu::WgcMailboxState mailbox_state() const noexcept {
        return capture_.mailbox_state();
    }

    [[nodiscard]] gpu::WgcResult last_error() const noexcept {
        return capture_.last_error();
    }

    [[nodiscard]] bool target_closed() const noexcept {
        return capture_.target_closed();
    }

    [[nodiscard]] bool native_epoch_needs_resize(
        std::uint32_t source_width,
        std::uint32_t source_height) const noexcept {
        return capture_size_.native()
            && source_width != 0
            && source_height != 0
            && (source_width != epoch_source_width_
                || source_height != epoch_source_height_);
    }

private:
    gpu::WgcResult create_probe(gpu::WgcCapture& output) const noexcept {
        return source_.kind == CaptureSourceKind::monitor
            ? gpu::WgcCapture::create_for_monitor(
                source_.monitor, device_.Get(), options_, output)
            : gpu::WgcCapture::create_for_window(
                source_.window, device_.Get(), options_, output);
    }

    gpu::WgcResult create_direct(
        gpu::SharedFrameBusPublisher& publisher,
        const gpu::WgcMailboxConfig& mailbox,
        gpu::WgcCapture& output) const noexcept {
        return source_.kind == CaptureSourceKind::monitor
            ? gpu::WgcCapture::create_for_monitor_to_bus(
                source_.monitor, publisher, options_, mailbox, output)
            : gpu::WgcCapture::create_for_window_to_bus(
                source_.window, publisher, options_, mailbox, output);
    }

    void create_epoch(const CaptureSize& size) {
        gpu::WgcCapture probe;
        const auto probed = create_probe(probe);
        if (!probed) fail("WGC source probe failed: " + probed.message);
        if (device_ == nullptr) device_ = probe.device();
        const auto source_state = probe.mailbox_state();
        probe.stop();
        if (device_ == nullptr || source_state.source_width == 0
            || source_state.source_height == 0) {
            fail("WGC source probe returned an invalid device or dimensions");
        }
        if (!capture_size_fits(
                size, source_state.source_width, source_state.source_height)) {
            fail("selected capture size exceeds the source dimensions");
        }

        const std::uint32_t output_width = size.native()
            ? source_state.source_width : size.width;
        const std::uint32_t output_height = size.native()
            ? source_state.source_height : size.height;
        gpu::WgcMailboxConfig mailbox;
        if (!size.native()) {
            mailbox.mode = gpu::WgcMailboxMode::centered_region;
            mailbox.width = size.width;
            mailbox.height = size.height;
        }

        gpu::SharedFrameBusConfig bus_config;
        bus_config.width = output_width;
        bus_config.height = output_height;
        bus_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bus_config.bind_flags = D3D11_BIND_SHADER_RESOURCE
            | D3D11_BIND_RENDER_TARGET;
        bus_config.slot_count = 4;
        bus_config.max_consumers = 1;

        gpu::SharedFrameBusPublisher next_publisher;
        auto gpu_result = gpu::SharedFrameBusPublisher::create(
            device_.Get(), bus_config, next_publisher);
        if (!gpu_result) {
            fail(std::string("preview bus create failed: ") + gpu_result.what());
        }
        gpu::SharedFrameBusRegistration next_registration;
        gpu_result = next_publisher.register_consumer(
            GetCurrentProcess(), next_registration);
        if (!gpu_result) {
            fail(std::string("preview bus consumer registration failed: ")
                + gpu_result.what());
        }
        gpu::SharedFrameBusConsumer next_consumer;
        gpu_result = gpu::SharedFrameBusConsumer::open(
            device_.Get(), next_registration, true, next_consumer);
        if (!gpu_result) {
            fail(std::string("preview bus consumer open failed: ")
                + gpu_result.what());
        }

        gpu::WgcCapture next_capture;
        const auto created = create_direct(
            next_publisher, mailbox, next_capture);
        if (!created) fail("WGC direct-bus create failed: " + created.message);
        const auto started = next_capture.start();
        if (!started) fail("WGC direct-bus start failed: " + started.message);

        close_epoch(true);
        capture_ = std::move(next_capture);
        publisher_ = std::move(next_publisher);
        consumer_ = std::move(next_consumer);
        registration_ = next_registration;
        registered_ = true;
        capture_size_ = size;
        epoch_source_width_ = source_state.source_width;
        epoch_source_height_ = source_state.source_height;
    }

    void close_epoch(bool checked) {
        capture_.stop();
        std::string failure;
        if (consumer_.initialized()) {
            const auto closed = consumer_.close();
            if (!closed) {
                failure = std::string("preview bus consumer close failed: ")
                    + closed.what();
            }
        }
        if (registered_ && publisher_.initialized()) {
            const auto unregistered = publisher_.unregister_consumer(
                registration_, 3'000);
            if (!unregistered && failure.empty()) {
                failure = std::string("preview bus consumer unregister failed: ")
                    + unregistered.what();
            }
        }
        capture_ = {};
        consumer_ = {};
        publisher_ = {};
        registration_ = {};
        registered_ = false;
        if (checked && !failure.empty()) fail(failure);
    }

    CaptureSource source_;
    gpu::WgcCaptureOptions options_{};
    ComPtr<ID3D11Device> device_;
    gpu::WgcCapture capture_;
    gpu::SharedFrameBusPublisher publisher_;
    gpu::SharedFrameBusConsumer consumer_;
    gpu::SharedFrameBusRegistration registration_{};
    CaptureSize capture_size_{};
    std::uint32_t epoch_source_width_ = 0;
    std::uint32_t epoch_source_height_ = 0;
    bool registered_ = false;
};

double smooth(double previous, double sample, bool initialized) noexcept {
    return initialized ? previous * 0.85 + sample * 0.15 : sample;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (argc == 2 && (std::wstring_view(argv[1]) == L"--help"
                || std::wstring_view(argv[1]) == L"-h")) {
            print_usage();
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--list-monitors") {
            print_monitors();
            return 0;
        }
        const PreviewOptions options = parse_options(argc, argv);
        const CaptureSource& source = options.source;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        gpu::WgcCaptureOptions capture_options;
        capture_options.buffer_count = 4;
        capture_options.include_cursor = true;
        capture_options.min_update_interval_us = 0;

        WindowState window_state;
        window_state.capture_size = options.size;
        ScopedWindow preview_window(create_preview_window(window_state, source.name));
        HWND preview = preview_window.get();
        if (source.kind == CaptureSourceKind::monitor) {
            (void)SetWindowDisplayAffinity(preview, WDA_EXCLUDEFROMCAPTURE);
        }
        DirectBusCapture pipeline;
        pipeline.initialize(source, capture_options, window_state.capture_size);
        window_state.capture_epoch_pending = false;
        PreviewRenderer renderer;
        renderer.initialize(preview, pipeline.device());
        ShowWindow(preview, SW_SHOW);
        UpdateWindow(preview);

        CounterFps source_fps;
        CaptureFps bus_fps;
        CaptureFps preview_fps;
        PreviewMetrics metrics;
        bool timing_initialized = false;
        bool running = true;

        while (running) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;

            if (window_state.resize_pending) {
                renderer.resize(window_state.width, window_state.height);
                window_state.resize_pending = false;
            }
            if (window_state.dpi_pending) {
                renderer.update_dpi();
                window_state.dpi_pending = false;
            }
            const auto mailbox = pipeline.mailbox_state();
            if (mailbox.source_width != 0 && mailbox.source_height != 0) {
                metrics.source_width = mailbox.source_width;
                metrics.source_height = mailbox.source_height;
                update_size_availability(
                    window_state,
                    preview,
                    mailbox.source_width,
                    mailbox.source_height);
                if (!capture_size_fits(
                        window_state.capture_size,
                        mailbox.source_width,
                        mailbox.source_height)) {
                    request_capture_size(window_state, {});
                } else if (pipeline.native_epoch_needs_resize(
                        mailbox.source_width, mailbox.source_height)) {
                    window_state.capture_epoch_pending = true;
                }
            }

            if (window_state.capture_epoch_pending) {
                pipeline.reconfigure(window_state.capture_size);
                window_state.capture_epoch_pending = false;
                source_fps.reset();
                bus_fps.reset();
                preview_fps.reset();
                metrics = {};
                timing_initialized = false;
                continue;
            }

            if (pipeline.target_closed()) break;
            if (window_state.minimized) {
                MsgWaitForMultipleObjectsEx(
                    0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }
            if (!renderer.ready_to_present()) {
                MsgWaitForMultipleObjectsEx(
                    0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }

            gpu::SharedFrameBusFrameLease frame;
            const auto acquire_begin = Clock::now();
            const auto acquired = pipeline.acquire_latest(8, frame);
            const auto acquire_end = Clock::now();
            if (!acquired) {
                if (pipeline.target_closed()) break;
                if (acquired.status != gpu::GpuStatus::timeout) {
                    fail(std::string("preview bus acquire failed: ")
                        + acquired.what());
                }
                const auto capture_error = pipeline.last_error();
                if (!capture_error
                    && capture_error.status != gpu::WgcStatus::region_unavailable) {
                    fail("WGC direct-bus capture failed: " + capture_error.message);
                }
                MsgWaitForMultipleObjectsEx(
                    0, nullptr, 2, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }

            const auto now = Clock::now();
            bus_fps.add(now);
            const auto capture_stats = pipeline.capture_stats();
            metrics.source_fps = source_fps.value(
                now, capture_stats.received_frames);
            metrics.bus_fps = bus_fps.value(now);
            metrics.preview_fps = preview_fps.value(now);
            metrics.acquire_ms = smooth(
                metrics.acquire_ms,
                milliseconds(acquire_end - acquire_begin),
                timing_initialized);
            metrics.sequence = frame.info().sequence;
            metrics.output_width = frame.info().width;
            metrics.output_height = frame.info().height;
            metrics.skipped = capture_stats.skipped_no_buffer;

            gpu::WgcFrameInfo preview_info;
            preview_info.width = frame.info().width;
            preview_info.height = frame.info().height;
            preview_info.format = frame.info().format;
            preview_info.sequence = frame.info().sequence;

            const auto total_begin = Clock::now();
            const RenderTiming timing = renderer.render(
                frame.texture(), preview_info, metrics);
            // EndDraw/Present queued every D2D read before the bus done fence.
            const auto release_begin = Clock::now();
            const auto released = pipeline.release(frame);
            const auto total_end = Clock::now();
            if (!released) {
                fail(std::string("preview bus release failed: ")
                    + released.what());
            }
            metrics.draw_ms = smooth(
                metrics.draw_ms, timing.draw_ms, timing_initialized);
            metrics.present_ms = smooth(
                metrics.present_ms, timing.present_ms, timing_initialized);
            metrics.release_ms = smooth(
                metrics.release_ms,
                milliseconds(total_end - release_begin),
                timing_initialized);
            metrics.total_ms = smooth(
                metrics.total_ms,
                milliseconds(total_end - total_begin),
                timing_initialized);
            timing_initialized = true;
            if (timing.presented) preview_fps.add(total_end);
            metrics.preview_fps = preview_fps.value(total_end);

            wchar_t window_title[640]{};
            (void)swprintf_s(
                window_title,
                L"FluxCap Direct Bus | WGC %.1f | Bus %.1f | Preview %.1f FPS | "
                L"Acquire %.3f ms | Release %.3f ms | Total %.3f ms | %u x %u | %s",
                metrics.source_fps,
                metrics.bus_fps,
                metrics.preview_fps,
                metrics.acquire_ms,
                metrics.release_ms,
                metrics.total_ms,
                metrics.output_width,
                metrics.output_height,
                source.name.c_str());
            SetWindowTextW(preview, window_title);
            if (timing.occluded) {
                MsgWaitForMultipleObjectsEx(
                    0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        }

        preview_window.reset();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU preview failed: " << error.what() << '\n';
        return 1;
    }
}
