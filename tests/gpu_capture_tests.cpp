#include "desktop_duplication_capture.hpp"
#include "image_encoder.hpp"
#include "gpu_crop.hpp"
#include "gpu_encoder.hpp"
#include "gpu_transform.hpp"
#include "shared_texture.hpp"
#include "wgc_capture.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <codecapi.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
namespace gpu = fluxcap::gpu;
namespace metadata = winrt::Windows::Foundation::Metadata;

constexpr wchar_t kWindowClass[] = L"FluxCapGpuOcclusionTest";
constexpr UINT kStopMessage = WM_APP + 1;
constexpr UINT kMoveOffscreenMessage = WM_APP + 2;
constexpr UINT kPulseMessage = WM_APP + 3;
constexpr UINT kResizePatternSmallMessage = WM_APP + 4;
constexpr UINT kResizePatternRestoreMessage = WM_APP + 5;
constexpr UINT kSetPatternPhase0Message = WM_APP + 6;
constexpr UINT kSetPatternPhase1Message = WM_APP + 7;
constexpr LONG kPatternWidth = 801;
constexpr LONG kPatternHeight = 799;
constexpr ULONG_PTR kPatternPhase0 = std::numeric_limits<ULONG_PTR>::max();
constexpr ULONG_PTR kPatternPhase1 = kPatternPhase0 - 1u;

bool cursor_capture_control_present() noexcept {
    try {
        return metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsCursorCaptureEnabled");
    } catch (...) {
        return false;
    }
}

struct WindowThreadState final {
    HANDLE ready = nullptr;
    std::atomic<HWND> target{nullptr};
    std::atomic<HWND> roi_target{nullptr};
    std::atomic<DWORD> error{ERROR_SUCCESS};
};

struct PacketCollector final {
    std::size_t packet_count = 0;
    std::size_t total_bytes = 0;
    bool saw_keyframe = false;
    std::int64_t first_timestamp_100ns = -1;
    std::int64_t last_timestamp_100ns = -1;
};

struct ChildConsumerResult final {
    std::uint32_t structure_size = sizeof(ChildConsumerResult);
    std::uint32_t success = 0;
    std::uint32_t pixel = 0;
    HRESULT hresult = E_FAIL;
    char message[256]{};
};

enum class BusChildOperation : std::uint32_t {
    open = 1,
    acquire_validate,
    release,
    close,
    exit_process
};

struct BusChildCommand final {
    std::uint32_t structure_size = sizeof(BusChildCommand);
    BusChildOperation operation = BusChildOperation::exit_process;
    std::uint32_t timeout_ms = 0;
    std::uint32_t source_x = 0;
    std::uint32_t source_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t phase = 0;
};

struct BusChildResponse final {
    std::uint32_t structure_size = sizeof(BusChildResponse);
    BusChildOperation operation = BusChildOperation::exit_process;
    std::uint32_t success = 0;
    gpu::GpuStatus status = gpu::GpuStatus::system_error;
    HRESULT hresult = E_FAIL;
    std::uint64_t sequence = 0;
    gpu::SharedFrameBusFrameMetadata metadata{};
    char message[256]{};
};

static_assert(std::is_trivially_copyable_v<BusChildCommand>);
static_assert(std::is_trivially_copyable_v<BusChildResponse>);
static_assert(std::is_trivially_copyable_v<gpu::SharedFrameBusRegistration>);
static_assert(std::is_trivially_copyable_v<gpu::SharedFrameBusFrameMetadata>);
static_assert(!std::is_copy_constructible_v<gpu::GpuEncoderInputLease>);
static_assert(!std::is_copy_assignable_v<gpu::GpuEncoderInputLease>);
static_assert(std::is_nothrow_move_constructible_v<gpu::GpuEncoderInputLease>);
static_assert(std::is_nothrow_move_assignable_v<gpu::GpuEncoderInputLease>);

class ScopedHandle final {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE value) noexcept : value_(value) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : value_(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] HANDLE release() noexcept {
        HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

class ScopedThreadDpiAwareness final {
public:
    ScopedThreadDpiAwareness() noexcept
        : previous_(SetThreadDpiAwarenessContext(
              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~ScopedThreadDpiAwareness() {
        if (previous_ != nullptr) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }
    ScopedThreadDpiAwareness(const ScopedThreadDpiAwareness&) = delete;
    ScopedThreadDpiAwareness& operator=(const ScopedThreadDpiAwareness&) = delete;
private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

void collect_packet(void* context, const gpu::EncodedPacket& packet) {
    auto& collector = *static_cast<PacketCollector*>(context);
    if (packet.data != nullptr && packet.size != 0) {
        if (collector.first_timestamp_100ns < 0) {
            collector.first_timestamp_100ns = packet.timestamp_100ns;
        }
        collector.last_timestamp_100ns = packet.timestamp_100ns;
        ++collector.packet_count;
        collector.total_bytes += packet.size;
        collector.saw_keyframe = collector.saw_keyframe || packet.keyframe;
    }
}

void reject_packet(void*, const gpu::EncodedPacket&) {
    throw std::runtime_error("intentional packet callback failure");
}

std::uint32_t coordinate_pixel_phase(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t phase) noexcept {
    const auto coordinate = [](std::uint32_t px, std::uint32_t py) {
        return 0xff000000u
            | ((py & 0x7ffu) << 12u)
            | (px & 0xfffu);
    };
    if ((phase & 1u) == 0) return coordinate(x, y);

    // The centered 320x320 ROI begins at (240,239). Phase one moves a
    // tile-aligned 32x32 block from local (32,32) to local (96,96) and fills
    // the old source. The rest stays byte-identical, giving the real WGC test
    // conservative, unique move evidence without synthetic GPU textures.
    constexpr std::uint32_t source_x = 272;
    constexpr std::uint32_t source_y = 271;
    constexpr std::uint32_t destination_x = 336;
    constexpr std::uint32_t destination_y = 335;
    constexpr std::uint32_t block_size = 32;
    if (x >= destination_x && x < destination_x + block_size
        && y >= destination_y && y < destination_y + block_size) {
        return coordinate(
            source_x + x - destination_x,
            source_y + y - destination_y);
    }
    if (x >= source_x && x < source_x + block_size
        && y >= source_y && y < source_y + block_size) {
        return 0xff12'3456u;
    }
    return coordinate(x, y);
}

std::uint32_t coordinate_pixel(std::uint32_t x, std::uint32_t y) noexcept {
    return coordinate_pixel_phase(x, y, 0);
}

LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        const ULONG_PTR value = static_cast<ULONG_PTR>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (value == kPatternPhase0 || value == kPatternPhase1) {
            RECT client{};
            GetClientRect(window, &client);
            const std::uint32_t width = static_cast<std::uint32_t>(client.right);
            const std::uint32_t height = static_cast<std::uint32_t>(client.bottom);
            const std::uint32_t phase = value == kPatternPhase1 ? 1u : 0u;
            try {
                std::vector<std::uint32_t> pixels(
                    static_cast<std::size_t>(width) * height);
                for (std::uint32_t y = 0; y < height; ++y) {
                    for (std::uint32_t x = 0; x < width; ++x) {
                        pixels[static_cast<std::size_t>(y) * width + x] =
                            coordinate_pixel_phase(x, y, phase);
                    }
                }
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
                bitmap.bmiHeader.biWidth = static_cast<LONG>(width);
                bitmap.bmiHeader.biHeight = -static_cast<LONG>(height);
                bitmap.bmiHeader.biPlanes = 1;
                bitmap.bmiHeader.biBitCount = 32;
                bitmap.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(
                    dc,
                    0,
                    0,
                    width,
                    height,
                    0,
                    0,
                    0,
                    height,
                    pixels.data(),
                    &bitmap,
                    DIB_RGB_COLORS);
            } catch (...) {
                FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(
                    GetStockObject(BLACK_BRUSH)));
            }
        } else {
            const COLORREF color = static_cast<COLORREF>(value);
            HBRUSH brush = CreateSolidBrush(color);
            FillRect(dc, &paint.rcPaint, brush);
            DeleteObject(brush);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == kStopMessage) {
        PostQuitMessage(0);
        return 0;
    }
    if (message == kMoveOffscreenMessage) {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        const int width = bounds.right - bounds.left;
        const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        SetWindowPos(
            window,
            nullptr,
            virtual_left - width - 128,
            virtual_top,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (message == kPulseMessage) {
        const ULONG_PTR current = static_cast<ULONG_PTR>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (current == kPatternPhase0 || current == kPatternPhase1) {
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                static_cast<LONG_PTR>(
                    current == kPatternPhase0 ? kPatternPhase1 : kPatternPhase0));
        } else {
            const COLORREF color = static_cast<COLORREF>(current);
            const BYTE red = GetRValue(color) == 220 ? 221 : 220;
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                static_cast<LONG_PTR>(RGB(red, 20, 20)));
        }
        RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        return 0;
    }
    if (message == kSetPatternPhase0Message
        || message == kSetPatternPhase1Message) {
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            static_cast<LONG_PTR>(message == kSetPatternPhase0Message
                    ? kPatternPhase0
                    : kPatternPhase1));
        RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        return 0;
    }
    if (message == kResizePatternSmallMessage
        || message == kResizePatternRestoreMessage) {
        const LONG width = message == kResizePatternSmallMessage
            ? 639 : kPatternWidth;
        const LONG height = message == kResizePatternSmallMessage
            ? 639 : kPatternHeight;
        SetWindowPos(
            window,
            nullptr,
            0,
            0,
            width,
            height,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void window_thread(WindowThreadState* state) {
    ScopedThreadDpiAwareness dpi_awareness;
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &test_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        state->error.store(GetLastError(), std::memory_order_release);
        SetEvent(state->ready);
        return;
    }

    HWND target = CreateWindowExW(
        0,
        kWindowClass,
        L"FluxCap target",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        420,
        320,
        nullptr,
        nullptr,
        window_class.hInstance,
        reinterpret_cast<void*>(static_cast<ULONG_PTR>(RGB(220, 20, 20))));
    HWND cover = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kWindowClass,
        L"FluxCap cover",
        WS_POPUP,
        100,
        100,
        420,
        320,
        nullptr,
        nullptr,
        window_class.hInstance,
        reinterpret_cast<void*>(static_cast<ULONG_PTR>(RGB(20, 20, 220))));
    HWND roi_target = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kWindowClass,
        L"FluxCap ROI coordinate target",
        WS_POPUP,
        600,
        100,
        kPatternWidth,
        kPatternHeight,
        nullptr,
        nullptr,
        window_class.hInstance,
        reinterpret_cast<void*>(kPatternPhase0));
    if (target == nullptr || cover == nullptr || roi_target == nullptr) {
        state->error.store(GetLastError(), std::memory_order_release);
        if (roi_target != nullptr) DestroyWindow(roi_target);
        if (cover != nullptr) DestroyWindow(cover);
        if (target != nullptr) DestroyWindow(target);
        SetEvent(state->ready);
        return;
    }

    ShowWindow(target, SW_SHOW);
    UpdateWindow(target);
    ShowWindow(roi_target, SW_SHOW);
    UpdateWindow(roi_target);
    ShowWindow(cover, SW_SHOW);
    UpdateWindow(cover);
    SetWindowPos(cover, HWND_TOPMOST, 100, 100, 420, 320, SWP_SHOWWINDOW);
    state->target.store(target, std::memory_order_release);
    state->roi_target.store(roi_target, std::memory_order_release);
    SetEvent(state->ready);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(roi_target);
    DestroyWindow(cover);
    DestroyWindow(target);
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint32_t read_bgra_pixel(
    ID3D11Device*,
    ID3D11DeviceContext*,
    ID3D11Texture2D*,
    std::uint32_t,
    std::uint32_t);

std::uint32_t read_center_pixel(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return read_bgra_pixel(
        device,
        context,
        texture,
        description.Width / 2,
        description.Height / 2);
}

std::uint32_t read_bgra_pixel(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    std::uint32_t x,
    std::uint32_t y) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM
        || x >= description.Width
        || y >= description.Height) {
        fail("read_bgra_pixel received an incompatible texture or coordinate");
    }
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(hr)) {
        fail("CreateTexture2D staging failed: " + std::to_string(hr));
    }
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fail("Map staging failed: " + std::to_string(hr));
    }
    const auto* row = static_cast<const std::uint8_t*>(mapped.pData)
        + static_cast<std::size_t>(y) * mapped.RowPitch;
    std::uint32_t pixel = 0;
    std::memcpy(&pixel, row + static_cast<std::size_t>(x) * 4, 4);
    context->Unmap(staging.Get(), 0);
    return pixel;
}

ComPtr<ID3D11Texture2D> create_coordinate_texture(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height) {
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] = coordinate_pixel(x, y);
        }
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = width * sizeof(std::uint32_t);
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, &initial, &texture);
    if (FAILED(hr)) {
        fail("coordinate texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

void verify_center_crop(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    std::uint32_t input_width,
    std::uint32_t input_height,
    std::uint32_t size) {
    gpu::GpuCropConfig config;
    config.input_width = input_width;
    config.input_height = input_height;
    config.x = (input_width - size) / 2u;
    config.y = (input_height - size) / 2u;
    config.width = size;
    config.height = size;

    gpu::GpuCrop crop;
    const auto created = gpu::GpuCrop::create(device, config, crop);
    if (!created) {
        fail(std::string("GPU center crop create failed: ") + created.what());
    }
    const auto processed = crop.process(source);
    if (!processed) {
        fail(std::string("GPU center crop process failed: ") + processed.what());
    }
    const auto repeated = crop.process(source);
    if (!repeated) {
        fail(std::string("repeated GPU center crop failed: ") + repeated.what());
    }
    if (!crop.initialized() || crop.generation() != 2
        || crop.config().x != config.x || crop.config().y != config.y) {
        fail("GPU center crop state is inconsistent");
    }

    D3D11_TEXTURE2D_DESC output{};
    crop.output_texture()->GetDesc(&output);
    if (output.Width != size || output.Height != size
        || output.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        fail("GPU center crop returned the wrong output texture");
    }
    const std::uint32_t first = read_bgra_pixel(
        device, context, crop.output_texture(), 0, 0);
    const std::uint32_t last = read_bgra_pixel(
        device, context, crop.output_texture(), size - 1, size - 1);
    if (first != coordinate_pixel(config.x, config.y)
        || last != coordinate_pixel(
            config.x + size - 1,
            config.y + size - 1)) {
        fail("GPU center crop copied the wrong source region");
    }
}

std::uint16_t read_center_luma(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    const DXGI_FORMAT format = description.Format;
    if (format != DXGI_FORMAT_NV12 && format != DXGI_FORMAT_P010) {
        fail("read_center_luma requires an NV12 or P010 texture");
    }
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(hr)) fail("CreateTexture2D luma staging failed: " + std::to_string(hr));
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) fail("Map luma staging failed: " + std::to_string(hr));
    const auto* row = static_cast<const std::uint8_t*>(mapped.pData)
        + static_cast<std::size_t>(description.Height / 2) * mapped.RowPitch;
    std::uint16_t luma = 0;
    if (format == DXGI_FORMAT_NV12) {
        luma = row[description.Width / 2];
    } else {
        std::uint16_t packed = 0;
        std::memcpy(
            &packed,
            row + static_cast<std::size_t>(description.Width / 2) * sizeof(packed),
            sizeof(packed));
        luma = static_cast<std::uint16_t>(packed >> 6);
    }
    context->Unmap(staging.Get(), 0);
    return luma;
}

struct PlanarReadback final {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_bytes = 0;
    std::vector<std::uint8_t> bytes;
};

PlanarReadback read_planar_texture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_NV12
        && description.Format != DXGI_FORMAT_P010) {
        fail("planar readback requires NV12 or P010");
    }
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(
        &description, nullptr, &staging);
    if (FAILED(hr)) {
        fail("planar staging texture creation failed: "
            + std::to_string(hr));
    }
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fail("planar staging map failed: " + std::to_string(hr));
    }

    PlanarReadback output;
    output.format = description.Format;
    output.width = description.Width;
    output.height = description.Height;
    output.row_bytes = description.Width
        * (description.Format == DXGI_FORMAT_P010 ? 2u : 1u);
    const std::uint32_t rows = description.Height
        + description.Height / 2u;
    output.bytes.resize(
        static_cast<std::size_t>(output.row_bytes) * rows);
    for (std::uint32_t row = 0; row < rows; ++row) {
        std::memcpy(
            output.bytes.data()
                + static_cast<std::size_t>(row) * output.row_bytes,
            static_cast<const std::uint8_t*>(mapped.pData)
                + static_cast<std::size_t>(row) * mapped.RowPitch,
            output.row_bytes);
    }
    context->Unmap(staging.Get(), 0);
    return output;
}

std::uint16_t planar_code_at(
    const PlanarReadback& readback,
    std::uint32_t row,
    std::uint32_t sample) {
    const bool p010 = readback.format == DXGI_FORMAT_P010;
    const std::uint32_t bytes_per_sample = p010 ? 2u : 1u;
    const std::uint32_t rows = readback.height + readback.height / 2u;
    if (row >= rows
        || static_cast<std::uint64_t>(sample + 1u) * bytes_per_sample
            > readback.row_bytes) {
        fail("planar code coordinate is out of bounds");
    }
    const std::size_t offset = static_cast<std::size_t>(row)
            * readback.row_bytes
        + static_cast<std::size_t>(sample) * bytes_per_sample;
    if (!p010) return readback.bytes[offset];

    std::uint16_t packed = 0;
    std::memcpy(&packed, readback.bytes.data() + offset, sizeof(packed));
    if ((packed & 0x3fu) != 0) {
        fail("P010 deterministic code is not packed as code10 << 6");
    }
    return static_cast<std::uint16_t>(packed >> 6u);
}

void test_deterministic_planar_transform(
    ID3D11Device* device,
    ID3D11DeviceContext* context) {
    constexpr std::uint32_t source_width = 4;
    constexpr std::uint32_t source_height = 4;
    constexpr std::uint32_t output_width = 6;
    constexpr std::uint32_t output_height = 8;
    std::array<std::uint32_t, source_width * source_height> source_pixels{};
    for (std::uint32_t y = 0; y < source_height; ++y) {
        for (std::uint32_t x = 0; x < source_width; ++x) {
            const std::uint32_t value = 16u + x * 23u + y * 17u;
            source_pixels[y * source_width + x] = 0xff00'0000u
                | (value << 16u) | (value << 8u) | value;
        }
    }
    D3D11_TEXTURE2D_DESC source_desc{};
    source_desc.Width = source_width;
    source_desc.Height = source_height;
    source_desc.MipLevels = 1;
    source_desc.ArraySize = 1;
    source_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    source_desc.SampleDesc.Count = 1;
    source_desc.Usage = D3D11_USAGE_DEFAULT;
    source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = source_pixels.data();
    initial.SysMemPitch = source_width * sizeof(std::uint32_t);
    ComPtr<ID3D11Texture2D> sampleable_source;
    HRESULT hr = device->CreateTexture2D(
        &source_desc, &initial, &sampleable_source);
    if (FAILED(hr)) {
        fail("deterministic planar source creation failed: "
            + std::to_string(hr));
    }

    source_desc.BindFlags = 0;
    ComPtr<ID3D11Texture2D> unbound_source;
    hr = device->CreateTexture2D(
        &source_desc, &initial, &unbound_source);
    if (FAILED(hr)) {
        fail("deterministic fallback source creation failed: "
            + std::to_string(hr));
    }

    const auto expected_tap = [](std::uint32_t coordinate,
                                 std::uint32_t source_extent,
                                 std::uint32_t output_extent) {
        return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(coordinate) * 2u + 1u)
            * source_extent
            / (static_cast<std::uint64_t>(output_extent) * 2u));
    };
    const auto expected_luma = [](std::uint32_t value, bool p010) {
        const double normalized = static_cast<double>(value) / 255.0;
        return static_cast<std::uint16_t>(std::floor(
            (p010 ? 64.0 + normalized * 876.0
                  : 16.0 + normalized * 219.0)
            + 0.5));
    };

    const auto run_format = [&](gpu::GpuPixelFormat pixel_format,
                                DXGI_FORMAT dxgi_format) {
        gpu::GpuTransformConfig config;
        config.input_width = source_width;
        config.input_height = source_height;
        config.output_width = output_width;
        config.output_height = output_height;
        config.output_format = pixel_format;
        config.output_color_space =
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        config.backend = gpu::GpuTransformBackend::deterministic_planar;
        gpu::GpuTransform transform;
        const auto created = gpu::GpuTransform::create(
            device, config, transform);
        if (!created) {
            fail(std::string("explicit deterministic planar create failed: ")
                + created.what());
        }
        if (transform.active_backend()
                != gpu::GpuTransformBackend::deterministic_planar
            || !transform.spatially_deterministic()) {
            fail("explicit deterministic planar backend was not fixed at creation");
        }
        const auto converted = transform.process(sampleable_source.Get());
        if (!converted || transform.generation() != 1) {
            fail(std::string("deterministic planar draw failed: ")
                + converted.what());
        }
        const PlanarReadback first = read_planar_texture(
            device, context, transform.output_texture());
        if (first.format != dxgi_format
            || first.width != output_width
            || first.height != output_height) {
            fail("deterministic planar output shape is inconsistent");
        }

        D3D11_TEXTURE2D_DESC destination_desc{};
        transform.output_texture()->GetDesc(&destination_desc);
        ComPtr<ID3D11Texture2D> second_destination;
        hr = device->CreateTexture2D(
            &destination_desc, nullptr, &second_destination);
        if (FAILED(hr)) {
            fail("second deterministic planar destination creation failed");
        }
        const auto converted_again = transform.process_into(
            sampleable_source.Get(), second_destination.Get());
        if (!converted_again || transform.generation() != 2) {
            fail("second deterministic planar draw failed");
        }
        const PlanarReadback second = read_planar_texture(
            device, context, second_destination.Get());
        if (first.bytes != second.bytes) {
            fail("deterministic planar bus slots are not byte-identical");
        }

        const bool p010 = dxgi_format == DXGI_FORMAT_P010;
        for (std::uint32_t y = 0; y < output_height; ++y) {
            for (std::uint32_t x = 0; x < output_width; ++x) {
                const std::uint32_t source_x = expected_tap(
                    x, source_width, output_width);
                const std::uint32_t source_y = expected_tap(
                    y, source_height, output_height);
                const std::uint32_t value = 16u
                    + source_x * 23u + source_y * 17u;
                const std::uint16_t expected = expected_luma(value, p010);
                const std::size_t offset =
                    static_cast<std::size_t>(y) * first.row_bytes
                    + x * (p010 ? 2u : 1u);
                if (p010) {
                    std::uint16_t packed = 0;
                    std::memcpy(&packed, first.bytes.data() + offset, 2);
                    if ((packed & 0x3fu) != 0
                        || (packed >> 6u) != expected) {
                        fail("P010 deterministic luma packing/tap mismatch");
                    }
                } else if (first.bytes[offset] != expected) {
                    fail("NV12 deterministic luma tap mismatch");
                }
            }
        }
        const std::size_t uv_offset =
            static_cast<std::size_t>(output_height) * first.row_bytes;
        if (p010) {
            for (std::size_t offset = uv_offset;
                 offset < first.bytes.size();
                 offset += 2) {
                std::uint16_t packed = 0;
                std::memcpy(&packed, first.bytes.data() + offset, 2);
                if ((packed & 0x3fu) != 0 || (packed >> 6u) != 512u) {
                    fail("P010 deterministic chroma is not neutral code10 << 6");
                }
            }
        } else {
            for (std::size_t offset = uv_offset;
                 offset < first.bytes.size();
                 ++offset) {
                if (first.bytes[offset] != 128u) {
                    fail("NV12 deterministic chroma is not neutral");
                }
            }
        }

        auto external_config = config;
        external_config.external_output_only = true;
        gpu::GpuTransform external;
        auto external_result = gpu::GpuTransform::create(
            device, external_config, external);
        if (!external_result
            || external.output_texture() != nullptr
            || external.active_backend()
                != gpu::GpuTransformBackend::deterministic_planar) {
            fail("external-output-only deterministic transform retained an internal output");
        }
        external_result = external.process(sampleable_source.Get());
        if (external_result
            || external_result.status != gpu::GpuStatus::invalid_argument
            || external.generation() != 0) {
            fail("external-output-only transform accepted process()");
        }
        ComPtr<ID3D11Texture2D> external_destination;
        hr = device->CreateTexture2D(
            &destination_desc, nullptr, &external_destination);
        if (FAILED(hr)) {
            fail("external-output-only destination creation failed");
        }
        external_result = external.process_into(
            sampleable_source.Get(), external_destination.Get());
        if (!external_result || external.generation() != 1) {
            fail("external-output-only transform rejected process_into()");
        }
        const PlanarReadback external_pixels = read_planar_texture(
            device, context, external_destination.Get());
        if (external_pixels.bytes != first.bytes) {
            fail("external-output-only deterministic output differs from process()");
        }
    };

    run_format(gpu::GpuPixelFormat::nv12, DXGI_FORMAT_NV12);
    run_format(gpu::GpuPixelFormat::p010, DXGI_FORMAT_P010);

    constexpr std::uint32_t color_width = 4;
    constexpr std::uint32_t color_height = 2;
    constexpr std::uint32_t red = 0xffff'0000u;
    constexpr std::uint32_t blue = 0xff00'00ffu;
    constexpr std::uint32_t black = 0xff00'0000u;
    constexpr std::uint32_t white = 0xffff'ffffu;
    const std::array<std::uint32_t, color_width * color_height> color_pixels{
        red, red, blue, blue,
        red, red, blue, blue};
    const std::array<std::uint32_t, color_width * color_height> endpoint_pixels{
        black, black, white, white,
        black, black, white, white};

    const auto create_color_source = [&] (
        const std::array<std::uint32_t, color_width * color_height>& pixels,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = color_width;
        description.Height = color_height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = color_width * sizeof(std::uint32_t);
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT created = device->CreateTexture2D(
            &description, &data, &texture);
        if (FAILED(created)) {
            fail("colored deterministic source creation failed: "
                + std::to_string(created));
        }
        return texture;
    };
    const auto color_source = create_color_source(color_pixels);
    const auto endpoint_source = create_color_source(endpoint_pixels);

    struct ExpectedColorCode final {
        std::uint16_t y = 0;
        std::uint16_t u = 0;
        std::uint16_t v = 0;
    };
    const auto expected_color_code = [](
        double r,
        double g,
        double b,
        bool p010,
        bool full_range) {
        const double y = r * 0.2126 + g * 0.7152 + b * 0.0722;
        const double cb = (b - y) / 1.8556;
        const double cr = (r - y) / 1.5748;
        const auto rounded = [](double value) {
            return static_cast<std::uint16_t>(std::floor(value + 0.5));
        };
        ExpectedColorCode result;
        if (p010) {
            result.y = rounded(full_range
                ? y * 1023.0
                : 64.0 + y * 876.0);
            result.u = rounded(full_range
                ? (cb + 0.5) * 1023.0
                : 512.0 + cb * 896.0);
            result.v = rounded(full_range
                ? (cr + 0.5) * 1023.0
                : 512.0 + cr * 896.0);
        } else {
            result.y = rounded(full_range
                ? y * 255.0
                : 16.0 + y * 219.0);
            result.u = rounded(full_range
                ? (cb + 0.5) * 255.0
                : 128.0 + cb * 224.0);
            result.v = rounded(full_range
                ? (cr + 0.5) * 255.0
                : 128.0 + cr * 224.0);
        }
        return result;
    };

    const auto verify_color_matrix = [&] (
        gpu::GpuPixelFormat pixel_format,
        DXGI_FORMAT dxgi_format,
        bool full_range) {
        gpu::GpuTransformConfig config;
        config.input_width = color_width;
        config.input_height = color_height;
        config.output_width = color_width;
        config.output_height = color_height;
        config.output_format = pixel_format;
        config.full_range_yuv = full_range;
        config.backend = gpu::GpuTransformBackend::deterministic_planar;
        gpu::GpuTransform transform;
        auto converted = gpu::GpuTransform::create(device, config, transform);
        if (!converted) {
            fail(std::string("colored deterministic transform create failed: ")
                + converted.what());
        }
        converted = transform.process(color_source.Get());
        if (!converted) {
            fail(std::string("colored deterministic transform failed: ")
                + converted.what());
        }
        const PlanarReadback readback = read_planar_texture(
            device, context, transform.output_texture());
        if (readback.format != dxgi_format) {
            fail("colored deterministic transform returned the wrong format");
        }

        const bool p010 = dxgi_format == DXGI_FORMAT_P010;
        const ExpectedColorCode expected_red = expected_color_code(
            1.0, 0.0, 0.0, p010, full_range);
        const ExpectedColorCode expected_blue = expected_color_code(
            0.0, 0.0, 1.0, p010, full_range);
        const std::uint16_t red_y = planar_code_at(readback, 0, 0);
        const std::uint16_t blue_y = planar_code_at(readback, 0, 2);
        const std::uint16_t red_u = planar_code_at(
            readback, color_height, 0);
        const std::uint16_t red_v = planar_code_at(
            readback, color_height, 1);
        const std::uint16_t blue_u = planar_code_at(
            readback, color_height, 2);
        const std::uint16_t blue_v = planar_code_at(
            readback, color_height, 3);
        if (red_y != expected_red.y
            || red_u != expected_red.u
            || red_v != expected_red.v
            || blue_y != expected_blue.y
            || blue_u != expected_blue.u
            || blue_v != expected_blue.v) {
            fail("deterministic RGB primaries do not match the BT.709 matrix");
        }
        if (red_v <= red_u || blue_u <= blue_v) {
            fail("deterministic R/B primaries lost their U/V orientation");
        }

        if (!full_range) return;
        converted = transform.process(endpoint_source.Get());
        if (!converted) {
            fail("full-range deterministic endpoint conversion failed");
        }
        const PlanarReadback endpoints = read_planar_texture(
            device, context, transform.output_texture());
        const std::uint16_t maximum = p010 ? 1023u : 255u;
        const std::uint16_t neutral = p010 ? 512u : 128u;
        if (planar_code_at(endpoints, 0, 0) != 0
            || planar_code_at(endpoints, 0, 2) != maximum
            || planar_code_at(endpoints, color_height, 0) != neutral
            || planar_code_at(endpoints, color_height, 1) != neutral
            || planar_code_at(endpoints, color_height, 2) != neutral
            || planar_code_at(endpoints, color_height, 3) != neutral) {
            fail("deterministic full-range planar endpoint codes are incorrect");
        }
    };

    verify_color_matrix(
        gpu::GpuPixelFormat::nv12, DXGI_FORMAT_NV12, false);
    verify_color_matrix(
        gpu::GpuPixelFormat::p010, DXGI_FORMAT_P010, false);
    verify_color_matrix(
        gpu::GpuPixelFormat::nv12, DXGI_FORMAT_NV12, true);
    verify_color_matrix(
        gpu::GpuPixelFormat::p010, DXGI_FORMAT_P010, true);

    gpu::GpuTransformConfig automatic_config;
    automatic_config.input_width = source_width;
    automatic_config.input_height = source_height;
    automatic_config.output_width = output_width;
    automatic_config.output_height = output_height;
    automatic_config.output_format = gpu::GpuPixelFormat::nv12;
    automatic_config.output_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    gpu::GpuTransform locked;
    auto result = gpu::GpuTransform::create(
        device, automatic_config, locked);
    if (!result
        || locked.active_backend() != gpu::GpuTransformBackend::automatic) {
        fail("automatic deterministic candidate was not left unselected");
    }
    result = locked.process(sampleable_source.Get());
    if (!result
        || locked.active_backend()
            != gpu::GpuTransformBackend::deterministic_planar
        || !locked.spatially_deterministic()) {
        fail("automatic backend did not select deterministic planar");
    }
    const auto locked_rejection = locked.process(unbound_source.Get());
    if (locked_rejection
        || locked_rejection.status != gpu::GpuStatus::unsupported
        || locked.active_backend()
            != gpu::GpuTransformBackend::deterministic_planar) {
        fail("selected deterministic backend switched within its epoch");
    }

    gpu::GpuTransform fallback;
    result = gpu::GpuTransform::create(
        device, automatic_config, fallback);
    if (!result) fail("automatic fallback transform create failed");
    result = fallback.process(unbound_source.Get());
    if (!result
        || fallback.active_backend()
            != gpu::GpuTransformBackend::video_processor
        || fallback.spatially_deterministic()) {
        fail("automatic source-SRV failure did not permanently select VP");
    }

    automatic_config.backend =
        gpu::GpuTransformBackend::deterministic_planar;
    gpu::GpuTransform explicit_failure;
    result = gpu::GpuTransform::create(
        device, automatic_config, explicit_failure);
    if (!result) fail("explicit deterministic fallback test create failed");
    const auto rejected = explicit_failure.process(unbound_source.Get());
    if (rejected
        || rejected.status != gpu::GpuStatus::unsupported
        || explicit_failure.generation() != 0
        || explicit_failure.active_backend()
            != gpu::GpuTransformBackend::deterministic_planar) {
        fail("explicit deterministic mode did not fail closed without an SRV");
    }

    const auto srgb_source = create_color_source(
        color_pixels, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    gpu::GpuTransformConfig srgb_config;
    srgb_config.input_width = color_width;
    srgb_config.input_height = color_height;
    srgb_config.output_width = color_width;
    srgb_config.output_height = color_height;
    // The capture epoch advertises nonlinear UNORM P709, while an individual
    // source may be a fully typed SRGB resource. validate_resources accepts
    // this pairing, but deterministic must not reinterpret that resource as
    // an UNORM SRV.
    srgb_config.input_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srgb_config.output_format = gpu::GpuPixelFormat::nv12;
    srgb_config.backend = gpu::GpuTransformBackend::deterministic_planar;
    gpu::GpuTransform srgb_explicit;
    result = gpu::GpuTransform::create(device, srgb_config, srgb_explicit);
    if (!result) {
        fail(std::string("explicit typed SRGB transform create failed: ")
            + result.what());
    }
    result = srgb_explicit.process(srgb_source.Get());
    if (result
        || result.status != gpu::GpuStatus::unsupported
        || srgb_explicit.generation() != 0
        || srgb_explicit.active_backend()
            != gpu::GpuTransformBackend::deterministic_planar) {
        fail("explicit deterministic mode did not reject a typed SRGB source");
    }

    srgb_config.backend = gpu::GpuTransformBackend::automatic;
    gpu::GpuTransform srgb_automatic;
    result = gpu::GpuTransform::create(device, srgb_config, srgb_automatic);
    if (!result) {
        fail(std::string("automatic typed SRGB transform create failed: ")
            + result.what());
    }
    if (srgb_automatic.active_backend()
        != gpu::GpuTransformBackend::automatic) {
        fail("automatic typed SRGB transform selected a backend before submit");
    }
    result = srgb_automatic.process(srgb_source.Get());
    if (!result) {
        fail(std::string("automatic typed SRGB VideoProcessor conversion failed: ")
            + result.what() + " (HRESULT "
            + std::to_string(result.hresult) + ")");
    }
    if (srgb_automatic.generation() != 1
        || srgb_automatic.compatibility_copy_submissions() != 1
        || srgb_automatic.active_backend()
            != gpu::GpuTransformBackend::video_processor) {
        fail("automatic typed SRGB conversion did not lock VP and disclose its compatibility copy");
    }

    gpu::GpuTransformConfig state_config = automatic_config;
    state_config.backend = gpu::GpuTransformBackend::deterministic_planar;
    gpu::GpuTransform state_transform;
    result = gpu::GpuTransform::create(device, state_config, state_transform);
    if (!result) {
        fail(std::string("context-state transform create failed: ")
            + result.what());
    }

    constexpr char sentinel_shader_source[] = R"hlsl(
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    return float4(0.0, 0.0, 0.0, 1.0);
}
float4 ps_main() : SV_Target {
    return float4(1.0, 0.0, 1.0, 1.0);
}
)hlsl";
    ComPtr<ID3DBlob> sentinel_vs_bytecode;
    ComPtr<ID3DBlob> sentinel_ps_bytecode;
    hr = D3DCompile(
        sentinel_shader_source,
        std::strlen(sentinel_shader_source),
        nullptr,
        nullptr,
        nullptr,
        "vs_main",
        "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &sentinel_vs_bytecode,
        nullptr);
    if (SUCCEEDED(hr)) {
        hr = D3DCompile(
            sentinel_shader_source,
            std::strlen(sentinel_shader_source),
            nullptr,
            nullptr,
            nullptr,
            "ps_main",
            "ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            &sentinel_ps_bytecode,
            nullptr);
    }
    ComPtr<ID3D11VertexShader> sentinel_vs;
    ComPtr<ID3D11PixelShader> sentinel_ps;
    if (SUCCEEDED(hr)) {
        hr = device->CreateVertexShader(
            sentinel_vs_bytecode->GetBufferPointer(),
            sentinel_vs_bytecode->GetBufferSize(),
            nullptr,
            &sentinel_vs);
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(
            sentinel_ps_bytecode->GetBufferPointer(),
            sentinel_ps_bytecode->GetBufferSize(),
            nullptr,
            &sentinel_ps);
    }
    if (FAILED(hr)) {
        fail("caller-state sentinel shader creation failed: "
            + std::to_string(hr));
    }

    D3D11_BUFFER_DESC sentinel_buffer_desc{};
    sentinel_buffer_desc.ByteWidth = 16;
    sentinel_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    sentinel_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> sentinel_buffer;
    hr = device->CreateBuffer(
        &sentinel_buffer_desc, nullptr, &sentinel_buffer);
    if (FAILED(hr)) fail("caller-state sentinel buffer creation failed");

    ComPtr<ID3D11ShaderResourceView> sentinel_srv;
    hr = device->CreateShaderResourceView(
        sampleable_source.Get(), nullptr, &sentinel_srv);
    if (FAILED(hr)) fail("caller-state sentinel SRV creation failed");

    D3D11_TEXTURE2D_DESC sentinel_target_desc{};
    sentinel_target_desc.Width = 2;
    sentinel_target_desc.Height = 2;
    sentinel_target_desc.MipLevels = 1;
    sentinel_target_desc.ArraySize = 1;
    sentinel_target_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sentinel_target_desc.SampleDesc.Count = 1;
    sentinel_target_desc.Usage = D3D11_USAGE_DEFAULT;
    sentinel_target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> sentinel_target;
    ComPtr<ID3D11RenderTargetView> sentinel_rtv;
    hr = device->CreateTexture2D(
        &sentinel_target_desc, nullptr, &sentinel_target);
    if (SUCCEEDED(hr)) {
        hr = device->CreateRenderTargetView(
            sentinel_target.Get(), nullptr, &sentinel_rtv);
    }
    if (FAILED(hr)) fail("caller-state sentinel RTV creation failed");

    constexpr D3D11_PRIMITIVE_TOPOLOGY sentinel_topology =
        D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    const D3D11_VIEWPORT sentinel_viewport{
        3.25f, 5.5f, 17.0f, 19.0f, 0.125f, 0.875f};
    context->ClearState();
    context->IASetPrimitiveTopology(sentinel_topology);
    context->VSSetShader(sentinel_vs.Get(), nullptr, 0);
    ID3D11Buffer* sentinel_buffer_pointer = sentinel_buffer.Get();
    context->PSSetConstantBuffers(0, 1, &sentinel_buffer_pointer);
    ID3D11ShaderResourceView* sentinel_srv_pointer = sentinel_srv.Get();
    context->PSSetShaderResources(0, 1, &sentinel_srv_pointer);
    context->PSSetShader(sentinel_ps.Get(), nullptr, 0);
    context->RSSetViewports(1, &sentinel_viewport);
    ID3D11RenderTargetView* sentinel_rtv_pointer = sentinel_rtv.Get();
    context->OMSetRenderTargets(1, &sentinel_rtv_pointer, nullptr);

    result = state_transform.process(sampleable_source.Get());
    if (!result) {
        context->ClearState();
        fail(std::string("context-state deterministic transform failed: ")
            + result.what());
    }

    D3D11_PRIMITIVE_TOPOLOGY restored_topology =
        D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&restored_topology);
    ComPtr<ID3D11VertexShader> restored_vs;
    context->VSGetShader(&restored_vs, nullptr, nullptr);
    ComPtr<ID3D11Buffer> restored_buffer;
    context->PSGetConstantBuffers(0, 1, &restored_buffer);
    ComPtr<ID3D11ShaderResourceView> restored_srv;
    context->PSGetShaderResources(0, 1, &restored_srv);
    ComPtr<ID3D11PixelShader> restored_ps;
    context->PSGetShader(&restored_ps, nullptr, nullptr);
    UINT restored_viewport_count = 1;
    D3D11_VIEWPORT restored_viewport{};
    context->RSGetViewports(&restored_viewport_count, &restored_viewport);
    ComPtr<ID3D11RenderTargetView> restored_rtv;
    context->OMGetRenderTargets(1, &restored_rtv, nullptr);
    const bool viewport_restored = restored_viewport_count == 1
        && restored_viewport.TopLeftX == sentinel_viewport.TopLeftX
        && restored_viewport.TopLeftY == sentinel_viewport.TopLeftY
        && restored_viewport.Width == sentinel_viewport.Width
        && restored_viewport.Height == sentinel_viewport.Height
        && restored_viewport.MinDepth == sentinel_viewport.MinDepth
        && restored_viewport.MaxDepth == sentinel_viewport.MaxDepth;
    const bool caller_state_restored =
        restored_topology == sentinel_topology
        && restored_vs.Get() == sentinel_vs.Get()
        && restored_buffer.Get() == sentinel_buffer.Get()
        && restored_srv.Get() == sentinel_srv.Get()
        && restored_ps.Get() == sentinel_ps.Get()
        && viewport_restored
        && restored_rtv.Get() == sentinel_rtv.Get();
    context->ClearState();
    if (!caller_state_restored) {
        fail("deterministic draw did not restore the caller graphics context state");
    }

    auto concurrent_config = state_config;
    concurrent_config.external_output_only = true;
    gpu::GpuTransform concurrent_transform;
    result = gpu::GpuTransform::create(
        device, concurrent_config, concurrent_transform);
    if (!result) {
        fail(std::string("concurrent deterministic transform create failed: ")
            + result.what());
    }
    D3D11_TEXTURE2D_DESC concurrent_destination_desc{};
    concurrent_destination_desc.Width = concurrent_config.output_width;
    concurrent_destination_desc.Height = concurrent_config.output_height;
    concurrent_destination_desc.MipLevels = 1;
    concurrent_destination_desc.ArraySize = 1;
    concurrent_destination_desc.Format = DXGI_FORMAT_NV12;
    concurrent_destination_desc.SampleDesc.Count = 1;
    concurrent_destination_desc.Usage = D3D11_USAGE_DEFAULT;
    concurrent_destination_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> concurrent_baseline;
    hr = device->CreateTexture2D(
        &concurrent_destination_desc, nullptr, &concurrent_baseline);
    if (FAILED(hr)) fail("concurrent baseline destination creation failed");
    result = concurrent_transform.process_into(
        sampleable_source.Get(), concurrent_baseline.Get());
    if (!result) {
        fail(std::string("concurrent baseline draw failed: ")
            + result.what());
    }
    const PlanarReadback concurrent_expected = read_planar_texture(
        device, context, concurrent_baseline.Get());

    constexpr std::size_t concurrent_output_count = 24;
    std::array<ComPtr<ID3D11Texture2D>, concurrent_output_count>
        concurrent_outputs;
    for (auto& output : concurrent_outputs) {
        hr = device->CreateTexture2D(
            &concurrent_destination_desc, nullptr, &output);
        if (FAILED(hr)) fail("concurrent destination creation failed");
    }

    std::atomic<bool> stop_perturbing{false};
    std::atomic<std::uint64_t> perturbation_count{0};
    std::thread perturb_context([&] {
        const D3D11_VIEWPORT disruptive_viewport{
            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        ID3D11Buffer* null_buffer = nullptr;
        ID3D11ShaderResourceView* null_srv = nullptr;
        while (!stop_perturbing.load(std::memory_order_acquire)) {
            context->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->VSSetShader(nullptr, nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &null_buffer);
            context->PSSetShaderResources(0, 1, &null_srv);
            context->PSSetShader(nullptr, nullptr, 0);
            context->RSSetViewports(1, &disruptive_viewport);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            perturbation_count.fetch_add(1, std::memory_order_release);
        }
    });
    while (perturbation_count.load(std::memory_order_acquire) < 64) {
        std::this_thread::yield();
    }

    gpu::GpuError concurrent_error;
    for (auto& output : concurrent_outputs) {
        concurrent_error = concurrent_transform.process_into(
            sampleable_source.Get(), output.Get());
        if (!concurrent_error) break;
    }
    stop_perturbing.store(true, std::memory_order_release);
    perturb_context.join();
    context->ClearState();
    if (!concurrent_error) {
        fail(std::string("shared-context deterministic draw failed: ")
            + concurrent_error.what());
    }
    if (perturbation_count.load(std::memory_order_acquire) < 64
        || concurrent_transform.generation()
            != concurrent_output_count + 1u) {
        fail("shared-context stress did not execute every deterministic draw");
    }
    for (const auto& output : concurrent_outputs) {
        const PlanarReadback actual = read_planar_texture(
            device, context, output.Get());
        if (actual.bytes != concurrent_expected.bytes) {
            fail("shared immediate-context state polluted deterministic output");
        }
    }
}

D3D11_TEXTURE2D_DESC require_transform_output(
    const gpu::GpuTransform& transform,
    DXGI_FORMAT expected_format,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    const char* name) {
    ID3D11Texture2D* texture = transform.output_texture();
    if (texture == nullptr) {
        fail(std::string(name) + " transform returned a null output texture");
    }

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != expected_format
        || description.Width != expected_width
        || description.Height != expected_height) {
        fail(std::string(name) + " transform returned the wrong texture format or dimensions");
    }
    return description;
}

bool read_exact(HANDLE pipe, void* output, std::size_t size) noexcept {
    auto* cursor = static_cast<std::uint8_t*>(output);
    while (size != 0) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            size, std::numeric_limits<DWORD>::max()));
        if (!ReadFile(pipe, cursor, chunk, &read, nullptr) || read == 0) return false;
        cursor += read;
        size -= read;
    }
    return true;
}

bool write_exact(HANDLE pipe, const void* input, std::size_t size) noexcept {
    const auto* cursor = static_cast<const std::uint8_t*>(input);
    while (size != 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            size, std::numeric_limits<DWORD>::max()));
        if (!WriteFile(pipe, cursor, chunk, &written, nullptr) || written == 0) return false;
        cursor += written;
        size -= written;
    }
    return true;
}

HANDLE parse_inherited_handle(const wchar_t* text) {
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (end == text || *end != L'\0'
        || value > std::numeric_limits<std::uintptr_t>::max()) {
        return nullptr;
    }
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
}

template <class SharedMetadata>
HRESULT create_device_for_adapter(
    const SharedMetadata& metadata,
    ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context) noexcept {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IDXGIAdapter1> selected;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return hr;
        DXGI_ADAPTER_DESC1 description{};
        hr = candidate->GetDesc1(&description);
        if (FAILED(hr)) return hr;
        if (description.AdapterLuid.LowPart == metadata.adapter_luid_low
            && description.AdapterLuid.HighPart == metadata.adapter_luid_high) {
            selected = std::move(candidate);
            break;
        }
    }
    if (selected == nullptr) return DXGI_ERROR_NOT_FOUND;

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL created_level{};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
        | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    hr = D3D11CreateDevice(
        selected.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
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
            selected.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &device,
            &created_level,
            &context);
    }
    return hr;
}

int run_shared_consumer_child(HANDLE input, HANDLE output) noexcept {
    ChildConsumerResult result;
    try {
        gpu::SharedTextureExport metadata;
        if (!read_exact(input, &metadata, sizeof(metadata))) {
            fail("child failed to receive shared texture metadata");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const HRESULT device_hr = create_device_for_adapter(metadata, device, context);
        if (FAILED(device_hr)) {
            result.hresult = device_hr;
            fail("child failed to create a D3D11 device on the exported adapter");
        }
        gpu::SharedTextureConsumer consumer;
        const auto opened = gpu::SharedTextureConsumer::open(
            device.Get(), metadata, true, consumer);
        if (!opened) {
            result.hresult = opened.hresult;
            fail(std::string("child failed to open shared texture: ") + opened.what());
        }
        const auto acquired = consumer.acquire(2'000);
        if (!acquired) {
            result.hresult = acquired.hresult;
            fail(std::string("child failed to acquire shared texture: ") + acquired.what());
        }
        result.pixel = read_center_pixel(device.Get(), context.Get(), consumer.texture());
        const auto released = consumer.release();
        if (!released) {
            result.hresult = released.hresult;
            fail(std::string("child failed to release shared texture: ") + released.what());
        }
        result.success = 1;
        result.hresult = S_OK;
        (void)strncpy_s(result.message, "ok", _TRUNCATE);
    } catch (const std::exception& error) {
        (void)strncpy_s(result.message, error.what(), _TRUNCATE);
    }
    const bool sent = write_exact(output, &result, sizeof(result));
    CloseHandle(input);
    CloseHandle(output);
    return sent && result.success != 0 ? 0 : 1;
}

std::uint32_t consume_shared_texture_in_child(
    gpu::SharedTexturePublisher& publisher) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE child_input_raw = nullptr;
    HANDLE parent_output_raw = nullptr;
    if (!CreatePipe(&child_input_raw, &parent_output_raw, &security, 0)) {
        fail("CreatePipe for child input failed");
    }
    ScopedHandle child_input(child_input_raw);
    ScopedHandle parent_output(parent_output_raw);
    HANDLE parent_input_raw = nullptr;
    HANDLE child_output_raw = nullptr;
    if (!CreatePipe(&parent_input_raw, &child_output_raw, &security, 0)) {
        fail("CreatePipe for child output failed");
    }
    ScopedHandle parent_input(parent_input_raw);
    ScopedHandle child_output(child_output_raw);
    if (!SetHandleInformation(parent_output.get(), HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(parent_input.get(), HANDLE_FLAG_INHERIT, 0)) {
        fail("SetHandleInformation failed");
    }

    std::vector<wchar_t> executable(32'768);
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        fail("GetModuleFileName failed");
    }
    std::wstring command_line = L"\"" + std::wstring(executable.data(), length)
        + L"\" --shared-consumer "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(child_input.get()))
        + L" "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(child_output.get()));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        fail("CreateProcess for shared texture consumer failed");
    }
    ScopedHandle process_handle(process.hProcess);
    ScopedHandle thread_handle(process.hThread);
    child_input.reset();
    child_output.reset();

    gpu::SharedTextureExport metadata;
    const auto exported = publisher.export_to_process(process_handle.get(), metadata);
    if (!exported) {
        fail(std::string("cross-process shared texture export failed: ") + exported.what());
    }
    if (!write_exact(parent_output.get(), &metadata, sizeof(metadata))) {
        fail("failed to send shared texture metadata to child");
    }
    parent_output.reset();

    const DWORD waited = WaitForSingleObject(process_handle.get(), 10'000);
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(process_handle.get(), 2);
        WaitForSingleObject(process_handle.get(), 1'000);
        fail("shared texture consumer child timed out");
    }
    if (waited != WAIT_OBJECT_0) fail("waiting for shared texture child failed");

    ChildConsumerResult result;
    if (!read_exact(parent_input.get(), &result, sizeof(result))
        || result.structure_size != sizeof(result)) {
        fail("shared texture child returned no valid result");
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)
        || exit_code != 0 || result.success == 0) {
        fail(std::string("shared texture child failed: ") + result.message
            + " (hr=" + std::to_string(result.hresult) + ")");
    }
    return result.pixel;
}

BusChildResponse bus_child_success(BusChildOperation operation) noexcept {
    BusChildResponse response;
    response.operation = operation;
    response.success = 1;
    response.status = gpu::GpuStatus::ok;
    response.hresult = S_OK;
    (void)strncpy_s(response.message, "ok", _TRUNCATE);
    return response;
}

BusChildResponse bus_child_error(
    BusChildOperation operation,
    const gpu::GpuError& error) noexcept {
    BusChildResponse response;
    response.operation = operation;
    response.status = error.status;
    response.hresult = error.hresult;
    (void)strncpy_s(response.message, error.what(), _TRUNCATE);
    return response;
}

BusChildResponse bus_child_exception(
    BusChildOperation operation,
    const char* message) noexcept {
    BusChildResponse response;
    response.operation = operation;
    (void)strncpy_s(response.message, message, _TRUNCATE);
    return response;
}

void validate_bus_roi(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    const BusChildCommand& command) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Width != command.width
        || description.Height != command.height
        || description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        fail("WGC direct-bus child received the wrong texture shape");
    }
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(hr)) {
        fail("WGC direct-bus child staging texture creation failed: "
            + std::to_string(hr));
    }
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fail("WGC direct-bus child staging Map failed: " + std::to_string(hr));
    }
    bool mismatch = false;
    std::uint32_t mismatch_x = 0;
    std::uint32_t mismatch_y = 0;
    std::uint32_t actual = 0;
    std::uint32_t expected = 0;
    for (std::uint32_t y = 0; y < command.height && !mismatch; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch);
        for (std::uint32_t x = 0; x < command.width; ++x) {
            expected = coordinate_pixel_phase(
                command.source_x + x, command.source_y + y, command.phase);
            if (row[x] != expected) {
                mismatch = true;
                mismatch_x = x;
                mismatch_y = y;
                actual = row[x];
                break;
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    if (mismatch) {
        fail("WGC direct-bus ROI mismatch at ("
            + std::to_string(mismatch_x) + ',' + std::to_string(mismatch_y)
            + "): expected=" + std::to_string(expected)
            + ", actual=" + std::to_string(actual));
    }
}

int run_bus_consumer_child(HANDLE input, HANDLE output) noexcept {
    gpu::SharedFrameBusFrameLease held;
    BusChildOperation operation = BusChildOperation::open;
    try {
        gpu::SharedFrameBusRegistration registration;
        if (!read_exact(input, &registration, sizeof(registration))) {
            fail("WGC direct-bus child did not receive its registration");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const HRESULT device_hr = create_device_for_adapter(
            registration, device, context);
        if (FAILED(device_hr)) {
            fail("WGC direct-bus child could not create the registered adapter device");
        }
        gpu::SharedFrameBusConsumer consumer;
        const gpu::GpuError opened = gpu::SharedFrameBusConsumer::open(
            device.Get(), registration, true, consumer);
        BusChildResponse response = opened
            ? bus_child_success(operation)
            : bus_child_error(operation, opened);
        if (!write_exact(output, &response, sizeof(response))) return 3;
        if (!opened) return 4;

        for (;;) {
            BusChildCommand command;
            if (!read_exact(input, &command, sizeof(command))) return 0;
            operation = command.operation;
            if (command.structure_size != sizeof(command)) {
                response = bus_child_exception(operation, "malformed bus child command");
            } else {
                try {
                    switch (operation) {
                    case BusChildOperation::acquire_validate: {
                        if (held) fail("WGC direct-bus child already holds a frame");
                        gpu::SharedFrameBusFrameLease next;
                        const gpu::GpuError acquired = consumer.acquire_latest(
                            command.timeout_ms, next);
                        if (!acquired) {
                            response = bus_child_error(operation, acquired);
                            break;
                        }
                        validate_bus_roi(
                            device.Get(), context.Get(), next.texture(), command);
                        held = std::move(next);
                        response = bus_child_success(operation);
                        response.sequence = held.info().sequence;
                        response.metadata = held.metadata();
                        break;
                    }
                    case BusChildOperation::release: {
                        const gpu::GpuError released = consumer.release(held);
                        response = released
                            ? bus_child_success(operation)
                            : bus_child_error(operation, released);
                        break;
                    }
                    case BusChildOperation::close: {
                        const gpu::GpuError closed = consumer.close();
                        response = closed
                            ? bus_child_success(operation)
                            : bus_child_error(operation, closed);
                        break;
                    }
                    case BusChildOperation::exit_process:
                        response = bus_child_success(operation);
                        if (!write_exact(output, &response, sizeof(response))) return 5;
                        CloseHandle(input);
                        CloseHandle(output);
                        return 0;
                    default:
                        response = bus_child_exception(
                            operation, "unsupported bus child command");
                        break;
                    }
                } catch (const std::exception& error) {
                    response = bus_child_exception(operation, error.what());
                }
            }
            if (!write_exact(output, &response, sizeof(response))) return 6;
        }
    } catch (const std::exception& error) {
        const BusChildResponse response = bus_child_exception(
            operation, error.what());
        (void)write_exact(output, &response, sizeof(response));
        CloseHandle(input);
        CloseHandle(output);
        return 2;
    }
}

class BusConsumerChild final {
public:
    BusConsumerChild() { spawn(); }
    ~BusConsumerChild() { cleanup(); }
    BusConsumerChild(const BusConsumerChild&) = delete;
    BusConsumerChild& operator=(const BusConsumerChild&) = delete;

    [[nodiscard]] HANDLE process() const noexcept { return process_.get(); }

    gpu::SharedFrameBusRegistration attach(
        gpu::SharedFrameBusPublisher& publisher) {
        gpu::SharedFrameBusRegistration registration;
        const gpu::GpuError registered = publisher.register_consumer(
            process_.get(), registration);
        if (!registered) {
            fail(std::string("register WGC direct-bus child failed: ")
                + registered.what());
        }
        send(registration);
        require_success(receive(8'000), BusChildOperation::open, "open");
        registration_ = registration;
        attached_ = true;
        return registration;
    }

    std::uint64_t acquire_validate(
        std::uint32_t source_x,
        std::uint32_t source_y,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t phase) {
        BusChildCommand command;
        command.operation = BusChildOperation::acquire_validate;
        command.timeout_ms = 5'000;
        command.source_x = source_x;
        command.source_y = source_y;
        command.width = width;
        command.height = height;
        command.phase = phase;
        const BusChildResponse response = transact(command);
        require_success(response, command.operation, "acquire/validate");
        if (response.sequence == 0) {
            fail("WGC direct-bus child acquired sequence zero");
        }
        last_metadata_ = response.metadata;
        return response.sequence;
    }

    [[nodiscard]] const gpu::SharedFrameBusFrameMetadata& metadata() const noexcept {
        return last_metadata_;
    }

    void release() {
        BusChildCommand command;
        command.operation = BusChildOperation::release;
        require_success(transact(command), command.operation, "release");
    }

    void close_and_unregister(gpu::SharedFrameBusPublisher& publisher) {
        close_consumer();
        if (attached_) {
            const gpu::GpuError unregistered = publisher.unregister_consumer(
                registration_, 3'000);
            if (!unregistered) {
                fail(std::string("unregister WGC direct-bus child failed: ")
                    + unregistered.what());
            }
            attached_ = false;
        }
        exit_child();
    }

    void close_after_publisher_wrapper_destroyed() {
        close_consumer();
        attached_ = false;
        exit_child();
    }

private:
    template <class T>
    void send(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!write_exact(output_.get(), &value, sizeof(value))) {
            fail("failed to send a command to WGC direct-bus child");
        }
    }

    BusChildResponse receive(std::uint32_t timeout_ms) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(
                    input_.get(), nullptr, 0, nullptr, &available, nullptr)) {
                fail("WGC direct-bus child response pipe closed");
            }
            if (available >= sizeof(BusChildResponse)) {
                BusChildResponse response;
                if (!read_exact(input_.get(), &response, sizeof(response))) {
                    fail("failed to read WGC direct-bus child response");
                }
                return response;
            }
            if (WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0) {
                fail("WGC direct-bus child exited before responding");
            }
            if (GetTickCount64() >= deadline) {
                fail("WGC direct-bus child response timed out");
            }
            Sleep(1);
        }
    }

    BusChildResponse transact(const BusChildCommand& command) {
        send(command);
        return receive(8'000);
    }

    static void require_success(
        const BusChildResponse& response,
        BusChildOperation operation,
        const char* label) {
        if (response.structure_size != sizeof(response)
            || response.operation != operation
            || response.success == 0
            || response.status != gpu::GpuStatus::ok) {
            fail(std::string("WGC direct-bus child ") + label + " failed: "
                + response.message + " (status="
                + std::to_string(static_cast<unsigned>(response.status))
                + ", hr=" + std::to_string(response.hresult) + ')');
        }
    }

    void close_consumer() {
        BusChildCommand command;
        command.operation = BusChildOperation::close;
        require_success(transact(command), command.operation, "close");
    }

    void exit_child() {
        BusChildCommand command;
        command.operation = BusChildOperation::exit_process;
        require_success(transact(command), command.operation, "exit");
        const DWORD waited = WaitForSingleObject(process_.get(), 3'000);
        if (waited != WAIT_OBJECT_0) {
            fail("WGC direct-bus child did not exit");
        }
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_.get(), &exit_code) || exit_code != 0) {
            fail("WGC direct-bus child returned a failure exit code");
        }
        process_.reset();
        thread_.reset();
        input_.reset();
        output_.reset();
    }

    void spawn() {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE child_input_raw = nullptr;
        HANDLE parent_output_raw = nullptr;
        if (!CreatePipe(
                &child_input_raw, &parent_output_raw, &security, 0)) {
            fail("CreatePipe for WGC direct-bus child input failed");
        }
        ScopedHandle child_input(child_input_raw);
        output_.reset(parent_output_raw);
        HANDLE parent_input_raw = nullptr;
        HANDLE child_output_raw = nullptr;
        if (!CreatePipe(
                &parent_input_raw, &child_output_raw, &security, 0)) {
            fail("CreatePipe for WGC direct-bus child output failed");
        }
        input_.reset(parent_input_raw);
        ScopedHandle child_output(child_output_raw);
        if (!SetHandleInformation(output_.get(), HANDLE_FLAG_INHERIT, 0)
            || !SetHandleInformation(input_.get(), HANDLE_FLAG_INHERIT, 0)) {
            fail("SetHandleInformation for WGC direct-bus child failed");
        }

        std::vector<wchar_t> executable(32'768);
        const DWORD length = GetModuleFileNameW(
            nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) {
            fail("GetModuleFileName for WGC direct-bus child failed");
        }
        std::wstring command_line = L"\"" + std::wstring(executable.data(), length)
            + L"\" --bus-consumer "
            + std::to_wstring(reinterpret_cast<std::uintptr_t>(child_input.get()))
            + L" "
            + std::to_wstring(reinterpret_cast<std::uintptr_t>(child_output.get()));
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                nullptr,
                command_line.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process)) {
            fail("CreateProcess for WGC direct-bus child failed");
        }
        process_.reset(process.hProcess);
        thread_.reset(process.hThread);
        child_input.reset();
        child_output.reset();
    }

    void cleanup() noexcept {
        output_.reset();
        if (process_.get() != nullptr
            && WaitForSingleObject(process_.get(), 250) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process_.get(), 0xfcb7u);
            (void)WaitForSingleObject(process_.get(), 2'000);
        }
        input_.reset();
        thread_.reset();
        process_.reset();
    }

    ScopedHandle process_;
    ScopedHandle thread_;
    ScopedHandle input_;
    ScopedHandle output_;
    gpu::SharedFrameBusRegistration registration_{};
    gpu::SharedFrameBusFrameMetadata last_metadata_{};
    bool attached_ = false;
};

gpu::SharedFrameBusConfig direct_bus_config(
    std::uint32_t size,
    std::uint32_t consumers = 1) noexcept {
    gpu::SharedFrameBusConfig config;
    config.width = size;
    config.height = size;
    config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
    config.slot_count = 3;
    config.max_consumers = consumers;
    return config;
}

HRESULT query_monitor_output_description(
    HMONITOR monitor,
    DXGI_OUTPUT_DESC& result) noexcept {
    result = {};
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
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(output_index, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) return hr;
            DXGI_OUTPUT_DESC description{};
            hr = output->GetDesc(&description);
            if (FAILED(hr)) return hr;
            if (description.Monitor == monitor
                && description.AttachedToDesktop) {
                result = description;
                return S_OK;
            }
        }
    }
    return DXGI_ERROR_NOT_FOUND;
}

void set_pattern_phase(HWND window, std::uint32_t phase) {
    DWORD_PTR ignored = 0;
    const LRESULT sent = SendMessageTimeoutW(
        window,
        phase == 0 ? kSetPatternPhase0Message : kSetPatternPhase1Message,
        0,
        0,
        SMTO_ABORTIFHUNG,
        3'000,
        &ignored);
    if (sent == 0) fail("failed to set the WGC direct-bus test pattern phase");
    (void)DwmFlush();
}

void resize_pattern_window(HWND window, bool shrink) {
    DWORD_PTR ignored = 0;
    const LRESULT sent = SendMessageTimeoutW(
        window,
        shrink ? kResizePatternSmallMessage : kResizePatternRestoreMessage,
        0,
        0,
        SMTO_ABORTIFHUNG,
        3'000,
        &ignored);
    if (sent == 0) fail("failed to resize the WGC direct-bus test window");
    (void)DwmFlush();
}

template <class Predicate>
bool wait_until(std::uint32_t timeout_ms, Predicate&& predicate) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        if (predicate()) return true;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return predicate();
}

void test_desktop_duplication_side_data_helpers() {
    gpu::WgcCursorShape shape;
    shape.sequence = 17;
    shape.kind = gpu::WgcCursorShapeKind::masked_color_bgra8;
    shape.width = 2;
    shape.height = 2;
    shape.hotspot_x = 1;
    shape.hotspot_y = 0;
    shape.stride_bytes = 8;
    shape.data = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16};
    const std::uint64_t key =
        gpu::internal::desktop_duplication_cursor_shape_key(shape);
    auto identical = shape;
    identical.sequence = 9'001;
    if (key == 0
        || gpu::internal::desktop_duplication_cursor_shape_key(identical)
            != key) {
        fail("Desktop Duplication cursor content key depends on capture-local sequence");
    }
    auto changed = shape;
    changed.data.back() ^= 0xffu;
    if (gpu::internal::desktop_duplication_cursor_shape_key(changed) == key) {
        fail("Desktop Duplication cursor content key did not distinguish pixels");
    }
    changed = shape;
    changed.kind = gpu::WgcCursorShapeKind::color_bgra8;
    if (gpu::internal::desktop_duplication_cursor_shape_key(changed) == key) {
        fail("Desktop Duplication cursor content key did not distinguish shape semantics");
    }
    if (!gpu::internal::desktop_duplication_win32_shape_fallback_allowed(
            false, false)
        || gpu::internal::desktop_duplication_win32_shape_fallback_allowed(
            true, false)
        || gpu::internal::desktop_duplication_win32_shape_fallback_allowed(
            false, true)) {
        fail("Desktop Duplication Win32 shape fallback can override native provenance");
    }

    gpu::WgcMoveRect move;
    move.source_x = 2;
    move.source_y = 4;
    move.destination = {6, 8, 10, 12};
    if (!gpu::internal::desktop_duplication_planar_move_aligned(move)) {
        fail("aligned Desktop Duplication planar move was rejected");
    }
    const auto require_rejected = [&](gpu::WgcMoveRect candidate) {
        if (gpu::internal::desktop_duplication_planar_move_aligned(candidate)) {
            fail("misaligned Desktop Duplication planar move was accepted");
        }
    };
    auto candidate = move;
    candidate.source_x = 3;
    require_rejected(candidate);
    candidate = move;
    candidate.source_y = 5;
    require_rejected(candidate);
    candidate = move;
    candidate.destination.x = 7;
    require_rejected(candidate);
    candidate = move;
    candidate.destination.y = 9;
    require_rejected(candidate);
    candidate = move;
    candidate.destination.width = 9;
    require_rejected(candidate);
    candidate = move;
    candidate.destination.height = 11;
    require_rejected(candidate);

    gpu::WgcFrameDamage full;
    full.base_sequence = 99;
    full.dirty_count = 7;
    full.move_count = 3;
    gpu::internal::desktop_duplication_full_damage(
        full, 640, 360, true);
    constexpr std::uint32_t expected_full_flags =
        gpu::wgc_damage_valid
        | gpu::wgc_damage_native
        | gpu::wgc_damage_native_move_available
        | gpu::wgc_damage_full_frame
        | gpu::wgc_damage_overflow;
    if (full.base_sequence != 0 || full.flags != expected_full_flags
        || full.dirty_count != 1 || full.move_count != 0
        || full.dirty_rects[0].x != 0 || full.dirty_rects[0].y != 0
        || full.dirty_rects[0].width != 640
        || full.dirty_rects[0].height != 360) {
        fail("Desktop Duplication capacity overflow did not fail closed to full damage");
    }
}

void require_wgc_bus_metadata(
    const gpu::SharedFrameBusFrameMetadata& metadata,
    std::uint32_t roi_x,
    std::uint32_t roi_y,
    std::uint32_t roi_width,
    std::uint32_t roi_height,
    std::uint64_t mailbox_generation,
    const char* label) {
    constexpr std::uint64_t expected_fields =
        gpu::shared_frame_bus_metadata_source_timestamp
        | gpu::shared_frame_bus_metadata_qpc
        | gpu::shared_frame_bus_metadata_source_dimensions
        | gpu::shared_frame_bus_metadata_roi
        | gpu::shared_frame_bus_metadata_mailbox_generation
        | gpu::shared_frame_bus_metadata_color_space;
    if (metadata.structure_size != sizeof(metadata)
        || metadata.metadata_version != gpu::shared_frame_bus_metadata_version
        || metadata.valid_fields != expected_fields
        || metadata.source_timestamp_100ns == 0
        || metadata.timestamp_qpc == 0
        || metadata.qpc_frequency == 0
        || metadata.source_width != static_cast<std::uint32_t>(kPatternWidth)
        || metadata.source_height != static_cast<std::uint32_t>(kPatternHeight)
        || metadata.roi_x != roi_x
        || metadata.roi_y != roi_y
        || metadata.roi_width != roi_width
        || metadata.roi_height != roi_height
        || metadata.mailbox_generation != mailbox_generation
        || metadata.color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        fail(std::string(label) + " WGC shared-bus metadata is inconsistent");
    }
}

void require_direct_bus_stats(
    const gpu::WgcCapture& capture,
    const gpu::SharedFrameBusPublisher& publisher,
    const char* label) {
    const gpu::WgcCaptureStats capture_stats = capture.stats();
    const gpu::SharedFrameBusStats bus_stats = publisher.stats();
    if (capture_stats.published_frames == 0
        || capture_stats.published_frames != bus_stats.published_frames
        || bus_stats.published_frames != bus_stats.direct_publishes
        || bus_stats.copied_publishes != 0
        || bus_stats.no_slot != 0
        || bus_stats.publish_attempts != bus_stats.published_frames) {
        fail(std::string(label) + " direct-bus statistics are inconsistent: WGC="
            + std::to_string(capture_stats.published_frames)
            + ", bus=" + std::to_string(bus_stats.published_frames)
            + ", direct=" + std::to_string(bus_stats.direct_publishes)
            + ", copied=" + std::to_string(bus_stats.copied_publishes)
            + ", no_slot=" + std::to_string(bus_stats.no_slot));
    }
}

void test_wgc_direct_bus_cursor_shape_publication(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    set_pattern_phase(target, 0);
    POINT original_cursor{};
    const BOOL had_cursor_position = GetCursorPos(&original_cursor);
    RECT bounds{};
    if (!GetWindowRect(target, &bounds)
        || !SetCursorPos(bounds.left + 400, bounds.top + 400)) {
        fail("failed to position the direct-bus cursor publication test");
    }
    (void)DwmFlush();

    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
        device, direct_bus_config(320), publisher);
    if (!bus_created) {
        fail(std::string("cursor WGC bus create failed: ")
            + bus_created.what());
    }
    gpu::SharedFrameBusRegistration registration;
    gpu::GpuError bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("cursor WGC bus registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("cursor WGC bus consumer open failed");

    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::centered_region;
    mailbox.width = 320;
    mailbox.height = 320;
    gpu::WgcCaptureOptions options = source_options;
    options.include_cursor = true;
    options.include_cursor_metadata = true;
    options.cursor_shape_refresh_interval_ms = 0;
    options.capture_epoch = 31;
    options.capture_epoch_nonce = 0x8d42'65b1ull;
    gpu::WgcCapture capture;
    const gpu::WgcResult capture_created =
        gpu::WgcCapture::create_for_window_to_bus(
            target, publisher, options, mailbox, capture);

    if (!cursor_capture_control_present()) {
        if (capture_created
            || capture_created.status != gpu::WgcStatus::not_supported
            || capture_created.hresult != E_NOINTERFACE
            || capture.device() != nullptr) {
            fail("independent cursor capture did not fail closed without WGC cursor control");
        }
        gpu::SharedFrameBusWriteLease reservation_probe;
        bus_result = publisher.begin_publish(0, reservation_probe);
        if (!bus_result || !reservation_probe) {
            fail("failed independent cursor creation retained the bus reservation");
        }
        reservation_probe.reset();
        bus_result = consumer.close();
        if (!bus_result) fail("unsupported cursor WGC bus consumer close failed");
        bus_result = publisher.unregister_consumer(registration, 3'000);
        if (!bus_result) fail("unsupported cursor WGC bus unregister failed");
        if (had_cursor_position) {
            (void)SetCursorPos(original_cursor.x, original_cursor.y);
        }
        return;
    }
    if (!capture_created) {
        fail("independent cursor direct-bus capture failed: "
            + capture_created.message);
    }
    const gpu::WgcResult started = capture.start();
    if (!started) {
        fail("independent cursor direct-bus start failed: " + started.message);
    }
    if (!RedrawWindow(
            target,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW)) {
        fail("failed to refresh the direct-bus cursor target");
    }

    gpu::SharedFrameBusFrameLease first;
    bus_result = consumer.acquire_latest(5'000, first);
    if (!bus_result) {
        fail("independent cursor direct-bus produced no first frame");
    }
    const gpu::SharedFrameBusFrameSideData first_side_data = first.side_data();
    const gpu::WgcCursorInfo first_cursor = first_side_data.cursor;
    if (first.info().sequence == 0
        || first_side_data.epoch != options.capture_epoch
        || first_side_data.epoch_nonce != options.capture_epoch_nonce
        || first_cursor.shape_sequence == 0
        || first_cursor.width == 0
        || first_cursor.height == 0
        || (first_cursor.flags & gpu::wgc_cursor_position_valid) == 0
        || (first_cursor.flags & gpu::wgc_cursor_shape_pending) == 0) {
        fail("first independent cursor bus frame was not a pending shape reference");
    }

    gpu::WgcCursorShape published_shape;
    gpu::GpuError shape_result;
    if (!wait_until(2'000, [&] {
            shape_result = consumer.cursor_shape(
                first_cursor.shape_sequence, published_shape);
            return static_cast<bool>(shape_result);
        })) {
        fail(std::string("pending cursor shape was not published after frame commit: ")
            + shape_result.what());
    }
    if (published_shape.sequence != first_cursor.shape_sequence
        || published_shape.width != first_cursor.width
        || published_shape.height != first_cursor.height
        || published_shape.hotspot_x != first_cursor.hotspot_x
        || published_shape.hotspot_y != first_cursor.hotspot_y
        || published_shape.kind == gpu::WgcCursorShapeKind::none
        || published_shape.data.empty()) {
        fail("published direct-bus cursor shape does not match its pending frame");
    }
    const std::uint64_t first_sequence = first.info().sequence;
    bus_result = consumer.release(first);
    if (!bus_result) fail("first cursor WGC bus release failed");

    set_pattern_phase(target, 1);
    gpu::SharedFrameBusFrameLease settled;
    bus_result = consumer.acquire_latest(5'000, settled);
    if (!bus_result) {
        fail("independent cursor direct-bus produced no settled frame");
    }
    const gpu::WgcCursorInfo settled_cursor = settled.side_data().cursor;
    if (settled.info().sequence <= first_sequence
        || settled_cursor.shape_sequence != first_cursor.shape_sequence
        || (settled_cursor.flags & gpu::wgc_cursor_shape_pending) != 0) {
        fail("cursor shape publication was not acknowledged by the next bus frame");
    }
    gpu::WgcCursorShape settled_shape;
    shape_result = consumer.cursor_shape(
        settled_cursor.shape_sequence, settled_shape);
    if (!shape_result
        || settled_shape.sequence != published_shape.sequence
        || settled_shape.hotspot_x != published_shape.hotspot_x
        || settled_shape.hotspot_y != published_shape.hotspot_y
        || settled_shape.data != published_shape.data) {
        fail("settled cursor bus frame could not resolve the published shape");
    }
    bus_result = consumer.release(settled);
    if (!bus_result) fail("settled cursor WGC bus release failed");

    capture.stop();
    const gpu::WgcCaptureStats stats = capture.stats();
    if (stats.published_frames < 2
        || stats.cursor_metadata_frames != stats.published_frames
        || stats.cursor_shape_updates == 0
        || stats.ingress_copy_submissions != stats.published_frames
        || stats.ingress_transform_submissions != 0) {
        fail("independent cursor publication changed the direct-bus ingress contract");
    }
    bus_result = consumer.close();
    if (!bus_result) fail("cursor WGC bus consumer close failed");
    bus_result = publisher.unregister_consumer(registration, 3'000);
    if (!bus_result) fail("cursor WGC bus unregister failed");
    if (had_cursor_position) {
        (void)SetCursorPos(original_cursor.x, original_cursor.y);
    }
}

void test_wgc_direct_bus_async_move_inference(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    set_pattern_phase(target, 0);
    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
        device, direct_bus_config(320), publisher);
    if (!bus_created) {
        fail(std::string("move-inference WGC bus create failed: ")
            + bus_created.what());
    }

    gpu::SharedFrameBusRegistration registration;
    gpu::GpuError bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("move-inference WGC bus registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("move-inference WGC bus consumer open failed");

    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::centered_region;
    mailbox.width = 320;
    mailbox.height = 320;
    gpu::WgcCaptureOptions options = source_options;
    options.damage_mode = gpu::WgcDamageMode::native_with_inferred_moves;
    options.capture_epoch = 29;
    options.capture_epoch_nonce = 0x3f71'a9c5ull;
    gpu::WgcCapture capture;
    const gpu::WgcResult capture_created =
        gpu::WgcCapture::create_for_window_to_bus(
            target, publisher, options, mailbox, capture);
    if (!capture_created
        && capture_created.status == gpu::WgcStatus::not_supported) {
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        return;
    }
    if (!capture_created) {
        fail("move-inference WGC capture create failed: "
            + capture_created.message);
    }
    const gpu::WgcResult started = capture.start();
    if (!started) {
        fail("move-inference WGC capture start failed: " + started.message);
    }

    gpu::SharedFrameBusFrameLease baseline;
    bus_result = consumer.acquire_latest(5'000, baseline);
    if (!bus_result) {
        fail("move-inference WGC baseline frame timed out: "
            + capture.last_error().message);
    }
    const gpu::SharedFrameBusFrameSideData baseline_side_data =
        baseline.side_data();
    if (baseline_side_data.epoch != options.capture_epoch
        || baseline_side_data.epoch_nonce != options.capture_epoch_nonce
        || (baseline_side_data.damage.flags & gpu::wgc_damage_valid) == 0
        || (baseline_side_data.damage.flags & gpu::wgc_damage_move_pending) != 0) {
        fail("move-inference WGC baseline side data is inconsistent");
    }
    bus_result = consumer.release(baseline);
    if (!bus_result) fail("move-inference WGC baseline release failed");

    set_pattern_phase(target, 1);
    gpu::SharedFrameBusFrameSideData pending_side_data;
    std::uint64_t pending_sequence = 0;
    std::uint32_t phase = 1;
    for (std::uint32_t attempt = 0; attempt < 4 && pending_sequence == 0;
         ++attempt) {
        gpu::SharedFrameBusFrameLease frame;
        bus_result = consumer.acquire_latest(5'000, frame);
        if (!bus_result) {
            fail("move-inference WGC animation frame timed out: "
                + capture.last_error().message);
        }
        const auto side_data = frame.side_data();
        if ((side_data.damage.flags & gpu::wgc_damage_move_pending) != 0) {
            pending_sequence = frame.info().sequence;
            pending_side_data = side_data;
        }
        bus_result = consumer.release(frame);
        if (!bus_result) fail("move-inference WGC animation release failed");
        if (pending_sequence == 0) {
            phase ^= 1u;
            set_pattern_phase(target, phase);
        }
    }
    if (pending_sequence == 0
        || pending_side_data.epoch != options.capture_epoch
        || pending_side_data.epoch_nonce != options.capture_epoch_nonce
        || pending_side_data.damage.base_sequence == 0
        || pending_side_data.damage.base_sequence >= pending_sequence) {
        fail("real WGC animation produced no coherent move-pending frame");
    }

    gpu::SharedFrameBusMoveResult move_result;
    const bool resolved = wait_until(5'000, [&] {
        const gpu::GpuError queried = consumer.try_get_move_result(
            pending_side_data.epoch,
            pending_side_data.epoch_nonce,
            pending_sequence,
            move_result);
        if (queried) return true;
        if (queried.status != gpu::GpuStatus::timeout) {
            fail(std::string("move-inference WGC result query failed: ")
                + queried.what());
        }
        return false;
    });
    if (!resolved) {
        fail("move-inference WGC result did not arrive for its original frame key");
    }
    if (move_result.epoch != pending_side_data.epoch
        || move_result.epoch_nonce != pending_side_data.epoch_nonce
        || move_result.sequence != pending_sequence
        || move_result.base_sequence != pending_side_data.damage.base_sequence
        || (move_result.flags & gpu::shared_frame_bus_move_result_valid) == 0
        || (move_result.flags & gpu::shared_frame_bus_move_result_inferred) == 0
        || (move_result.flags & gpu::shared_frame_bus_move_result_fail_closed) != 0
        || move_result.move_count != 1) {
        fail("move-inference WGC result did not preserve its frame identity");
    }
    const gpu::WgcMoveRect& move = move_result.move_rects[0];
    if (move.source_x != 32 || move.source_y != 32
        || move.destination.x != 96 || move.destination.y != 96
        || move.destination.width != 32 || move.destination.height != 32) {
        fail("real WGC animation returned the wrong inferred move rectangle");
    }

    capture.stop();
    const gpu::WgcCaptureStats capture_stats = capture.stats();
    const gpu::SharedFrameBusStats bus_stats = publisher.stats();
    if (capture_stats.published_frames == 0
        || capture_stats.ingress_copy_submissions
            != capture_stats.published_frames
        || capture_stats.ingress_transform_submissions != 0
        || capture_stats.move_inference_submissions
            != capture_stats.published_frames
        || capture_stats.move_results_published == 0
        || capture_stats.move_results_published
            != bus_stats.move_results_published
        || bus_stats.direct_publishes != capture_stats.published_frames
        || bus_stats.copied_publishes != 0) {
        fail("move inference changed the WGC direct-bus zero-redundancy contract");
    }
    bus_result = consumer.close();
    if (!bus_result) fail("move-inference WGC bus consumer close failed");
    bus_result = publisher.unregister_consumer(registration, 3'000);
    if (!bus_result) fail("move-inference WGC bus unregister failed");
}

void test_wgc_planar_bus_external_encoder(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    gpu::SharedFrameBusConfig bus_config = direct_bus_config(160);
    bus_config.format = DXGI_FORMAT_NV12;
    bus_config.color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    bus_config.bind_flags = D3D11_BIND_RENDER_TARGET
        | D3D11_BIND_VIDEO_ENCODER;

    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
        device, bus_config, publisher);
    if (!bus_created && bus_created.status == gpu::GpuStatus::unsupported) return;
    if (!bus_created) {
        fail(std::string("planar WGC bus create failed: ") + bus_created.what());
    }

    gpu::SharedFrameBusRegistration registration;
    auto bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("planar WGC bus consumer registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("planar WGC bus consumer open failed");

    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::centered_region;
    mailbox.width = 320;
    mailbox.height = 320;
    gpu::WgcCaptureOptions options = source_options;
    options.planar_transform_backend =
        gpu::GpuTransformBackend::deterministic_planar;
    options.capture_epoch = 19;
    options.capture_epoch_nonce = 0x9d3a'51c7ull;
    gpu::WgcCapture capture;
    const gpu::WgcResult capture_created =
        gpu::WgcCapture::create_for_window_to_bus(
            target, publisher, options, mailbox, capture);
    if (!capture_created
        && (capture_created.status == gpu::WgcStatus::not_supported
            || capture_created.hresult == E_NOINTERFACE)) {
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        return;
    }
    if (!capture_created) {
        fail("planar WGC capture create failed: " + capture_created.message);
    }
    const gpu::WgcResult started = capture.start();
    if (!started) fail("planar WGC capture start failed: " + started.message);

    set_pattern_phase(target, 0);
    gpu::SharedFrameBusFrameLease probe;
    const gpu::GpuError acquired = consumer.acquire_latest(5'000, probe);
    if (!acquired) {
        const auto capture_error = capture.last_error();
        fail("planar WGC bus produced no frame: " + capture_error.message);
    }
    const auto metadata = probe.metadata();
    const auto side_data = probe.side_data();
    if (probe.info().format != DXGI_FORMAT_NV12
        || probe.info().width != bus_config.width
        || probe.info().height != bus_config.height
        || metadata.color_space
            != DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
        || metadata.roi_width != mailbox.width
        || metadata.roi_height != mailbox.height
        || side_data.epoch != options.capture_epoch
        || side_data.epoch_nonce != options.capture_epoch_nonce
        || (side_data.damage.flags & gpu::wgc_damage_valid) == 0) {
        fail("planar WGC bus frame contract is inconsistent");
    }
    const auto& damage = side_data.damage;
    if ((damage.flags & gpu::wgc_damage_full_frame) != 0) {
        if (damage.dirty_count != 1
            || damage.dirty_rects[0].x != 0
            || damage.dirty_rects[0].y != 0
            || damage.dirty_rects[0].width != bus_config.width
            || damage.dirty_rects[0].height != bus_config.height) {
            fail("scaled planar full damage is malformed");
        }
    } else {
        if ((damage.flags & gpu::wgc_damage_native) == 0) {
            fail("scaled planar partial damage lost native provenance");
        }
        for (std::uint32_t index = 0; index < damage.dirty_count; ++index) {
            const auto& dirty = damage.dirty_rects[index];
            if (dirty.x < 0 || dirty.y < 0
                || dirty.width == 0 || dirty.height == 0
                || ((static_cast<std::uint32_t>(dirty.x)
                        | static_cast<std::uint32_t>(dirty.y)
                        | dirty.width | dirty.height) & 1u) != 0
                || static_cast<std::uint64_t>(dirty.x) + dirty.width
                    > bus_config.width
                || static_cast<std::uint64_t>(dirty.y) + dirty.height
                    > bus_config.height) {
                fail("scaled planar partial damage is out of bounds or chroma-unaligned");
            }
        }
    }
    const PlanarReadback baseline_pixels = read_planar_texture(
        device, capture.context(), probe.texture());
    const std::uint64_t baseline_sequence = probe.info().sequence;
    bus_result = consumer.release(probe);
    if (!bus_result) fail("planar WGC probe release failed");

    gpu::SharedFrameBusFrameLease changed;
    gpu::SharedFrameBusFrameSideData changed_side_data{};
    PlanarReadback changed_pixels;
    bool observed_changed_pixels = false;
    for (std::uint32_t attempt = 0;
         attempt < 6 && !observed_changed_pixels;
         ++attempt) {
        set_pattern_phase(target, (attempt & 1u) == 0 ? 1 : 0);
        bus_result = consumer.acquire_latest(5'000, changed);
        if (!bus_result || changed.info().sequence <= baseline_sequence) {
            fail("deterministic planar WGC produced no newer frame");
        }
        changed_pixels = read_planar_texture(
            device, capture.context(), changed.texture());
        observed_changed_pixels = changed_pixels.bytes
            != baseline_pixels.bytes;
        if (observed_changed_pixels) {
            changed_side_data = changed.side_data();
            break;
        }
        bus_result = consumer.release(changed);
        if (!bus_result) {
            fail("unchanged deterministic planar frame release failed");
        }
    }
    if (!observed_changed_pixels) {
        fail("deterministic planar animation changed no output bytes");
    }
    const auto& changed_damage = changed_side_data.damage;
    if ((changed_damage.flags & gpu::wgc_damage_valid) == 0
        || (changed_damage.flags & gpu::wgc_damage_native) == 0
        || (changed_damage.flags & gpu::wgc_damage_full_frame) != 0
        || changed_damage.dirty_count == 0) {
        fail("deterministic scaled planar WGC did not publish native partial damage");
    }
    if (changed_pixels.format != baseline_pixels.format
        || changed_pixels.width != baseline_pixels.width
        || changed_pixels.height != baseline_pixels.height
        || changed_pixels.row_bytes != baseline_pixels.row_bytes
        || changed_pixels.bytes.size() != baseline_pixels.bytes.size()) {
        fail("deterministic planar WGC readback shape changed");
    }
    const auto covered = [&](std::uint32_t x, std::uint32_t y) {
        for (std::uint32_t index = 0;
             index < changed_damage.dirty_count;
             ++index) {
            const auto& rect = changed_damage.dirty_rects[index];
            if (rect.x >= 0 && rect.y >= 0
                && x >= static_cast<std::uint32_t>(rect.x)
                && y >= static_cast<std::uint32_t>(rect.y)
                && x < static_cast<std::uint64_t>(rect.x) + rect.width
                && y < static_cast<std::uint64_t>(rect.y) + rect.height) {
                return true;
            }
        }
        return false;
    };
    std::size_t changed_byte_count = 0;
    const std::uint32_t total_rows = bus_config.height
        + bus_config.height / 2u;
    for (std::uint32_t row = 0; row < total_rows; ++row) {
        for (std::uint32_t column = 0;
             column < changed_pixels.row_bytes;
             ++column) {
            const std::size_t offset =
                static_cast<std::size_t>(row) * changed_pixels.row_bytes
                + column;
            if (baseline_pixels.bytes[offset]
                == changed_pixels.bytes[offset]) {
                continue;
            }
            ++changed_byte_count;
            const std::uint32_t frame_x = row < bus_config.height
                ? column
                : (column / 2u) * 2u;
            const std::uint32_t frame_y = row < bus_config.height
                ? row
                : (row - bus_config.height) * 2u;
            if (!covered(frame_x, frame_y)) {
                fail("deterministic planar damage omitted a changed final byte");
            }
        }
    }
    if (changed_byte_count == 0) {
        fail("deterministic planar byte comparison is inconsistent");
    }
    bus_result = consumer.release(changed);
    if (!bus_result) fail("deterministic planar changed-frame release failed");

    gpu::AsyncGpuPipelineConfig pipeline_config;
    pipeline_config.transform.input_width = bus_config.width;
    pipeline_config.transform.input_height = bus_config.height;
    pipeline_config.transform.output_width = bus_config.width;
    pipeline_config.transform.output_height = bus_config.height;
    pipeline_config.transform.input_format = DXGI_FORMAT_NV12;
    pipeline_config.transform.input_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    pipeline_config.transform.output_format = gpu::GpuPixelFormat::nv12;
    pipeline_config.transform.output_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    pipeline_config.encoder.codec = gpu::VideoCodec::h264;
    pipeline_config.encoder.width = bus_config.width;
    pipeline_config.encoder.height = bus_config.height;
    pipeline_config.encoder.input_format = DXGI_FORMAT_NV12;
    pipeline_config.encoder.input_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    pipeline_config.encoder.require_video_encoder_input_bind = true;
    pipeline_config.encoder.frame_rate_numerator = 60;
    pipeline_config.encoder.frame_rate_denominator = 1;
    pipeline_config.encoder.bitrate = 2'000'000;
    pipeline_config.encoder.input_pool_size = 3;
    pipeline_config.queue_depth = 3;

    PacketCollector packets;
    auto mismatched_color_config = pipeline_config;
    mismatched_color_config.transform.output_color_space =
        DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
    gpu::AsyncGpuPipeline rejected_pipeline;
    const auto rejected_bypass =
        gpu::AsyncGpuPipeline::create_from_shared_bus(
            device,
            std::move(consumer),
            mismatched_color_config,
            &collect_packet,
            &packets,
            rejected_pipeline);
    if (rejected_bypass
        || rejected_bypass.status
            != gpu::AsyncGpuPipelineStatus::invalid_argument
        || !consumer.initialized()) {
        fail("planar external bypass ignored a requested color conversion");
    }

    gpu::AsyncGpuPipeline pipeline;
    const auto pipeline_created = gpu::AsyncGpuPipeline::create_from_shared_bus(
        device,
        std::move(consumer),
        pipeline_config,
        &collect_packet,
        &packets,
        pipeline);
    if (!pipeline_created) {
        fail("planar external encoder pipeline create failed: "
            + pipeline_created.message);
    }
    set_pattern_phase(target, 1);
    if (!wait_until(5'000, [&] {
            return pipeline.stats().processed_frames != 0;
        })) {
        fail("planar external encoder pipeline processed no frame");
    }
    capture.stop();
    const auto drained = pipeline.drain(15'000);
    if (!drained) {
        fail("planar external encoder pipeline drain failed: " + drained.message);
    }
    const auto pipeline_stats = pipeline.stats();
    const auto capture_stats = capture.stats();
    const auto bus_stats = publisher.stats();
    if (pipeline_stats.processed_frames == 0
        || pipeline_stats.encoder_copied_submissions != 0
        || pipeline_stats.input_copy_submissions != 0
        || pipeline_stats.transform_submissions != 0
        || pipeline_stats.encoder_direct_submissions
            != pipeline_stats.processed_frames
        || pipeline_stats.encoder_external_submissions
            != pipeline_stats.processed_frames
        || pipeline_stats.encoder_external_identity_verified_submissions
            != pipeline_stats.encoder_external_submissions
        || pipeline_stats.encoder_external_video_encoder_bound_submissions
            != pipeline_stats.encoder_external_submissions
        || capture_stats.ingress_copy_submissions != 0
        || capture_stats.ingress_transform_submissions == 0
        || capture_stats.ingress_transform_submissions
            != capture_stats.published_frames
        || bus_stats.copied_publishes != 0
        || bus_stats.direct_publishes != bus_stats.published_frames
        || packets.packet_count == 0) {
        fail("planar WGC external encoder zero-copy statistics are inconsistent");
    }
    pipeline.close();
    bus_result = publisher.unregister_consumer(registration, 5'000);
    if (!bus_result) fail("planar external encoder left a bus slot pinned");
}

void test_wgc_hdr_p010_bus(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    gpu::SharedFrameBusConfig bus_config = direct_bus_config(320);
    bus_config.format = DXGI_FORMAT_P010;
    bus_config.color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    bus_config.bind_flags = D3D11_BIND_RENDER_TARGET
        | D3D11_BIND_VIDEO_ENCODER;
    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError created = gpu::SharedFrameBusPublisher::create(
        device, bus_config, publisher);
    if (!created && created.status == gpu::GpuStatus::unsupported) return;
    if (!created) fail(std::string("P010 WGC bus create failed: ") + created.what());

    gpu::SharedFrameBusRegistration registration;
    auto bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("P010 WGC bus registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("P010 WGC bus consumer open failed");

    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::centered_region;
    mailbox.width = 320;
    mailbox.height = 320;
    gpu::WgcCaptureOptions options = source_options;
    options.pixel_format = gpu::WgcPixelFormat::rgba16_float;
    options.damage_mode = gpu::WgcDamageMode::disabled;
    options.capture_epoch = 23;
    options.capture_epoch_nonce = 0x77b1'3e09ull;
    gpu::WgcCapture capture;
    const auto capture_created = gpu::WgcCapture::create_for_window_to_bus(
        target, publisher, options, mailbox, capture);
    if (!capture_created
        && (capture_created.status == gpu::WgcStatus::not_supported
            || capture_created.hresult == E_NOINTERFACE
            || capture_created.hresult == DXGI_ERROR_UNSUPPORTED)) {
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        return;
    }
    if (!capture_created) {
        fail("P010 WGC capture create failed: " + capture_created.message);
    }
    const auto started = capture.start();
    if (!started) fail("P010 WGC capture start failed: " + started.message);
    set_pattern_phase(target, 0);

    gpu::SharedFrameBusFrameLease frame;
    const gpu::GpuError acquired = consumer.acquire_latest(5'000, frame);
    if (!acquired) {
        const auto error = capture.last_error();
        capture.stop();
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        if (error.hresult == E_NOINTERFACE
            || error.hresult == DXGI_ERROR_UNSUPPORTED) {
            return;
        }
        fail("P010 WGC bus produced no frame: " + error.message);
    }
    const auto metadata = frame.metadata();
    const auto side_data = frame.side_data();
    if (frame.info().format != DXGI_FORMAT_P010
        || metadata.color_space
            != DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
        || side_data.epoch != options.capture_epoch
        || side_data.epoch_nonce != options.capture_epoch_nonce) {
        fail("P010 WGC bus did not preserve its HDR epoch/color contract");
    }
    bus_result = consumer.release(frame);
    if (!bus_result) fail("P010 WGC bus release failed");

    gpu::GpuEncoderConfig encoder_config;
    encoder_config.codec = gpu::VideoCodec::hevc;
    encoder_config.width = 320;
    encoder_config.height = 320;
    encoder_config.frame_rate_numerator = 60;
    encoder_config.frame_rate_denominator = 1;
    encoder_config.bitrate = 2'000'000;
    encoder_config.gop_size = 60;
    encoder_config.input_format = DXGI_FORMAT_P010;
    encoder_config.input_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    encoder_config.require_video_encoder_input_bind = true;
    encoder_config.profile = eAVEncH265VProfile_Main_420_10;
    encoder_config.input_pool_size = 3;

    gpu::GpuEncoderSupport encoder_support;
    const auto probed = gpu::GpuEncoder::probe(
        device, encoder_config, encoder_support);
    if (!probed && probed.status == gpu::GpuEncoderStatus::invalid_argument) {
        fail("valid P010/PQ HEVC Main10 probe contract was rejected");
    }
    bool encoder_available = probed
        && encoder_support.supported
        && encoder_support.d3d11_aware
        && encoder_support.external_planar_input;
    if (encoder_available) {
        PacketCollector initialization_packets;
        gpu::GpuEncoder initialization_probe;
        const auto initialized = initialization_probe.initialize(
            device,
            encoder_config,
            &collect_packet,
            &initialization_packets);
        if (!initialized
            && initialized.status != gpu::GpuEncoderStatus::not_supported
            && initialized.status
                != gpu::GpuEncoderStatus::unsupported_input_format) {
            fail("advertised P010/PQ HEVC Main10 encoder failed to initialize: "
                + initialized.message);
        }
        encoder_available = static_cast<bool>(initialized);
        initialization_probe.close();
    }

    if (!encoder_available) {
        capture.stop();
        const auto stats = capture.stats();
        if (stats.published_frames == 0
            || stats.ingress_copy_submissions != 0
            || stats.ingress_transform_submissions
                != stats.published_frames) {
            fail("P010 WGC bus performed a redundant ingress copy");
        }
        bus_result = consumer.close();
        if (!bus_result) fail("P010 WGC bus consumer close failed");
        bus_result = publisher.unregister_consumer(registration, 3'000);
        if (!bus_result) fail("P010 WGC bus unregister failed");
        return;
    }

    gpu::AsyncGpuPipelineConfig pipeline_config;
    pipeline_config.transform.input_width = 320;
    pipeline_config.transform.input_height = 320;
    pipeline_config.transform.output_width = 320;
    pipeline_config.transform.output_height = 320;
    pipeline_config.transform.input_format = DXGI_FORMAT_P010;
    pipeline_config.transform.input_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    pipeline_config.transform.output_format = gpu::GpuPixelFormat::p010;
    pipeline_config.transform.output_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    pipeline_config.encoder = encoder_config;
    pipeline_config.queue_depth = 3;

    PacketCollector packets;
    gpu::AsyncGpuPipeline pipeline;
    const auto pipeline_created = gpu::AsyncGpuPipeline::create_from_shared_bus(
        device,
        std::move(consumer),
        pipeline_config,
        &collect_packet,
        &packets,
        pipeline);
    if (!pipeline_created) {
        fail("P010/PQ external encoder pipeline create failed: "
            + pipeline_created.message);
    }
    set_pattern_phase(target, 1);
    if (!wait_until(5'000, [&] {
            return pipeline.stats().processed_frames != 0;
        })) {
        fail("P010/PQ external encoder pipeline processed no frame");
    }
    capture.stop();
    const auto drained = pipeline.drain(15'000);
    if (!drained) {
        fail("P010/PQ external encoder pipeline drain failed: "
            + drained.message);
    }
    const auto pipeline_stats = pipeline.stats();
    const auto capture_stats = capture.stats();
    const auto bus_stats = publisher.stats();
    if (capture_stats.published_frames == 0
        || capture_stats.ingress_copy_submissions != 0
        || capture_stats.ingress_transform_submissions
            != capture_stats.published_frames
        || pipeline_stats.processed_frames == 0
        || pipeline_stats.input_copy_submissions != 0
        || pipeline_stats.transform_submissions != 0
        || pipeline_stats.encoder_external_submissions
            != pipeline_stats.processed_frames
        || pipeline_stats.encoder_external_identity_verified_submissions
            != pipeline_stats.encoder_external_submissions
        || pipeline_stats.encoder_external_video_encoder_bound_submissions
            != pipeline_stats.encoder_external_submissions
        || pipeline_stats.encoder_direct_submissions
            != pipeline_stats.processed_frames
        || pipeline_stats.encoder_copied_submissions != 0
        || bus_stats.copied_publishes != 0
        || bus_stats.direct_publishes != bus_stats.published_frames
        || packets.packet_count == 0) {
        fail("P010/PQ HEVC Main10 zero-copy statistics are inconsistent");
    }
    pipeline.close();
    bus_result = publisher.unregister_consumer(registration, 5'000);
    if (!bus_result) fail("P010/PQ external encoder left a bus slot pinned");
}

void test_gpu_encoder_invalid_color_contract(ID3D11Device* device) {
    gpu::GpuEncoderConfig config;
    config.codec = gpu::VideoCodec::h264;
    config.width = 320;
    config.height = 320;
    config.input_format = DXGI_FORMAT_NV12;
    config.input_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    gpu::GpuEncoderSupport support;
    const auto rejected = gpu::GpuEncoder::probe(device, config, support);
    if (rejected
        || rejected.status != gpu::GpuEncoderStatus::invalid_argument
        || rejected.hresult != E_INVALIDARG
        || support.supported
        || support.matching_transform_count != 0) {
        fail("encoder probe accepted an RGB color contract for NV12 input");
    }

    config.codec = gpu::VideoCodec::hevc;
    config.input_format = DXGI_FORMAT_P010;
    config.input_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    config.hdr10.enabled = true;
    config.hdr10.red_primary_x = 50'001;
    const auto invalid_mastering = gpu::GpuEncoder::probe(
        device, config, support);
    if (invalid_mastering
        || invalid_mastering.status != gpu::GpuEncoderStatus::invalid_argument) {
        fail("encoder accepted out-of-range HDR10 mastering primaries");
    }
    config.hdr10 = {};
    config.require_bitstream_hdr10_metadata = true;
    const auto missing_mastering = gpu::GpuEncoder::probe(
        device, config, support);
    if (missing_mastering
        || missing_mastering.status != gpu::GpuEncoderStatus::invalid_argument) {
        fail("encoder accepted HDR10 bitstream validation without metadata");
    }

    config = {};
    config.codec = gpu::VideoCodec::h264;
    config.width = 320;
    config.height = 320;
    config.input_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.input_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    config.require_video_encoder_input_bind = true;
    const auto invalid_bind_contract = gpu::GpuEncoder::probe(
        device, config, support);
    if (invalid_bind_contract
        || invalid_bind_contract.status
            != gpu::GpuEncoderStatus::invalid_argument) {
        fail("encoder accepted a video-encoder bind gate for non-planar input");
    }
}

void test_wgc_direct_bus_reconfigure(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& options) {
    set_pattern_phase(target, 0);
    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
        device, direct_bus_config(320), publisher);
    if (!bus_created) {
        fail(std::string("320 WGC direct bus create failed: ")
            + bus_created.what());
    }
    BusConsumerChild child;
    const gpu::SharedFrameBusRegistration registration = child.attach(publisher);

    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::centered_region;
    mailbox.width = 320;
    mailbox.height = 320;
    gpu::WgcCapture capture;
    const gpu::WgcResult created = gpu::WgcCapture::create_for_window_to_bus(
        target, publisher, options, mailbox, capture);
    if (!created) fail("320 WGC direct-bus create failed: " + created.message);
    auto manual_source = create_coordinate_texture(device, 320, 320);
    const gpu::GpuError reserved_copy = publisher.publish(
        manual_source.Get(), 0);
    gpu::SharedFrameBusWriteLease reserved_direct;
    const gpu::GpuError reserved_begin = publisher.begin_publish(
        0, reserved_direct);
    if (reserved_copy
        || reserved_copy.status != gpu::GpuStatus::invalid_argument
        || reserved_begin
        || reserved_begin.status != gpu::GpuStatus::invalid_argument
        || reserved_direct) {
        fail("ordinary SharedFrameBus publication bypassed the WGC producer reservation");
    }
    const gpu::WgcMailboxState centered_state = capture.mailbox_state();
    if (!centered_state.region_available || centered_state.generation == 0) {
        fail("320 WGC direct-bus mailbox was not initially available");
    }
    const gpu::WgcResult started = capture.start();
    if (!started) fail("320 WGC direct-bus start failed: " + started.message);

    gpu::WgcFrameLease private_frame;
    gpu::WgcResult private_acquire;
    std::atomic<bool> private_acquire_done{false};
    std::thread private_waiter([&] {
        private_acquire = capture.acquire_latest(INFINITE, private_frame);
        private_acquire_done.store(true, std::memory_order_release);
    });
    const ULONGLONG acquire_deadline = GetTickCount64() + 250;
    while (!private_acquire_done.load(std::memory_order_acquire)
        && GetTickCount64() < acquire_deadline) {
        Sleep(1);
    }
    const bool acquire_returned_immediately = private_acquire_done.load(
        std::memory_order_acquire);
    if (!acquire_returned_immediately) capture.stop();
    private_waiter.join();
    if (!acquire_returned_immediately
        || private_acquire
        || private_acquire.status != gpu::WgcStatus::invalid_state
        || private_frame) {
        fail("bus-only WGC exposed a private capture-ring frame");
    }

    const std::uint64_t centered_sequence = child.acquire_validate(
        240, 239, 320, 320, 0);
    const auto centered_metadata = child.metadata();
    require_wgc_bus_metadata(
        centered_metadata,
        240,
        239,
        320,
        320,
        centered_state.generation,
        "centered 320");
    child.release();

    gpu::WgcMailboxConfig absolute = mailbox;
    absolute.mode = gpu::WgcMailboxMode::absolute_region;
    absolute.x = 17;
    absolute.y = 23;
    const gpu::WgcResult reconfigured = capture.set_mailbox_config(absolute);
    if (!reconfigured) {
        fail("same-size WGC direct-bus reconfiguration failed: "
            + reconfigured.message);
    }
    const gpu::WgcMailboxState absolute_state = capture.mailbox_state();
    if (!absolute_state.region_available
        || absolute_state.generation != centered_state.generation + 1
        || absolute_state.config.mode != gpu::WgcMailboxMode::absolute_region
        || absolute_state.config.x != 17
        || absolute_state.config.y != 23) {
        fail("same-size WGC direct-bus reconfiguration returned stale state");
    }

    gpu::WgcMailboxConfig wrong_size = absolute;
    wrong_size.width = 640;
    wrong_size.height = 640;
    const gpu::WgcResult rejected = capture.set_mailbox_config(wrong_size);
    const gpu::WgcMailboxState after_rejection = capture.mailbox_state();
    if (rejected
        || rejected.status != gpu::WgcStatus::invalid_argument
        || after_rejection.generation != absolute_state.generation
        || after_rejection.config.width != 320
        || after_rejection.config.height != 320
        || after_rejection.config.x != absolute.x
        || after_rejection.config.y != absolute.y) {
        fail("size-changing WGC direct-bus reconfiguration changed its epoch");
    }

    set_pattern_phase(target, 1);
    const std::uint64_t absolute_sequence = child.acquire_validate(
        absolute.x, absolute.y, 320, 320, 1);
    const auto absolute_metadata = child.metadata();
    require_wgc_bus_metadata(
        absolute_metadata,
        absolute.x,
        absolute.y,
        320,
        320,
        absolute_state.generation,
        "absolute 320");
    if (absolute_sequence <= centered_sequence) {
        fail("same-size WGC direct-bus reconfiguration did not advance sequence");
    }
    if (absolute_metadata.timestamp_qpc <= centered_metadata.timestamp_qpc
        || absolute_metadata.source_timestamp_100ns
            < centered_metadata.source_timestamp_100ns) {
        fail("reconfigured WGC direct-bus timestamps did not advance monotonically");
    }
    child.release();
    require_direct_bus_stats(capture, publisher, "320 ROI");
    capture.stop();

    const gpu::GpuError copied_after_stop = publisher.publish(
        manual_source.Get(), 0);
    gpu::SharedFrameBusWriteLease direct_after_stop;
    const gpu::GpuError begun_after_stop = publisher.begin_publish(
        0, direct_after_stop);
    if (!copied_after_stop || !begun_after_stop || !direct_after_stop) {
        fail("SharedFrameBus producer reservation was not released by WGC stop");
    }
    capture.context()->CopyResource(
        direct_after_stop.texture(), manual_source.Get());
    const gpu::GpuError committed_after_stop = publisher.commit(
        std::move(direct_after_stop));
    if (!committed_after_stop) {
        fail(std::string("manual direct publish after WGC stop failed: ")
            + committed_after_stop.what());
    }
    child.close_and_unregister(publisher);
    (void)registration;
}

void test_wgc_direct_bus_absolute_640(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& options) {
    set_pattern_phase(target, 0);
    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
        device, direct_bus_config(640), publisher);
    if (!bus_created) {
        fail(std::string("640 WGC direct bus create failed: ")
            + bus_created.what());
    }
    BusConsumerChild child;
    child.attach(publisher);
    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::absolute_region;
    mailbox.x = 17;
    mailbox.y = 23;
    mailbox.width = 640;
    mailbox.height = 640;
    gpu::WgcCapture capture;
    const gpu::WgcResult created = gpu::WgcCapture::create_for_window_to_bus(
        target, publisher, options, mailbox, capture);
    if (!created) fail("640 WGC direct-bus create failed: " + created.message);
    const gpu::WgcResult started = capture.start();
    if (!started) fail("640 WGC direct-bus start failed: " + started.message);
    const gpu::WgcMailboxState initial_state = capture.mailbox_state();
    const std::uint64_t initial_sequence = child.acquire_validate(
        mailbox.x, mailbox.y, 640, 640, 0);
    const auto initial_metadata = child.metadata();
    require_wgc_bus_metadata(
        initial_metadata,
        mailbox.x,
        mailbox.y,
        640,
        640,
        initial_state.generation,
        "initial absolute 640");
    child.release();

    resize_pattern_window(target, true);
    gpu::WgcMailboxState unavailable_state;
    gpu::WgcResult unavailable_error;
    const bool became_unavailable = wait_until(3'000, [&] {
        unavailable_state = capture.mailbox_state();
        unavailable_error = capture.last_error();
        return !unavailable_state.region_available
            && unavailable_state.source_width == 639
            && unavailable_state.source_height == 639
            && unavailable_error.status == gpu::WgcStatus::region_unavailable;
    });
    if (!became_unavailable
        || unavailable_state.generation != initial_state.generation) {
        fail("640 WGC direct bus did not enter region_unavailable on source shrink");
    }
    const std::uint64_t unavailable_sequence = publisher.sequence();
    const gpu::SharedFrameBusStats unavailable_bus_stats = publisher.stats();
    const std::uint64_t unavailable_received = capture.stats().received_frames;
    set_pattern_phase(target, 1);
    if (!wait_until(3'000, [&] {
            return capture.stats().received_frames > unavailable_received;
        })) {
        fail("shrunk WGC direct-bus source emitted no test presentation");
    }
    if (publisher.sequence() != unavailable_sequence
        || publisher.stats().published_frames
            != unavailable_bus_stats.published_frames
        || capture.last_error().status != gpu::WgcStatus::region_unavailable) {
        fail("unavailable WGC ROI advanced the shared bus");
    }

    resize_pattern_window(target, false);
    gpu::WgcMailboxState restored_state;
    if (!wait_until(3'000, [&] {
            restored_state = capture.mailbox_state();
            return restored_state.region_available
                && restored_state.source_width == kPatternWidth
                && restored_state.source_height == kPatternHeight;
        })
        || restored_state.generation != initial_state.generation) {
        fail("640 WGC direct bus did not restore its existing mailbox epoch");
    }
    set_pattern_phase(target, 1);
    const std::uint64_t restored_sequence = child.acquire_validate(
        mailbox.x, mailbox.y, 640, 640, 1);
    const auto restored_metadata = child.metadata();
    require_wgc_bus_metadata(
        restored_metadata,
        mailbox.x,
        mailbox.y,
        640,
        640,
        restored_state.generation,
        "restored absolute 640");
    if (restored_sequence <= initial_sequence
        || restored_sequence <= unavailable_sequence
        || capture.last_error().status != gpu::WgcStatus::ok) {
        fail("restored 640 WGC direct bus did not resume publication");
    }
    if (restored_metadata.timestamp_qpc <= initial_metadata.timestamp_qpc
        || restored_metadata.source_timestamp_100ns
            < initial_metadata.source_timestamp_100ns) {
        fail("restored 640 WGC direct-bus timestamps did not advance monotonically");
    }
    child.release();
    require_direct_bus_stats(capture, publisher, "640 ROI");
    capture.stop();
    child.close_and_unregister(publisher);
}

void test_wgc_direct_bus_full_frame_resize_guard(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& options) {
    set_pattern_phase(target, 0);
    gpu::SharedFrameBusConfig config = direct_bus_config(1);
    config.width = static_cast<std::uint32_t>(kPatternWidth);
    config.height = static_cast<std::uint32_t>(kPatternHeight);
    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
        device, config, publisher);
    if (!bus_created) {
        fail(std::string("full-frame resize-guard bus create failed: ")
            + bus_created.what());
    }
    BusConsumerChild child;
    child.attach(publisher);
    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::full_frame;
    gpu::WgcCapture capture;
    const gpu::WgcResult created = gpu::WgcCapture::create_for_window_to_bus(
        target, publisher, options, mailbox, capture);
    if (!created) {
        fail("full-frame resize-guard WGC create failed: " + created.message);
    }
    const gpu::WgcResult started = capture.start();
    if (!started) {
        fail("full-frame resize-guard WGC start failed: " + started.message);
    }
    const auto initial_state = capture.mailbox_state();
    const std::uint64_t initial_sequence = child.acquire_validate(
        0,
        0,
        static_cast<std::uint32_t>(kPatternWidth),
        static_cast<std::uint32_t>(kPatternHeight),
        0);
    const auto initial_metadata = child.metadata();
    require_wgc_bus_metadata(
        initial_metadata,
        0,
        0,
        static_cast<std::uint32_t>(kPatternWidth),
        static_cast<std::uint32_t>(kPatternHeight),
        initial_state.generation,
        "initial full-frame resize guard");
    child.release();

    resize_pattern_window(target, true);
    gpu::WgcMailboxState unavailable_state;
    if (!wait_until(3'000, [&] {
            unavailable_state = capture.mailbox_state();
            return !unavailable_state.region_available
                && unavailable_state.source_width == 639
                && unavailable_state.source_height == 639
                && capture.last_error().status
                    == gpu::WgcStatus::region_unavailable;
        })) {
        fail("full-frame WGC resize did not require a new bus epoch");
    }
    const std::uint64_t unavailable_sequence = publisher.sequence();
    const auto unavailable_stats = publisher.stats();
    const auto received_before_drop = capture.stats().received_frames;
    set_pattern_phase(target, 1);
    if (!wait_until(3'000, [&] {
            return capture.stats().received_frames > received_before_drop;
        })) {
        fail("resized full-frame WGC source emitted no test presentation");
    }
    if (publisher.sequence() != unavailable_sequence
        || unavailable_sequence < initial_sequence
        || publisher.stats().published_frames != unavailable_stats.published_frames) {
        fail("full-frame size mismatch advanced the fixed shared bus");
    }

    resize_pattern_window(target, false);
    gpu::WgcMailboxState restored_state;
    if (!wait_until(3'000, [&] {
            restored_state = capture.mailbox_state();
            return restored_state.region_available
                && restored_state.source_width
                    == static_cast<std::uint32_t>(kPatternWidth)
                && restored_state.source_height
                    == static_cast<std::uint32_t>(kPatternHeight);
        })) {
        fail("full-frame WGC resize guard did not recover at epoch dimensions");
    }
    set_pattern_phase(target, 1);
    const std::uint64_t restored_sequence = child.acquire_validate(
        0,
        0,
        static_cast<std::uint32_t>(kPatternWidth),
        static_cast<std::uint32_t>(kPatternHeight),
        1);
    const auto restored_metadata = child.metadata();
    require_wgc_bus_metadata(
        restored_metadata,
        0,
        0,
        static_cast<std::uint32_t>(kPatternWidth),
        static_cast<std::uint32_t>(kPatternHeight),
        restored_state.generation,
        "restored full-frame resize guard");
    if (restored_sequence <= unavailable_sequence
        || restored_metadata.timestamp_qpc <= initial_metadata.timestamp_qpc) {
        fail("full-frame WGC resize guard did not resume coherently");
    }
    child.release();
    capture.stop();
    child.close_and_unregister(publisher);
}

void test_recoverable_wgc_full_frame_resize(
    HWND target,
    const gpu::WgcCaptureOptions& source_options) {
    struct ScopedWindowRestore final {
        HWND window = nullptr;
        ~ScopedWindowRestore() {
            try {
                resize_pattern_window(window, false);
            } catch (...) {
            }
        }
    } restore{target};

    resize_pattern_window(target, false);
    set_pattern_phase(target, 0);

    gpu::RecoverableGpuPipelineConfig config;
    config.capture = source_options;
    config.capture.damage_mode = gpu::WgcDamageMode::disabled;
    config.mailbox.mode = gpu::WgcMailboxMode::full_frame;
    config.pipeline.transform.input_width = 320;
    config.pipeline.transform.input_height = 320;
    config.pipeline.transform.output_width = 320;
    config.pipeline.transform.output_height = 320;
    config.pipeline.transform.output_format = gpu::GpuPixelFormat::nv12;
    config.pipeline.transform.output_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    config.pipeline.encoder.codec = gpu::VideoCodec::h264;
    config.pipeline.encoder.width = 320;
    config.pipeline.encoder.height = 320;
    config.pipeline.encoder.input_format = DXGI_FORMAT_NV12;
    config.pipeline.encoder.frame_rate_numerator = 60;
    config.pipeline.encoder.frame_rate_denominator = 1;
    config.pipeline.encoder.bitrate = 2'000'000;
    config.pipeline.encoder.input_pool_size = 3;
    config.pipeline.queue_depth = 3;
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;
    config.bus_slot_count = 3;
    config.retry.monitor_interval_ms = 5;
    config.retry.initial_backoff_ms = 5;
    config.retry.maximum_backoff_ms = 100;

    std::atomic<std::uint64_t> packet_count{0};
    const auto count_packet = +[](void* context, const gpu::EncodedPacket& packet) {
        if (packet.data != nullptr && packet.size != 0) {
            static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(
                1, std::memory_order_release);
        }
    };

    gpu::RecoverableGpuPipeline pipeline;
    const auto created = gpu::RecoverableGpuPipeline::create_for_window(
        target, config, count_packet, &packet_count, pipeline);
    if (!created) {
        fail("recoverable full-frame WGC create failed: " + created.message);
    }
    const auto started = pipeline.start();
    if (!started) {
        fail("recoverable full-frame WGC start failed: " + started.message);
    }

    const auto initial = pipeline.snapshot();
    if (initial.state != gpu::RecoverableGpuPipelineState::running
        || initial.epoch == 0 || initial.epoch_nonce == 0
        || initial.active_source_width
            != static_cast<std::uint32_t>(kPatternWidth)
        || initial.active_source_height
            != static_cast<std::uint32_t>(kPatternHeight)
        || initial.active_bus_format != DXGI_FORMAT_NV12
        || !initial.active_external_encoder_input
        || initial.active_bus_width != 320
        || initial.active_bus_height != 320
        || !initial.capture_running || !initial.encoder_accepting) {
        fail("recoverable full-frame WGC initial epoch is inconsistent: source="
            + std::to_string(initial.active_source_width) + "x"
            + std::to_string(initial.active_source_height)
            + ", epoch=" + std::to_string(initial.epoch));
    }

    std::uint32_t pattern_phase = 0;
    const auto require_epoch_activity = [&]
        (const gpu::RecoverableGpuPipelineSnapshot& baseline,
         const char* label) {
        const std::uint64_t packets_before = packet_count.load(
            std::memory_order_acquire);
        const ULONGLONG deadline = GetTickCount64() + 15'000;
        ULONGLONG next_pulse = 0;
        bool activity_observed = false;
        gpu::RecoverableGpuPipelineSnapshot current;
        do {
            current = pipeline.snapshot();
            activity_observed = activity_observed
                || (current.state == gpu::RecoverableGpuPipelineState::running
                && current.epoch == baseline.epoch
                && current.epoch_nonce == baseline.epoch_nonce
                && current.active_source_width == baseline.active_source_width
                && current.active_source_height == baseline.active_source_height
                && current.capture.published_frames
                    > baseline.capture.published_frames
                && current.pipeline.processed_frames
                    > baseline.pipeline.processed_frames
                && current.pipeline.encoded_packets
                    > baseline.pipeline.encoded_packets
                && packet_count.load(std::memory_order_acquire) > packets_before
                && !current.first_recovered_frame_pending);
            if (activity_observed) {
                if (current.active_bus_format != DXGI_FORMAT_NV12
                    || !current.active_external_encoder_input
                    || current.capture.ingress_copy_submissions != 0
                    || current.bus.copied_publishes != 0
                    || current.pipeline.input_copy_submissions != 0
                    || current.pipeline.transform_submissions != 0
                    || current.pipeline.encoder_copied_submissions != 0) {
                    fail(std::string(label)
                        + " epoch violated the scaled planar zero-copy contract");
                }
                // These counters come from three independently running
                // components and snapshot() intentionally does not stop the
                // graph. Wait for one quiescent observation instead of
                // treating a frame committed between atomic loads as a copy
                // contract failure.
                if (current.capture.ingress_transform_submissions
                        == current.capture.published_frames
                    && current.bus.direct_publishes
                        == current.capture.published_frames
                    && current.pipeline.encoder_external_submissions
                        == current.pipeline.processed_frames
                    && current.pipeline
                            .encoder_external_identity_verified_submissions
                        == current.pipeline.encoder_external_submissions) {
                    return current;
                }
            }
            const ULONGLONG now = GetTickCount64();
            if (!activity_observed && now >= next_pulse) {
                pattern_phase ^= 1u;
                set_pattern_phase(target, pattern_phase);
                next_pulse = now + 50;
            }
            Sleep(2);
        } while (GetTickCount64() < deadline);

        const auto error = pipeline.last_error();
        fail(std::string(label)
            + " epoch produced no post-build frame/packet: state="
            + gpu::recoverable_gpu_pipeline_state_string(current.state)
            + ", published=" + std::to_string(current.capture.published_frames)
            + ", processed=" + std::to_string(current.pipeline.processed_frames)
            + ", packets=" + std::to_string(current.pipeline.encoded_packets)
            + ", error=" + error.message);
    };

    const auto wait_for_fresh_epoch = [&]
        (const gpu::RecoverableGpuPipelineSnapshot& previous,
         std::uint32_t expected_width,
         std::uint32_t expected_height,
         const char* label) {
        gpu::RecoverableGpuPipelineSnapshot current;
        const bool rebuilt = wait_until(20'000, [&] {
            current = pipeline.snapshot();
            return current.state == gpu::RecoverableGpuPipelineState::running
                && current.epoch > previous.epoch
                && current.epoch_nonce != 0
                && current.epoch_nonce != previous.epoch_nonce
                && current.active_source_width == expected_width
                && current.active_source_height == expected_height;
        });
        if (!rebuilt) {
            const auto error = pipeline.last_error();
            fail(std::string(label) + " did not install a fresh source epoch: state="
                + gpu::recoverable_gpu_pipeline_state_string(current.state)
                + ", source=" + std::to_string(current.active_source_width) + "x"
                + std::to_string(current.active_source_height)
                + ", epoch=" + std::to_string(current.epoch)
                + ", error=" + error.message);
        }
        if (current.active_bus_format != DXGI_FORMAT_NV12
            || !current.active_external_encoder_input
            || current.active_bus_width != 320
            || current.active_bus_height != 320
            || !current.capture_running || !current.encoder_accepting) {
            fail(std::string(label)
                + " fresh epoch changed the scaled planar contract");
        }
        return current;
    };

    const auto initial_active = require_epoch_activity(initial, "initial");

    resize_pattern_window(target, true);
    const auto shrunk = wait_for_fresh_epoch(
        initial_active, 639, 639, "shrunk full-frame WGC");
    const auto shrunk_active = require_epoch_activity(shrunk, "shrunk");

    resize_pattern_window(target, false);
    const auto restored = wait_for_fresh_epoch(
        shrunk_active,
        initial.active_source_width,
        initial.active_source_height,
        "restored full-frame WGC");
    const auto restored_active = require_epoch_activity(restored, "restored");
    if (restored_active.recovery_attempts < 2
        || restored_active.recovery_successes < 2
        || restored_active.rebuild_attempts < 3) {
        fail("recoverable full-frame resize bypassed the epoch recovery path");
    }

    pipeline.stop();
    if (pipeline.snapshot().state != gpu::RecoverableGpuPipelineState::stopped) {
        fail("recoverable full-frame WGC did not stop cleanly");
    }
    resize_pattern_window(target, false);
}

void test_wgc_direct_bus_api_validation(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& options) {
    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::centered_region;
    mailbox.width = 320;
    mailbox.height = 320;

    gpu::SharedFrameBusPublisher wrong_size_bus;
    const gpu::GpuError wrong_size_created = gpu::SharedFrameBusPublisher::create(
        device, direct_bus_config(319), wrong_size_bus);
    if (!wrong_size_created) {
        fail(std::string("mismatched-size test bus create failed: ")
            + wrong_size_created.what());
    }
    gpu::WgcCapture wrong_size_capture;
    const gpu::WgcResult wrong_size = gpu::WgcCapture::create_for_window_to_bus(
        target, wrong_size_bus, options, mailbox, wrong_size_capture);
    gpu::SharedFrameBusWriteLease size_probe;
    const gpu::GpuError size_probe_result = wrong_size_bus.begin_publish(
        0, size_probe);
    if (wrong_size
        || wrong_size.status != gpu::WgcStatus::invalid_argument
        || wrong_size_capture.device() != nullptr
        || !size_probe_result
        || !size_probe) {
        fail("WGC direct bus accepted mismatched output dimensions or leaked its reservation");
    }
    size_probe.reset();

    gpu::SharedFrameBusConfig wrong_format_config = direct_bus_config(320);
    wrong_format_config.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpu::SharedFrameBusPublisher wrong_format_bus;
    const gpu::GpuError wrong_format_created =
        gpu::SharedFrameBusPublisher::create(
            device, wrong_format_config, wrong_format_bus);
    if (!wrong_format_created) {
        fail(std::string("mismatched-format test bus create failed: ")
            + wrong_format_created.what());
    }
    gpu::WgcCapture wrong_format_capture;
    const gpu::WgcResult wrong_format = gpu::WgcCapture::create_for_window_to_bus(
        target, wrong_format_bus, options, mailbox, wrong_format_capture);
    gpu::SharedFrameBusWriteLease format_probe;
    const gpu::GpuError format_probe_result = wrong_format_bus.begin_publish(
        0, format_probe);
    if (wrong_format
        || wrong_format.status != gpu::WgcStatus::invalid_argument
        || wrong_format_capture.device() != nullptr
        || !format_probe_result
        || !format_probe) {
        fail("WGC direct bus accepted a non-BGRA format or leaked its reservation");
    }
    format_probe.reset();
}

void test_wgc_direct_bus_no_slot_recovery(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& options) {
    set_pattern_phase(target, 0);
    gpu::SharedFrameBusConfig config = direct_bus_config(320, 2);
    config.slot_count = 2;
    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
        device, config, publisher);
    if (!bus_created) {
        fail(std::string("no-slot WGC direct bus create failed: ")
            + bus_created.what());
    }

    gpu::SharedFrameBusRegistration registration_a;
    gpu::SharedFrameBusRegistration registration_b;
    const gpu::GpuError registered_a = publisher.register_consumer(
        GetCurrentProcess(), registration_a);
    const gpu::GpuError registered_b = publisher.register_consumer(
        GetCurrentProcess(), registration_b);
    if (!registered_a || !registered_b) {
        fail("failed to register same-process no-slot consumers");
    }
    gpu::SharedFrameBusConsumer consumer_a;
    gpu::SharedFrameBusConsumer consumer_b;
    const gpu::GpuError opened_a = gpu::SharedFrameBusConsumer::open(
        device, registration_a, true, consumer_a);
    const gpu::GpuError opened_b = gpu::SharedFrameBusConsumer::open(
        device, registration_b, true, consumer_b);
    if (!opened_a || !opened_b) {
        fail("failed to open same-process no-slot consumers");
    }

    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::centered_region;
    mailbox.width = 320;
    mailbox.height = 320;
    gpu::WgcCapture capture;
    const gpu::WgcResult created = gpu::WgcCapture::create_for_window_to_bus(
        target, publisher, options, mailbox, capture);
    if (!created) fail("no-slot WGC direct-bus create failed: " + created.message);
    const gpu::WgcResult started = capture.start();
    if (!started) fail("no-slot WGC direct-bus start failed: " + started.message);

    BusChildCommand expected;
    expected.operation = BusChildOperation::acquire_validate;
    expected.source_x = 240;
    expected.source_y = 239;
    expected.width = 320;
    expected.height = 320;
    expected.phase = 0;
    gpu::SharedFrameBusFrameLease held_a;
    const gpu::GpuError acquired_a = consumer_a.acquire_latest(5'000, held_a);
    if (!acquired_a) fail("first no-slot consumer did not acquire its frame");
    validate_bus_roi(device, capture.context(), held_a.texture(), expected);
    const std::uint64_t sequence_a = held_a.info().sequence;
    const auto metadata_a = held_a.metadata();
    const auto no_slot_mailbox = capture.mailbox_state();
    require_wgc_bus_metadata(
        metadata_a,
        expected.source_x,
        expected.source_y,
        expected.width,
        expected.height,
        no_slot_mailbox.generation,
        "first pinned no-slot frame");

    set_pattern_phase(target, 1);
    if (!wait_until(3'000, [&] { return publisher.sequence() > sequence_a; })) {
        fail("second no-slot source presentation was not published");
    }
    expected.phase = 1;
    gpu::SharedFrameBusFrameLease held_b;
    const gpu::GpuError acquired_b = consumer_b.acquire_latest(5'000, held_b);
    if (!acquired_b || held_b.info().sequence <= sequence_a) {
        fail("second no-slot consumer did not pin a distinct newer slot");
    }
    validate_bus_roi(device, capture.context(), held_b.texture(), expected);
    const auto metadata_b = held_b.metadata();
    require_wgc_bus_metadata(
        metadata_b,
        expected.source_x,
        expected.source_y,
        expected.width,
        expected.height,
        no_slot_mailbox.generation,
        "second pinned no-slot frame");

    const std::uint64_t pinned_sequence = publisher.sequence();
    const gpu::SharedFrameBusStats before_full = publisher.stats();
    const gpu::WgcCaptureStats capture_before_full = capture.stats();
    set_pattern_phase(target, 0);
    if (!wait_until(3'000, [&] {
            const gpu::WgcCaptureStats current = capture.stats();
            return current.received_frames > capture_before_full.received_frames
                && current.skipped_no_buffer
                    > capture_before_full.skipped_no_buffer;
        })) {
        fail("full two-slot bus did not produce an observable nonblocking WGC drop");
    }
    const gpu::SharedFrameBusStats while_full = publisher.stats();
    const auto held_metadata_a_after_drop = held_a.metadata();
    const auto held_metadata_b_after_drop = held_b.metadata();
    if (publisher.sequence() != pinned_sequence
        || while_full.published_frames != before_full.published_frames
        || while_full.no_slot <= before_full.no_slot
        || std::memcmp(
            &held_metadata_a_after_drop, &metadata_a, sizeof(metadata_a)) != 0
        || std::memcmp(
            &held_metadata_b_after_drop, &metadata_b, sizeof(metadata_b)) != 0) {
        fail("full two-slot bus advanced sequence instead of dropping the WGC frame");
    }

    const gpu::GpuError released_a = consumer_a.release(held_a);
    if (!released_a) fail("failed to release a no-slot recovery slot");
    set_pattern_phase(target, 1);
    if (!wait_until(3'000, [&] {
            return publisher.sequence() > pinned_sequence;
        })) {
        fail("WGC direct bus did not recover after a slot was released");
    }
    gpu::SharedFrameBusFrameLease recovered;
    const gpu::GpuError acquired_recovered = consumer_a.acquire_latest(
        5'000, recovered);
    if (!acquired_recovered || recovered.info().sequence <= pinned_sequence) {
        fail("released no-slot consumer could not acquire the recovered frame");
    }
    expected.phase = 1;
    validate_bus_roi(device, capture.context(), recovered.texture(), expected);
    const auto recovered_metadata = recovered.metadata();
    require_wgc_bus_metadata(
        recovered_metadata,
        expected.source_x,
        expected.source_y,
        expected.width,
        expected.height,
        no_slot_mailbox.generation,
        "recovered no-slot frame");
    if (recovered_metadata.timestamp_qpc <= metadata_b.timestamp_qpc) {
        fail("recovered no-slot metadata did not advance its publication QPC");
    }
    const gpu::GpuError released_recovered = consumer_a.release(recovered);
    const gpu::GpuError released_b = consumer_b.release(held_b);
    if (!released_recovered || !released_b) {
        fail("failed to release recovered no-slot frame leases");
    }

    const gpu::WgcCaptureStats final_capture_stats = capture.stats();
    const gpu::SharedFrameBusStats final_bus_stats = publisher.stats();
    if (final_capture_stats.published_frames != final_bus_stats.published_frames
        || final_bus_stats.published_frames != final_bus_stats.direct_publishes
        || final_bus_stats.copied_publishes != 0
        || final_bus_stats.no_slot == 0
        || final_bus_stats.publish_attempts
            != final_bus_stats.published_frames + final_bus_stats.no_slot) {
        fail("no-slot recovery statistics are inconsistent");
    }
    capture.stop();
    const gpu::GpuError closed_a = consumer_a.close();
    if (!closed_a) fail("failed to close first no-slot consumer");
    const gpu::GpuError unregistered_a = publisher.unregister_consumer(
        registration_a, 3'000);
    if (!unregistered_a) fail("failed to unregister first no-slot consumer");
    const gpu::GpuError closed_b = consumer_b.close();
    if (!closed_b) fail("failed to close second no-slot consumer");
    const gpu::GpuError unregistered_b = publisher.unregister_consumer(
        registration_b, 3'000);
    if (!unregistered_b) fail("failed to unregister second no-slot consumer");
}

void test_wgc_retains_bus_after_publisher_wrapper_destruction(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& options) {
    set_pattern_phase(target, 1);
    BusConsumerChild child;
    gpu::WgcCapture capture;
    {
        gpu::SharedFrameBusPublisher publisher;
        const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
            device, direct_bus_config(320), publisher);
        if (!bus_created) {
            fail(std::string("lifetime WGC direct bus create failed: ")
                + bus_created.what());
        }
        child.attach(publisher);
        gpu::WgcMailboxConfig mailbox;
        mailbox.mode = gpu::WgcMailboxMode::centered_region;
        mailbox.width = 320;
        mailbox.height = 320;
        const gpu::WgcResult created = gpu::WgcCapture::create_for_window_to_bus(
            target, publisher, options, mailbox, capture);
        if (!created) {
            fail("lifetime WGC direct-bus create failed: " + created.message);
        }
        gpu::SharedFrameBusPublisher moved = std::move(publisher);
        if (publisher.initialized() || !moved.initialized()) {
            fail("SharedFrameBusPublisher move did not transfer its wrapper state");
        }
    }

    const gpu::WgcResult started = capture.start();
    if (!started) {
        fail("WGC lost its bus after publisher wrapper destruction: "
            + started.message);
    }
    child.acquire_validate(240, 239, 320, 320, 1);
    child.release();
    if (capture.stats().published_frames == 0) {
        fail("WGC retained-bus lifetime test published no frame");
    }
    child.close_after_publisher_wrapper_destroyed();
    capture.stop();
}

bool test_desktop_duplication_native_metadata(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    DXGI_OUTPUT_DESC output_description{};
    const HRESULT output_found = query_monitor_output_description(
        monitor, output_description);
    if (FAILED(output_found)) {
        fail("Desktop Duplication output/rotation query failed: "
            + std::to_string(output_found));
    }
    DXGI_MODE_ROTATION rotation = output_description.Rotation;
    if (rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
        rotation = DXGI_MODE_ROTATION_IDENTITY;
    }
    if (rotation != DXGI_MODE_ROTATION_IDENTITY
        && rotation != DXGI_MODE_ROTATION_ROTATE90
        && rotation != DXGI_MODE_ROTATION_ROTATE180
        && rotation != DXGI_MODE_ROTATION_ROTATE270) {
        fail("Desktop Duplication reported an invalid DXGI rotation");
    }
    const bool rotated = rotation != DXGI_MODE_ROTATION_IDENTITY;
    wchar_t strict_move_value[8]{};
    const bool require_native_move = GetEnvironmentVariableW(
        L"FLUXCAP_DD_REQUIRE_NATIVE_MOVE",
        strict_move_value,
        static_cast<DWORD>(std::size(strict_move_value))) != 0
        && strict_move_value[0] != L'0';

    MONITORINFO monitor_info{sizeof(monitor_info)};
    RECT client_rect{};
    POINT client_origin{};
    if (!GetMonitorInfoW(monitor, &monitor_info)
        || !GetClientRect(target, &client_rect)
        || !ClientToScreen(target, &client_origin)) {
        fail("Desktop Duplication target/monitor geometry query failed");
    }
    if (!SetWindowPos(
            target,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        fail("Desktop Duplication test target could not be made visible");
    }
    (void)DwmFlush();

    gpu::SharedFrameBusConfig bus_config = direct_bus_config(320);
    bus_config.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    // Rotated Desktop Duplication writes with a VideoProcessor transform;
    // identity rotation still uses CopySubresourceRegion into this resource.
    bus_config.bind_flags |= D3D11_BIND_RENDER_TARGET;
    bus_config.slot_count = 4;
    gpu::SharedFrameBusPublisher publisher;
    const auto bus_created = gpu::SharedFrameBusPublisher::create(
        device, bus_config, publisher);
    if (!bus_created) {
        fail(std::string("Desktop Duplication bus create failed: ")
            + bus_created.what());
    }
    gpu::SharedFrameBusRegistration registration;
    auto bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("Desktop Duplication bus registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("Desktop Duplication bus consumer open failed");

    gpu::WgcCaptureOptions options = source_options;
    options.include_cursor = false;
    options.include_cursor_metadata = true;
    options.damage_mode = gpu::WgcDamageMode::native_report_only;
    options.pixel_format = gpu::WgcPixelFormat::bgra8;
    options.capture_epoch = 31;
    options.capture_epoch_nonce = 0xa1d7'54e3ull;
    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::absolute_region;
    const std::uint32_t pattern_source_x = static_cast<std::uint32_t>(
        (client_rect.right - client_rect.left
            - static_cast<LONG>(bus_config.width)) / 2);
    const std::uint32_t pattern_source_y = static_cast<std::uint32_t>(
        (client_rect.bottom - client_rect.top
            - static_cast<LONG>(bus_config.height)) / 2);
    mailbox.x = static_cast<std::uint32_t>(
        client_origin.x - monitor_info.rcMonitor.left) + pattern_source_x;
    mailbox.y = static_cast<std::uint32_t>(
        client_origin.y - monitor_info.rcMonitor.top) + pattern_source_y;
    mailbox.width = bus_config.width;
    mailbox.height = bus_config.height;
    set_pattern_phase(target, 1);

    gpu::DesktopDuplicationCapture capture;
    const auto created =
        gpu::DesktopDuplicationCapture::create_for_monitor_to_bus(
            monitor, publisher, options, mailbox, capture);
    if (!created && (created.status == gpu::WgcStatus::not_supported
            || created.hresult == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
            || created.hresult == DXGI_ERROR_UNSUPPORTED)) {
        std::cout << "[SKIP] Desktop Duplication native metadata"
            << " (rotation=" << static_cast<unsigned>(rotation)
            << ", status=" << static_cast<unsigned>(created.status)
            << ", hr=" << created.hresult
            << ", message=" << created.message << ")\n";
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        (void)SetWindowPos(
            target, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return false;
    }
    if (!created) {
        fail("Desktop Duplication capture create failed: " + created.message);
    }
    const auto started = capture.start();
    if (!started) {
        fail("Desktop Duplication capture start failed: " + started.message);
    }

    POINT original_cursor{};
    const bool had_cursor_position = GetCursorPos(&original_cursor) != FALSE;
    HCURSOR cross = LoadCursorW(nullptr, IDC_CROSS);
    HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    const LONG_PTR previous_class_cursor = SetClassLongPtrW(
        target, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(cross));
    const int parked_cursor_x = monitor_info.rcMonitor.left + 8;
    const int parked_cursor_y = monitor_info.rcMonitor.top + 8;
    if (!SetCursorPos(parked_cursor_x, parked_cursor_y)) {
        fail("Desktop Duplication cursor parking failed");
    }
    (void)DwmFlush();

    // Validate orientation while the system pointer is outside the ROI. Some
    // adapters bake a hardware cursor into the duplicated desktop image even
    // though pointer shape/position metadata is also present.
    std::uint64_t pixel_sequence = 0;
    for (std::uint32_t attempt = 0; attempt < 8 && pixel_sequence == 0;
         ++attempt) {
        gpu::SharedFrameBusFrameLease pixel_frame;
        bus_result = consumer.acquire_latest(5'000, pixel_frame);
        if (!bus_result) {
            fail("Desktop Duplication orientation frame timed out: "
                + capture.last_error().message);
        }
        const auto& parked = pixel_frame.side_data().cursor;
        if ((parked.flags & gpu::wgc_cursor_position_valid) != 0
            && parked.screen_x == parked_cursor_x
            && parked.screen_y == parked_cursor_y) {
            BusChildCommand expected;
            expected.width = bus_config.width;
            expected.height = bus_config.height;
            expected.source_x = pattern_source_x;
            expected.source_y = pattern_source_y;
            expected.phase = 1;
            validate_bus_roi(
                device, capture.context(), pixel_frame.texture(), expected);
            pixel_sequence = pixel_frame.info().sequence;
        }
        bus_result = consumer.release(pixel_frame);
        if (!bus_result) {
            fail("Desktop Duplication orientation frame release failed");
        }
    }
    if (pixel_sequence == 0) {
        fail("Desktop Duplication did not publish parked cursor coordinates");
    }

    const int cursor_x = client_origin.x
        + static_cast<int>(pattern_source_x + bus_config.width / 2);
    const int cursor_y = client_origin.y
        + static_cast<int>(pattern_source_y + bus_config.height / 2);
    if (!SetCursorPos(cursor_x, cursor_y)) {
        fail("Desktop Duplication cursor positioning failed");
    }
    (void)SendMessageW(
        target,
        WM_SETCURSOR,
        reinterpret_cast<WPARAM>(target),
        MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    set_pattern_phase(target, 1);
    (void)DwmFlush();

    bool saw_native_frame = false;
    bool saw_native_move = false;
    bool saw_cursor_position = false;
    bool saw_cursor_shape = false;
    std::uint64_t last_sequence = pixel_sequence;
    RECT original_bounds{};
    (void)GetWindowRect(target, &original_bounds);
    for (std::uint32_t attempt = 0; attempt < 12
         && (!saw_native_move || !saw_cursor_position || !saw_cursor_shape);
         ++attempt) {
        gpu::SharedFrameBusFrameLease frame;
        bus_result = consumer.acquire_latest(5'000, frame);
        if (!bus_result) {
            fail("Desktop Duplication frame timed out: "
                + capture.last_error().message);
        }
        const auto metadata = frame.metadata();
        const auto side_data = frame.side_data();
        const auto& damage = side_data.damage;
        if (frame.info().sequence <= last_sequence
            || frame.info().format != DXGI_FORMAT_B8G8R8A8_UNORM
            || metadata.color_space
                != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
            || side_data.epoch != options.capture_epoch
            || side_data.epoch_nonce != options.capture_epoch_nonce
            || (damage.flags & gpu::wgc_damage_valid) == 0
            || (damage.flags & gpu::wgc_damage_native) == 0
            || (damage.flags & gpu::wgc_damage_native_move_available) == 0
            || (damage.flags & gpu::wgc_damage_native_move_unavailable) != 0
            || (damage.flags & gpu::wgc_damage_inferred_move) != 0) {
            fail("Desktop Duplication frame provenance is inconsistent");
        }
        saw_native_frame = true;
        if (damage.move_count != 0
            && (damage.base_sequence == 0
                || damage.base_sequence >= frame.info().sequence)) {
            fail("Desktop Duplication native move lost its base sequence");
        }
        saw_native_move = saw_native_move || damage.move_count != 0;
        for (std::uint32_t index = 0; index < damage.dirty_count; ++index) {
            const auto& dirty = damage.dirty_rects[index];
            if (dirty.x < 0 || dirty.y < 0
                || static_cast<std::uint64_t>(dirty.x) + dirty.width
                    > bus_config.width
                || static_cast<std::uint64_t>(dirty.y) + dirty.height
                    > bus_config.height) {
                fail("Desktop Duplication dirty rectangle escaped the ROI");
            }
        }
        if (side_data.cursor.shape_sequence != 0) {
            gpu::WgcCursorShape shape;
            gpu::GpuError shape_result;
            const bool shape_available = wait_until(2'000, [&] {
                shape_result = consumer.cursor_shape(
                    side_data.cursor.shape_sequence, shape);
                return static_cast<bool>(shape_result);
            });
            if (!shape_available
                || shape.sequence != side_data.cursor.shape_sequence
                || shape.sequence
                    != gpu::internal::desktop_duplication_cursor_shape_key(shape)
                || shape.width != side_data.cursor.width
                || shape.height != side_data.cursor.height
                || shape.hotspot_x != side_data.cursor.hotspot_x
                || shape.hotspot_y != side_data.cursor.hotspot_y
                || shape.kind == gpu::WgcCursorShapeKind::none
                || shape.data.empty()) {
                fail("Desktop Duplication cursor shape/hotspot did not round-trip: "
                    + std::string(shape_result.what()));
            }
            saw_cursor_shape = true;
        }
        const auto& cursor = side_data.cursor;
        if ((cursor.flags & gpu::wgc_cursor_position_valid) != 0
            && cursor.screen_x == cursor_x
            && cursor.screen_y == cursor_y) {
            const std::int32_t expected_frame_x = cursor_x
                - monitor_info.rcMonitor.left
                - static_cast<std::int32_t>(mailbox.x);
            const std::int32_t expected_frame_y = cursor_y
                - monitor_info.rcMonitor.top
                - static_cast<std::int32_t>(mailbox.y);
            if (cursor.frame_x != expected_frame_x
                || cursor.frame_y != expected_frame_y
                || (cursor.flags & gpu::wgc_cursor_visible) == 0
                || cursor.sample_qpc == 0) {
                fail("Desktop Duplication cursor screen/frame mapping is inconsistent");
            }
            saw_cursor_position = true;
        }

        last_sequence = frame.info().sequence;
        bus_result = consumer.release(frame);
        if (!bus_result) fail("Desktop Duplication bus release failed");

        if (attempt == 0) {
            (void)SetWindowPos(
                target,
                nullptr,
                original_bounds.left + 32,
                original_bounds.top + 32,
                0,
                0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            set_pattern_phase(target, (attempt & 1u) == 0 ? 0 : 1);
        }
        (void)DwmFlush();
    }

    (void)SetWindowPos(
        target,
        nullptr,
        original_bounds.left,
        original_bounds.top,
        0,
        0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    (void)SetClassLongPtrW(
        target,
        GCLP_HCURSOR,
        previous_class_cursor != 0
            ? previous_class_cursor
            : reinterpret_cast<LONG_PTR>(arrow));
    if (had_cursor_position) (void)SetCursorPos(original_cursor.x, original_cursor.y);
    capture.stop();
    if (!saw_native_frame || (require_native_move && !saw_native_move)
        || !saw_cursor_position || !saw_cursor_shape) {
        fail("Desktop Duplication metadata missing: native_frame="
            + std::to_string(saw_native_frame)
            + ", native_move=" + std::to_string(saw_native_move)
            + ", cursor_position=" + std::to_string(saw_cursor_position)
            + ", cursor_shape=" + std::to_string(saw_cursor_shape)
            + ", last_sequence=" + std::to_string(last_sequence)
            + ", capture_error=" + capture.last_error().message);
    }
    const auto capture_stats = capture.stats();
    const auto bus_stats = publisher.stats();
    const std::uint64_t desktop_present_frames =
        capture.desktop_present_frames();
    if (capture_stats.published_frames == 0
        || desktop_present_frames == 0
        || desktop_present_frames > capture_stats.published_frames
        || capture_stats.native_damage_frames
            != capture_stats.published_frames
        || capture_stats.ingress_copy_submissions != (rotated
            ? 0 : capture_stats.published_frames)
        || capture_stats.ingress_transform_submissions != (rotated
            ? capture_stats.published_frames : 0)
        || capture_stats.cursor_metadata_frames
            != capture_stats.published_frames
        || capture_stats.cursor_shape_updates == 0
        || bus_stats.direct_publishes != capture_stats.published_frames
        || bus_stats.copied_publishes != 0) {
        fail("Desktop Duplication changed the zero-redundancy ingress contract");
    }

    // Run the GPU cursor-compositing contract after the shared-bus stats
    // were snapshotted so its publishes cannot pollute the counters above.
    auto baked_cursor_options = options;
    baked_cursor_options.include_cursor = true;
    baked_cursor_options.include_cursor_metadata = false;
    gpu::DesktopDuplicationCapture baked_cursor_capture;
    const auto baked_cursor_created =
        gpu::DesktopDuplicationCapture::create_for_monitor_to_bus(
            monitor,
            publisher,
            baked_cursor_options,
            mailbox,
            baked_cursor_capture);
    if (!baked_cursor_created
        && baked_cursor_created.status == gpu::WgcStatus::not_supported) {
        std::cout << "[SKIP] Desktop Duplication GPU cursor compositing: "
                  << baked_cursor_created.message << "\n";
    } else if (!baked_cursor_created) {
        fail("Desktop Duplication GPU cursor compositing create failed: "
            + baked_cursor_created.message);
    } else {
        const auto baked_started = baked_cursor_capture.start();
        if (!baked_started) {
            fail("Desktop Duplication GPU cursor compositing start failed: "
                + baked_started.message);
        }
        set_pattern_phase(target, 1);
        for (int spin = 0; spin < 300; ++spin) {
            if (baked_cursor_capture.stats().published_frames > 0) break;
            Sleep(10);
        }
        const auto baked_stats = baked_cursor_capture.stats();
        baked_cursor_capture.stop();
        std::cout << "[diag] cursor compositing stats: published="
                  << baked_stats.published_frames << ", copies="
                  << baked_stats.ingress_copy_submissions << ", transforms="
                  << baked_stats.ingress_transform_submissions
                  << ", cursor_metadata="
                  << baked_stats.cursor_metadata_frames << "\n";
        if (baked_stats.published_frames == 0
            || baked_stats.ingress_copy_submissions
                < baked_stats.published_frames
            || baked_stats.cursor_metadata_frames != 0) {
            fail("Desktop Duplication GPU cursor compositing contract is "
                "inconsistent");
        }
    }

    bus_result = consumer.close();
    if (!bus_result) fail("Desktop Duplication consumer close failed");
    bus_result = publisher.unregister_consumer(registration, 5'000);
    if (!bus_result) fail("Desktop Duplication left a bus slot pinned");
    (void)SetWindowPos(
        target, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    std::cout << "[PASS] Desktop Duplication native metadata executed"
        << " (rotation=" << static_cast<unsigned>(rotation)
        << ", frames=" << capture_stats.published_frames
        << ", native_move=" << (saw_native_move ? "observed" : "not-observed")
        << ")\n";
    return true;
}

void test_recoverable_desktop_duplication_zero_copy(HWND target) {
    const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    gpu::RecoverableGpuPipelineConfig config;
    config.capture.include_cursor = false;
    config.capture.include_cursor_metadata = true;
    config.capture.damage_mode = gpu::WgcDamageMode::native_report_only;
    config.capture.pixel_format = gpu::WgcPixelFormat::bgra8;
    config.mailbox.mode = gpu::WgcMailboxMode::centered_region;
    config.mailbox.width = 320;
    config.mailbox.height = 320;
    config.pipeline.transform.input_width = 320;
    config.pipeline.transform.input_height = 320;
    config.pipeline.transform.output_width = 320;
    config.pipeline.transform.output_height = 320;
    config.pipeline.transform.input_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.pipeline.transform.input_color_space =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    config.pipeline.transform.output_format = gpu::GpuPixelFormat::nv12;
    config.pipeline.transform.output_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    config.pipeline.encoder.codec = gpu::VideoCodec::h264;
    config.pipeline.encoder.width = 320;
    config.pipeline.encoder.height = 320;
    config.pipeline.encoder.input_format = DXGI_FORMAT_NV12;
    config.pipeline.encoder.input_color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    config.pipeline.encoder.bitrate = 2'000'000;
    config.pipeline.encoder.input_pool_size = 3;
    config.pipeline.queue_depth = 3;
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;
    config.monitor_backend =
        gpu::RecoverableMonitorCaptureBackend::desktop_duplication;
    config.bus_slot_count = 3;
    config.retry.max_retry_count = 0;
    config.retry.monitor_interval_ms = 5;

    PacketCollector packets;
    gpu::RecoverableGpuPipeline pipeline;
    const auto created = gpu::RecoverableGpuPipeline::create_for_monitor(
        monitor, config, &collect_packet, &packets, pipeline);
    if (!created) {
        fail("recoverable Desktop Duplication factory failed: "
            + created.message);
    }
    const auto started = pipeline.start();
    if (!started
        && (started.status == gpu::RecoverableGpuPipelineStatus::unsupported
            || started.hresult == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
            || started.hresult == DXGI_ERROR_UNSUPPORTED)) {
        pipeline.stop();
        return;
    }
    if (!started) {
        fail("recoverable Desktop Duplication start failed: "
            + started.message);
    }
    set_pattern_phase(target, 0);
    (void)DwmFlush();
    if (!wait_until(10'000, [&] {
            const auto snapshot = pipeline.snapshot();
            return snapshot.capture.published_frames != 0
                && snapshot.pipeline.processed_frames != 0;
        })) {
        fail("recoverable Desktop Duplication processed no frame: "
            + pipeline.last_error().message);
    }
    const auto snapshot = pipeline.snapshot();
    if (snapshot.active_monitor_backend
            != gpu::RecoverableMonitorCaptureBackend::desktop_duplication
        || !snapshot.capture_running
        || !snapshot.encoder_accepting
        || snapshot.capture.native_damage_frames == 0
        || snapshot.bus.copied_publishes != 0
        || snapshot.bus.direct_publishes
            != snapshot.capture.published_frames
        || snapshot.pipeline.input_copy_submissions != 0
        || snapshot.pipeline.encoder_copied_submissions != 0) {
        fail("recoverable Desktop Duplication graph contract is inconsistent");
    }
    if (snapshot.active_bus_format == DXGI_FORMAT_NV12) {
        if (!snapshot.active_external_encoder_input
            || snapshot.capture.ingress_copy_submissions != 0
            || snapshot.capture.ingress_transform_submissions
                != snapshot.capture.published_frames
            || snapshot.pipeline.transform_submissions != 0
            || snapshot.pipeline.encoder_external_submissions
                != snapshot.pipeline.processed_frames
            || snapshot.pipeline
                    .encoder_external_identity_verified_submissions
                != snapshot.pipeline.encoder_external_submissions) {
            fail("Desktop Duplication planar graph was not single-transform external input");
        }
    } else if (snapshot.active_bus_format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        if (snapshot.active_external_encoder_input
            || snapshot.capture.ingress_copy_submissions
                != snapshot.capture.published_frames
            || snapshot.capture.ingress_transform_submissions != 0
            || snapshot.pipeline.transform_submissions
                != snapshot.pipeline.processed_frames
            || snapshot.pipeline.encoder_external_submissions != 0
            || snapshot.pipeline
                    .encoder_external_identity_verified_submissions != 0) {
            fail("Desktop Duplication native fallback copy/transform contract is inconsistent");
        }
    } else {
        fail("recoverable Desktop Duplication selected an unknown bus format");
    }
    pipeline.stop();
}

void test_desktop_duplication_hdr_p010(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    gpu::SharedFrameBusConfig bus_config = direct_bus_config(160);
    bus_config.format = DXGI_FORMAT_P010;
    bus_config.color_space =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    bus_config.bind_flags = D3D11_BIND_RENDER_TARGET;
    gpu::SharedFrameBusPublisher publisher;
    const auto bus_created = gpu::SharedFrameBusPublisher::create(
        device, bus_config, publisher);
    if (!bus_created && bus_created.status == gpu::GpuStatus::unsupported) return;
    if (!bus_created) {
        fail(std::string("HDR Desktop Duplication bus create failed: ")
            + bus_created.what());
    }
    gpu::SharedFrameBusRegistration registration;
    auto bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("HDR Desktop Duplication registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("HDR Desktop Duplication consumer open failed");

    auto options = source_options;
    options.include_cursor = false;
    options.include_cursor_metadata = false;
    options.pixel_format = gpu::WgcPixelFormat::rgba16_float;
    options.damage_mode = gpu::WgcDamageMode::native_report_only;
    options.capture_epoch = 37;
    options.capture_epoch_nonce = 0xe1b9'63a5ull;
    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::centered_region;
    mailbox.width = 320;
    mailbox.height = 320;
    gpu::DesktopDuplicationCapture capture;
    const auto created =
        gpu::DesktopDuplicationCapture::create_for_monitor_to_bus(
            MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST),
            publisher,
            options,
            mailbox,
            capture);
    if (!created && (created.status == gpu::WgcStatus::not_supported
            || created.hresult == DXGI_ERROR_UNSUPPORTED
            || created.hresult == E_NOINTERFACE)) {
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        return;
    }
    if (!created) {
        fail("HDR Desktop Duplication create failed: " + created.message);
    }
    const auto started = capture.start();
    if (!started) {
        fail("HDR Desktop Duplication start failed: " + started.message);
    }
    set_pattern_phase(target, 1);
    (void)DwmFlush();
    gpu::SharedFrameBusFrameLease frame;
    bus_result = consumer.acquire_latest(5'000, frame);
    if (!bus_result) {
        fail("HDR Desktop Duplication produced no P010 frame: "
            + capture.last_error().message);
    }
    const auto metadata = frame.metadata();
    const auto side_data = frame.side_data();
    if (frame.info().format != DXGI_FORMAT_P010
        || frame.info().width != bus_config.width
        || frame.info().height != bus_config.height
        || metadata.color_space
            != DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
        || metadata.roi_width != mailbox.width
        || metadata.roi_height != mailbox.height
        || side_data.epoch != options.capture_epoch
        || side_data.epoch_nonce != options.capture_epoch_nonce
        || (side_data.damage.flags & gpu::wgc_damage_native) == 0
        || (side_data.damage.flags & gpu::wgc_damage_full_frame) == 0
        || side_data.damage.dirty_count != 1
        || side_data.damage.dirty_rects[0].width != bus_config.width
        || side_data.damage.dirty_rects[0].height != bus_config.height
        || (side_data.damage.flags
            & gpu::wgc_damage_native_move_available) == 0) {
        fail("HDR Desktop Duplication lost its FP16/P010 color or damage contract");
    }
    bus_result = consumer.release(frame);
    if (!bus_result) fail("HDR Desktop Duplication frame release failed");
    capture.stop();
    const auto capture_stats = capture.stats();
    const auto bus_stats = publisher.stats();
    if (capture_stats.published_frames == 0
        || capture_stats.ingress_copy_submissions != 0
        || capture_stats.ingress_transform_submissions
            != capture_stats.published_frames
        || bus_stats.direct_publishes != capture_stats.published_frames
        || bus_stats.copied_publishes != 0) {
        fail("HDR Desktop Duplication performed a redundant pixel copy");
    }
    bus_result = consumer.close();
    if (!bus_result) fail("HDR Desktop Duplication consumer close failed");
    bus_result = publisher.unregister_consumer(registration, 5'000);
    if (!bus_result) fail("HDR Desktop Duplication left a slot pinned");
}

// Forces a real display-mode change (alternate refresh rate, same
// resolution) to prove the worker-side DXGI_ERROR_ACCESS_LOST rebuild:
// the capture must re-duplicate, keep publishing, and count the rebuild.
// Gated behind FLUXCAP_DD_ALLOW_MODE_CHANGE=1 because it flashes the
// physical display; the original mode is restored by RAII even on failure.
void test_desktop_duplication_access_lost_recovery(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    wchar_t allow[8]{};
    const bool allowed =
        GetEnvironmentVariableW(
            L"FLUXCAP_DD_ALLOW_MODE_CHANGE",
            allow,
            static_cast<DWORD>(std::size(allow))) != 0
        && allow[0] != L'0';
    if (!allowed) {
        std::cout << "[SKIP] Desktop Duplication ACCESS_LOST recovery: set "
                     "FLUXCAP_DD_ALLOW_MODE_CHANGE=1 to allow a temporary "
                     "display refresh-rate change\n";
        return;
    }
    const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (!GetMonitorInfoW(monitor, &monitor_info)) {
        fail("recovery target monitor query failed");
    }
    const std::uint32_t width = static_cast<std::uint32_t>(
        monitor_info.rcMonitor.right - monitor_info.rcMonitor.left);
    const std::uint32_t height = static_cast<std::uint32_t>(
        monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top);

    // Find an alternate refresh rate at the same resolution.
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(
            nullptr, ENUM_CURRENT_SETTINGS, &current)) {
        fail("EnumDisplaySettingsW(current) failed");
    }
    DEVMODEW alternate{};
    bool alternate_found = false;
    for (DWORD mode_index = 0;
         EnumDisplaySettingsW(nullptr, mode_index, &alternate);
         ++mode_index) {
        alternate.dmSize = sizeof(alternate);
        if (static_cast<std::uint32_t>(alternate.dmPelsWidth) == width
            && static_cast<std::uint32_t>(alternate.dmPelsHeight) == height
            && alternate.dmBitsPerPel == current.dmBitsPerPel
            && alternate.dmDisplayFrequency
                != current.dmDisplayFrequency
            && alternate.dmDisplayFrequency >= 30) {
            alternate_found = true;
            break;
        }
    }
    if (!alternate_found) {
        std::cout << "[SKIP] Desktop Duplication ACCESS_LOST recovery: no "
                     "alternate refresh rate at the current resolution\n";
        return;
    }

    struct ModeRestore final {
        DEVMODEW original;
        ~ModeRestore() {
            (void)ChangeDisplaySettingsExW(
                nullptr,
                &original,
                nullptr,
                CDS_FULLSCREEN,
                nullptr);
        }
    } mode_restore{current};

    gpu::SharedFrameBusConfig bus_config;
    bus_config.width = width;
    bus_config.height = height;
    bus_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bus_config.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bus_config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
    bus_config.slot_count = 3;
    gpu::SharedFrameBusPublisher publisher;
    auto bus_created = gpu::SharedFrameBusPublisher::create(
        device, bus_config, publisher);
    if (!bus_created) {
        fail(std::string("recovery bus create failed: ") + bus_created.what());
    }
    gpu::SharedFrameBusRegistration registration;
    auto bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("recovery bus registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("recovery bus consumer open failed");

    gpu::WgcCaptureOptions options = source_options;
    options.include_cursor = false;
    options.include_cursor_metadata = false;
    options.damage_mode = gpu::WgcDamageMode::native_report_only;
    options.pixel_format = gpu::WgcPixelFormat::bgra8;
    options.capture_epoch = 47;
    options.capture_epoch_nonce = 0xacce'5511ull;
    options.frame_timeout_ms = 50;
    options.access_lost_retry_limit = 0;
    options.access_lost_retry_initial_ms = 25;
    options.access_lost_retry_max_ms = 500;
    gpu::WgcMailboxConfig mailbox; // full frame survives the mode change
    gpu::DesktopDuplicationCapture capture;
    const auto created = gpu::DesktopDuplicationCapture::create_for_monitor_to_bus(
        monitor, publisher, options, mailbox, capture);
    if (!created && created.status == gpu::WgcStatus::not_supported) {
        std::cout << "[SKIP] Desktop Duplication ACCESS_LOST recovery: "
                  << created.message << "\n";
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        return;
    }
    if (!created) {
        fail("recovery capture create failed: " + created.message);
    }
    const auto started = capture.start();
    if (!started) {
        fail("recovery capture start failed: " + started.message);
    }
    if (!wait_until(10'000, [&] {
            return capture.stats().published_frames != 0;
        })) {
        fail("recovery capture published no baseline frame");
    }
    const std::uint64_t baseline = capture.stats().published_frames;
    const auto pre_change_error = capture.last_error();

    const LONG applied = ChangeDisplaySettingsExW(
        nullptr,
        &alternate,
        nullptr,
        CDS_FULLSCREEN,
        nullptr);
    if (applied != DISP_CHANGE_SUCCESSFUL) {
        fail("temporary display mode change failed: "
            + std::to_string(applied));
    }
    Sleep(1'000);

    const bool rebuilt = wait_until(15'000, [&] {
        return capture.stats().session_rebuilds != 0;
    });
    const bool kept_publishing = wait_until(10'000, [&] {
        return capture.stats().published_frames > baseline;
    });
    (void)ChangeDisplaySettingsExW(
        nullptr,
        &current,
        nullptr,
        CDS_FULLSCREEN,
        nullptr);
    Sleep(1'000);
    const auto stats = capture.stats();
    const bool still_running = capture.running();
    const auto post_error = capture.last_error();
    capture.stop();
    (void)consumer.close();
    bus_result = publisher.unregister_consumer(registration, 5'000);
    if (!bus_result) fail("recovery left a slot pinned");

    std::cout << "[PASS] Desktop Duplication ACCESS_LOST recovery executed"
              << " (baseline=" << baseline
              << ", published=" << stats.published_frames
              << ", rebuilds=" << stats.session_rebuilds
              << ", recovery=" << stats.recovery_attempts << '/'
              << stats.recovery_successes
              << ", running=" << still_running << ")\n";
    if (!rebuilt) {
        fail("display mode change produced no ACCESS_LOST rebuild "
             "(rebuilds=0); the mechanism was not exercised on this system");
    }
    if (!kept_publishing || !still_running
        || stats.published_frames <= baseline) {
        fail("capture did not resume publishing after the rebuild: "
             + post_error.message);
    }
    if (stats.recovery_successes == 0) {
        fail("rebuild succeeded without counting a recovery success");
    }
}

// Aggregates every output of the bus adapter into one virtual-desktop
// texture and pixel-verifies the pattern window inside it, then exercises
// the idle heartbeat republish path.
bool test_desktop_duplication_controller_bus(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    RECT client_rect{};
    POINT client_origin{};
    if (!GetMonitorInfoW(monitor, &monitor_info)
        || !GetClientRect(target, &client_rect)
        || !ClientToScreen(target, &client_origin)) {
        fail("controller target/monitor geometry query failed");
    }
    if (!SetWindowPos(
            target,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        fail("controller test target could not be made visible");
    }
    (void)DwmFlush();

    // Replicate the controller's virtual-desktop enumeration for the bus
    // device adapter so pixel expectations can address bus coordinates.
    RECT virtual_bounds{0, 0, 0, 0};
    bool bounds_known = false;
    bool rotated_output = false;
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> device_adapter;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))
        && SUCCEEDED(dxgi_device->GetAdapter(&device_adapter))) {
        DXGI_ADAPTER_DESC adapter_description{};
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(device_adapter->GetDesc(&adapter_description))
            && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT adapter_index = 0;; ++adapter_index) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(adapter_index, &adapter)
                    == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                DXGI_ADAPTER_DESC candidate{};
                if (FAILED(adapter->GetDesc(&candidate))
                    || candidate.AdapterLuid.LowPart
                        != adapter_description.AdapterLuid.LowPart
                    || candidate.AdapterLuid.HighPart
                        != adapter_description.AdapterLuid.HighPart) {
                    continue;
                }
                for (UINT output_index = 0;; ++output_index) {
                    ComPtr<IDXGIOutput> output;
                    if (adapter->EnumOutputs(output_index, &output)
                        == DXGI_ERROR_NOT_FOUND) {
                        break;
                    }
                    DXGI_OUTPUT_DESC description{};
                    if (FAILED(output->GetDesc(&description))
                        || !description.AttachedToDesktop) {
                        continue;
                    }
                    if (description.Rotation != DXGI_MODE_ROTATION_IDENTITY
                        && description.Rotation
                            != DXGI_MODE_ROTATION_UNSPECIFIED) {
                        rotated_output = true;
                    }
                    if (!bounds_known) {
                        virtual_bounds = description.DesktopCoordinates;
                        bounds_known = true;
                    } else {
                        virtual_bounds.left = std::min(
                            virtual_bounds.left,
                            description.DesktopCoordinates.left);
                        virtual_bounds.top = std::min(
                            virtual_bounds.top,
                            description.DesktopCoordinates.top);
                        virtual_bounds.right = std::max(
                            virtual_bounds.right,
                            description.DesktopCoordinates.right);
                        virtual_bounds.bottom = std::max(
                            virtual_bounds.bottom,
                            description.DesktopCoordinates.bottom);
                    }
                }
            }
        }
    }
    if (!bounds_known) {
        fail("controller could not enumerate a virtual desktop for the test");
    }
    const std::uint32_t virtual_width = static_cast<std::uint32_t>(
        virtual_bounds.right - virtual_bounds.left);
    const std::uint32_t virtual_height = static_cast<std::uint32_t>(
        virtual_bounds.bottom - virtual_bounds.top);

    gpu::SharedFrameBusConfig bus_config;
    bus_config.width = virtual_width;
    bus_config.height = virtual_height;
    bus_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bus_config.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bus_config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
    bus_config.slot_count = 3;
    gpu::SharedFrameBusPublisher publisher;
    auto bus_created = gpu::SharedFrameBusPublisher::create(
        device, bus_config, publisher);
    if (!bus_created) {
        fail(std::string("controller bus create failed: ") + bus_created.what());
    }
    gpu::SharedFrameBusRegistration registration;
    auto bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("controller bus registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("controller bus consumer open failed");

    gpu::WgcCaptureOptions options = source_options;
    options.include_cursor = false;
    options.include_cursor_metadata = false;
    options.damage_mode = gpu::WgcDamageMode::native_report_only;
    options.pixel_format = gpu::WgcPixelFormat::bgra8;
    options.capture_epoch = 41;
    options.capture_epoch_nonce = 0xc077'4011ull;
    options.idle_republish_interval_ms = 60;
    options.frame_timeout_ms = 20;
    gpu::DesktopDuplicationController controller;
    const auto created = gpu::DesktopDuplicationController::create_for_bus(
        publisher, options, controller);
    if (!created
        && (created.status == gpu::WgcStatus::not_supported
            || created.status == gpu::WgcStatus::invalid_argument
            || rotated_output)) {
        std::cout << "[SKIP] Desktop Duplication controller: "
                  << created.message << "\n";
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        (void)SetWindowPos(
            target, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return false;
    }
    if (!created) {
        fail("controller create failed: " + created.message);
    }
    if (controller.monitor_count() == 0) {
        fail("controller aggregated no monitors");
    }
    const auto started = controller.start();
    if (!started) {
        fail("controller start failed: " + started.message);
    }
    set_pattern_phase(target, 0);
    if (!wait_until(10'000, [&] {
            return controller.stats().published_frames != 0;
        })) {
        fail("controller published no frame: "
            + controller.last_error().message);
    }

    // The acquired frame may predate the pattern switch on an active
    // desktop; verify pixels with bounded retries against fresh frames.
    bool pixels_verified = false;
    for (int attempt = 0; attempt < 5 && !pixels_verified; ++attempt) {
    gpu::SharedFrameBusFrameLease frame;
    bus_result = consumer.acquire_latest(5'000, frame);
    if (!bus_result) fail("controller frame timed out");
    const auto metadata = frame.metadata();
    const auto side_data = frame.side_data();
    const auto& damage = side_data.damage;
    // On an active desktop the acquired frame may already be past the first
    // full-frame publish; native dirty rectangles are equally valid.
    if (frame.info().width != virtual_width
        || frame.info().height != virtual_height
        || frame.info().format != DXGI_FORMAT_B8G8R8A8_UNORM
        || metadata.source_width != virtual_width
        || metadata.source_height != virtual_height
        || metadata.color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
        || side_data.epoch != options.capture_epoch
        || side_data.epoch_nonce != options.capture_epoch_nonce
        || (damage.flags & gpu::wgc_damage_valid) == 0
        || ((damage.flags & gpu::wgc_damage_full_frame) == 0
            && (damage.flags & gpu::wgc_damage_native) == 0)) {
        fail("controller frame provenance is inconsistent");
    }
    for (std::uint32_t index = 0; index < damage.dirty_count; ++index) {
        const auto& dirty = damage.dirty_rects[index];
        if (dirty.x < 0 || dirty.y < 0
            || static_cast<std::uint64_t>(dirty.x) + dirty.width
                > virtual_width
            || static_cast<std::uint64_t>(dirty.y) + dirty.height
                > virtual_height) {
            fail("controller dirty rectangle escaped the virtual desktop");
        }
    }

    // Pixel-verify the pattern window region of the composed virtual
    // desktop with a stride to bound runtime on large desktops.
    D3D11_TEXTURE2D_DESC staging_description{};
    staging_description.Width = virtual_width;
    staging_description.Height = virtual_height;
    staging_description.MipLevels = 1;
    staging_description.ArraySize = 1;
    staging_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_description.SampleDesc.Count = 1;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(
        &staging_description, nullptr, &staging);
    if (FAILED(hr)) {
        fail("controller staging texture creation failed: "
            + std::to_string(hr));
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopyResource(staging.Get(), frame.texture());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) fail("controller staging map failed");
    const LONG window_left =
        client_origin.x - virtual_bounds.left;
    const LONG window_top =
        client_origin.y - virtual_bounds.top;
    const LONG window_width =
        client_rect.right - client_rect.left;
    const LONG window_height =
        client_rect.bottom - client_rect.top;
    bool mismatch_found = false;
    std::uint32_t mismatch_x = 0;
    std::uint32_t mismatch_y = 0;
    std::uint32_t mismatch_actual = 0;
    std::uint32_t mismatch_expected = 0;
    for (LONG y = 0; y < window_height && !mismatch_found; y += 3) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(mapped.pData)
                + static_cast<std::size_t>(window_top + y)
                    * mapped.RowPitch);
        for (LONG x = 0; x < window_width; x += 3) {
            const std::uint32_t expected =
                coordinate_pixel_phase(x, y, 0);
            if (row[window_left + x] != expected) {
                mismatch_found = true;
                mismatch_x = x;
                mismatch_y = y;
                mismatch_actual = row[window_left + x];
                mismatch_expected = expected;
                break;
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    bus_result = consumer.release(frame);
    if (!bus_result) fail("controller frame release failed");
    if (!mismatch_found) {
        pixels_verified = true;
        break;
    }
    if (attempt == 4) {
        fail("controller virtual-desktop pixel mismatch at window ("
            + std::to_string(mismatch_x) + ',' + std::to_string(mismatch_y)
            + "): expected=" + std::to_string(mismatch_expected)
            + ", actual=" + std::to_string(mismatch_actual));
    }
    set_pattern_phase(target, 0);
    (void)DwmFlush();
    }
    if (!pixels_verified) {
        fail("controller pixel verification did not complete");
    }

    // Keep the desktop static and wait for a heartbeat republish: the
    // sidecar must report valid-but-empty damage with no dirty rectangles.
    // Hosted/interactive desktops can present continuously (cursor or DWM
    // activity at high refresh rates); in that case the heartbeat
    // precondition never holds and the check degrades to an environmental
    // skip rather than a failure.
    const std::uint64_t idle_before =
        controller.stats().idle_republished_frames;
    const std::uint64_t received_before =
        controller.stats().received_frames;
    const bool heartbeat_observed = wait_until(5'000, [&] {
        return controller.stats().idle_republished_frames > idle_before;
    });
    if (heartbeat_observed) {
        gpu::SharedFrameBusFrameLease heartbeat_frame;
        bus_result = consumer.acquire_latest(5'000, heartbeat_frame);
        if (!bus_result) fail("controller heartbeat frame timed out");
        const auto heartbeat_damage = heartbeat_frame.side_data().damage;
        if ((heartbeat_damage.flags & gpu::wgc_damage_valid) == 0
            || (heartbeat_damage.flags & gpu::wgc_damage_full_frame) != 0
            || heartbeat_damage.dirty_count != 0
            || heartbeat_damage.move_count != 0) {
            fail("controller heartbeat side data is not a valid empty republish");
        }
        bus_result = consumer.release(heartbeat_frame);
        if (!bus_result) fail("controller heartbeat release failed");
    } else {
        const auto live = controller.stats();
        const std::uint64_t received_growth =
            live.received_frames - received_before;
        // If no heartbeat fired, every inter-frame gap stayed below the
        // idle interval, which implies a sustained >= 1/60ms frame rate.
        // Anything above a small floor therefore means "the desktop stayed
        // active", not "the worker is stuck".
        const bool desktop_stayed_active =
            received_growth >= 40 && controller.running();
        if (!desktop_stayed_active) {
            fail("controller heartbeat never republished and the worker "
                "stopped receiving frames (growth="
                + std::to_string(received_growth) + ", error="
                + controller.last_error().message + ")");
        }
        std::cout << "[SKIP] controller heartbeat verification: the desktop "
                     "stayed active for the whole window (received="
                  << live.received_frames << ")\n";
    }

    controller.stop();
    const auto stats = controller.stats();
    if (stats.published_frames == 0
        || stats.received_frames == 0
        || stats.full_damage_frames == 0
        || (heartbeat_observed && stats.idle_republished_frames == 0)) {
        fail("controller statistics are inconsistent");
    }
    bus_result = consumer.close();
    if (!bus_result) fail("controller consumer close failed");
    bus_result = publisher.unregister_consumer(registration, 5'000);
    if (!bus_result) fail("controller left a slot pinned");
    (void)SetWindowPos(
        target, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return true;
}

// CPU GDI fallback publishing the same absolute mailbox contract as the
// Desktop Duplication path, verified pixel-exactly against the pattern.
void test_gdi_monitor_capture_bus(
    HWND target,
    ID3D11Device* device,
    const gpu::WgcCaptureOptions& source_options) {
    const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    RECT client_rect{};
    POINT client_origin{};
    if (!GetMonitorInfoW(monitor, &monitor_info)
        || !GetClientRect(target, &client_rect)
        || !ClientToScreen(target, &client_origin)) {
        fail("GDI target/monitor geometry query failed");
    }
    if (!SetWindowPos(
            target,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        fail("GDI test target could not be made visible");
    }
    (void)DwmFlush();

    gpu::SharedFrameBusConfig bus_config = direct_bus_config(320);
    bus_config.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    gpu::SharedFrameBusPublisher publisher;
    auto bus_created = gpu::SharedFrameBusPublisher::create(
        device, bus_config, publisher);
    if (!bus_created) {
        fail(std::string("GDI bus create failed: ") + bus_created.what());
    }
    gpu::SharedFrameBusRegistration registration;
    auto bus_result = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!bus_result) fail("GDI bus registration failed");
    gpu::SharedFrameBusConsumer consumer;
    bus_result = gpu::SharedFrameBusConsumer::open(
        device, registration, true, consumer);
    if (!bus_result) fail("GDI bus consumer open failed");

    gpu::WgcCaptureOptions options = source_options;
    options.include_cursor = false;
    options.include_cursor_metadata = true;
    options.damage_mode = gpu::WgcDamageMode::disabled;
    options.pixel_format = gpu::WgcPixelFormat::bgra8;
    options.capture_epoch = 43;
    options.capture_epoch_nonce = 0x9d1'ca0eull;
    options.gdi_poll_interval_ms = 33;
    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::absolute_region;
    const std::uint32_t pattern_source_x = static_cast<std::uint32_t>(
        (client_rect.right - client_rect.left
            - static_cast<LONG>(bus_config.width)) / 2);
    const std::uint32_t pattern_source_y = static_cast<std::uint32_t>(
        (client_rect.bottom - client_rect.top
            - static_cast<LONG>(bus_config.height)) / 2);
    mailbox.x = static_cast<std::uint32_t>(
        client_origin.x - monitor_info.rcMonitor.left) + pattern_source_x;
    mailbox.y = static_cast<std::uint32_t>(
        client_origin.y - monitor_info.rcMonitor.top) + pattern_source_y;
    mailbox.width = bus_config.width;
    mailbox.height = bus_config.height;
    set_pattern_phase(target, 1);
    gpu::GdiMonitorCapture capture;
    const auto created = gpu::GdiMonitorCapture::create_for_monitor_to_bus(
        monitor, publisher, options, mailbox, capture);
    if (!created && created.status == gpu::WgcStatus::not_supported) {
        std::cout << "[SKIP] GDI monitor capture: " << created.message << "\n";
        (void)consumer.close();
        (void)publisher.unregister_consumer(registration, 3'000);
        (void)SetWindowPos(
            target, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    if (!created) {
        fail("GDI capture create failed: " + created.message);
    }
    const auto started = capture.start();
    if (!started) {
        fail("GDI capture start failed: " + started.message);
    }
    if (!wait_until(10'000, [&] {
            return capture.stats().published_frames >= 2;
        })) {
        fail("GDI capture published no frames: "
            + capture.last_error().message);
    }
    gpu::SharedFrameBusFrameLease frame;
    bus_result = consumer.acquire_latest(5'000, frame);
    if (!bus_result) fail("GDI frame timed out");
    const auto side_data = frame.side_data();
    const auto& damage = side_data.damage;
    if (frame.info().format != DXGI_FORMAT_B8G8R8A8_UNORM
        || frame.info().width != bus_config.width
        || frame.info().height != bus_config.height
        || side_data.epoch != options.capture_epoch
        || side_data.epoch_nonce != options.capture_epoch_nonce
        || (damage.flags & gpu::wgc_damage_valid) == 0
        || (damage.flags & gpu::wgc_damage_full_frame) == 0
        || damage.dirty_count != 1
        || damage.dirty_rects[0].x != 0
        || damage.dirty_rects[0].y != 0
        || damage.dirty_rects[0].width != bus_config.width
        || damage.dirty_rects[0].height != bus_config.height
        || (damage.flags & gpu::wgc_damage_native) != 0) {
        fail("GDI frame did not carry the full-frame non-native contract");
    }
    BusChildCommand expected;
    expected.width = bus_config.width;
    expected.height = bus_config.height;
    expected.source_x = pattern_source_x;
    expected.source_y = pattern_source_y;
    expected.phase = 1;
    validate_bus_roi(device, capture.context(), frame.texture(), expected);
    bus_result = consumer.release(frame);
    if (!bus_result) fail("GDI frame release failed");

    capture.stop();
    const auto stats = capture.stats();
    if (stats.published_frames < 2
        || stats.full_damage_frames != stats.published_frames
        || stats.cursor_metadata_frames != stats.published_frames
        || stats.ingress_copy_submissions != stats.published_frames) {
        fail("GDI statistics are inconsistent");
    }
    bus_result = consumer.close();
    if (!bus_result) fail("GDI consumer close failed");
    bus_result = publisher.unregister_consumer(registration, 5'000);
    if (!bus_result) fail("GDI left a slot pinned");
    (void)SetWindowPos(
        target, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--shared-consumer") {
        HANDLE input = parse_inherited_handle(argv[2]);
        HANDLE output = parse_inherited_handle(argv[3]);
        return input != nullptr && output != nullptr
            ? run_shared_consumer_child(input, output)
            : 2;
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--bus-consumer") {
        HANDLE input = parse_inherited_handle(argv[2]);
        HANDLE output = parse_inherited_handle(argv[3]);
        return input != nullptr && output != nullptr
            ? run_bus_consumer_child(input, output)
            : 2;
    }
    WindowThreadState windows;
    windows.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (windows.ready == nullptr) {
        std::cerr << "[FAIL] CreateEvent failed\n";
        return 1;
    }
    std::thread ui(window_thread, &windows);

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (WaitForSingleObject(windows.ready, 5000) != WAIT_OBJECT_0) {
            fail("window thread timed out");
        }
        if (windows.error.load(std::memory_order_acquire) != ERROR_SUCCESS) {
            fail("window thread failed: " + std::to_string(
                windows.error.load(std::memory_order_relaxed)));
        }
        HWND target = windows.target.load(std::memory_order_acquire);
        HWND roi_target = windows.roi_target.load(std::memory_order_acquire);
        if (target == nullptr || roi_target == nullptr) {
            fail("test target window is null");
        }
        (void)DwmFlush();
        std::this_thread::sleep_for(100ms);

        gpu::WgcCapture capture;
        gpu::WgcCaptureOptions options;
        options.buffer_count = 3;
        options.include_cursor = false;
        const auto created = gpu::WgcCapture::create_for_window(
            target,
            nullptr,
            options,
            capture);
        if (!created) {
            fail("WGC create failed: " + created.message);
        }
        const auto started = capture.start();
        if (!started) {
            fail("WGC start failed: " + started.message);
        }

        gpu::WgcFrameLease frame;
        const auto acquired = capture.acquire_latest(5000, frame);
        if (!acquired) {
            fail("WGC acquire failed: " + acquired.message);
        }
        test_gpu_encoder_invalid_color_contract(capture.device());
        test_desktop_duplication_side_data_helpers();
        test_deterministic_planar_transform(
            capture.device(), capture.context());
        if (frame.texture() == nullptr || frame.info().width < 100 || frame.info().height < 100) {
            fail("WGC returned an invalid GPU frame");
        }
        if (frame.info().source_timestamp_100ns <= 0) {
            fail("WGC returned no source presentation timestamp");
        }
        const auto& first_damage = frame.damage();
        if ((first_damage.flags & gpu::wgc_damage_valid) == 0
            || (first_damage.flags & gpu::wgc_damage_full_frame) == 0
            || (first_damage.flags & gpu::wgc_damage_discontinuity) == 0
            || (first_damage.flags
                & gpu::wgc_damage_native_move_unavailable) == 0
            || first_damage.base_sequence != 0
            || first_damage.dirty_count != 1
            || first_damage.move_count != 0
            || first_damage.dirty_rects[0].x != 0
            || first_damage.dirty_rects[0].y != 0
            || first_damage.dirty_rects[0].width != frame.info().width
            || first_damage.dirty_rects[0].height != frame.info().height) {
            fail("first WGC frame did not expose conservative full damage");
        }

        const std::uint32_t pixel = read_center_pixel(
            capture.device(),
            capture.context(),
            frame.texture());
        const std::uint8_t blue = static_cast<std::uint8_t>(pixel & 0xffu);
        const std::uint8_t red = static_cast<std::uint8_t>((pixel >> 16) & 0xffu);
        if (red <= blue + 80) {
            fail("occluded target pixel was not red; WGC may have captured the cover");
        }

        if (!PostMessageW(target, kPulseMessage, 0, 0)) {
            fail("failed to update WGC native-damage target");
        }
        gpu::WgcFrameLease damage_frame;
        const auto damage_acquired = capture.acquire_latest(5'000, damage_frame);
        if (!damage_acquired) {
            fail("WGC native-damage acquire failed: " + damage_acquired.message);
        }
        const auto& damage = damage_frame.damage();
        if ((damage.flags & gpu::wgc_damage_valid) == 0
            || (damage.flags & gpu::wgc_damage_native_move_unavailable) == 0
            || damage.move_count != 0
            || damage.dirty_count > gpu::wgc_max_dirty_rects) {
            fail("WGC returned malformed native-damage metadata");
        }
        for (std::uint32_t index = 0; index < damage.dirty_count; ++index) {
            const auto& rect = damage.dirty_rects[index];
            if (rect.x < 0 || rect.y < 0
                || rect.width == 0 || rect.height == 0
                || static_cast<std::uint64_t>(rect.x) + rect.width
                    > damage_frame.info().width
                || static_cast<std::uint64_t>(rect.y) + rect.height
                    > damage_frame.info().height) {
                fail("WGC native-damage rectangle is outside the frame");
            }
        }
        damage_frame.reset();

        gpu::WgcMailboxConfig region_config;
        region_config.mode = gpu::WgcMailboxMode::centered_region;
        region_config.width = 320;
        region_config.height = 320;
        gpu::WgcCapture region_capture;
        const auto region_created = gpu::WgcCapture::create_for_window(
            roi_target,
            capture.device(),
            options,
            region_config,
            region_capture);
        if (!region_created) {
            fail("fused ROI WGC create failed: " + region_created.message);
        }
        const auto region_started = region_capture.start();
        if (!region_started) {
            fail("fused ROI WGC start failed: " + region_started.message);
        }

        gpu::WgcFrameLease region_frame;
        const auto region_acquired = region_capture.acquire_latest(5'000, region_frame);
        if (!region_acquired) {
            fail("fused ROI WGC acquire failed: " + region_acquired.message);
        }
        D3D11_TEXTURE2D_DESC region_description{};
        region_frame.texture()->GetDesc(&region_description);
        const auto first_region_info = region_frame.mailbox_info();
        if (region_description.Width != 320 || region_description.Height != 320
            || region_frame.info().width != 320 || region_frame.info().height != 320
            || first_region_info.width != 320 || first_region_info.height != 320
            || first_region_info.source_width != kPatternWidth
            || first_region_info.source_height != kPatternHeight
            || first_region_info.x != 240
            || first_region_info.y != 239
            || first_region_info.generation == 0) {
            fail("fused ROI mailbox returned inconsistent dimensions or metadata: texture="
                + std::to_string(region_description.Width) + "x"
                + std::to_string(region_description.Height) + ", frame="
                + std::to_string(region_frame.info().width) + "x"
                + std::to_string(region_frame.info().height) + ", source="
                + std::to_string(first_region_info.source_width) + "x"
                + std::to_string(first_region_info.source_height) + ", origin=("
                + std::to_string(first_region_info.x) + ","
                + std::to_string(first_region_info.y) + "), generation="
                + std::to_string(first_region_info.generation));
        }
        const auto verify_region_pixel = [&](gpu::WgcFrameLease& lease,
                                             std::uint32_t output_x,
                                             std::uint32_t output_y,
                                             std::uint32_t source_x,
                                             std::uint32_t source_y,
                                             std::uint32_t phase,
                                             const char* label) {
            const std::uint32_t actual = read_bgra_pixel(
                region_capture.device(),
                region_capture.context(),
                lease.texture(),
                output_x,
                output_y);
            const std::uint32_t expected = coordinate_pixel_phase(
                source_x, source_y, phase);
            if (actual != expected) {
                fail(std::string(label) + " pixel mismatch: expected="
                    + std::to_string(expected) + ", actual="
                    + std::to_string(actual));
            }
        };
        verify_region_pixel(
            region_frame, 0, 0, first_region_info.x, first_region_info.y, 0,
            "center ROI top-left");
        verify_region_pixel(
            region_frame,
            319,
            319,
            first_region_info.x + 319,
            first_region_info.y + 319,
            0,
            "center ROI bottom-right");
        verify_region_pixel(
            region_frame,
            160,
            160,
            first_region_info.x + 160,
            first_region_info.y + 160,
            0,
            "center ROI midpoint");

        const auto first_region_state = region_capture.mailbox_state();
        if (!first_region_state.region_available
            || first_region_state.generation != first_region_info.generation) {
            fail("fused ROI mailbox state did not match its first frame");
        }
        auto invalid_region = region_config;
        invalid_region.x = 1;
        const auto invalid_region_result = region_capture.set_mailbox_config(
            invalid_region);
        if (invalid_region_result
            || invalid_region_result.status != gpu::WgcStatus::invalid_argument
            || region_capture.mailbox_state().generation
                != first_region_state.generation) {
            fail("invalid ROI mailbox configuration changed capture state");
        }

        region_config.mode = gpu::WgcMailboxMode::absolute_region;
        region_config.x = 17;
        region_config.y = 23;
        region_config.width = 640;
        region_config.height = 640;
        const auto region_reconfigured = region_capture.set_mailbox_config(
            region_config);
        if (!region_reconfigured) {
            fail("fused ROI mailbox reconfiguration failed: "
                + region_reconfigured.message);
        }
        const auto second_region_state = region_capture.mailbox_state();
        if (!second_region_state.region_available
            || second_region_state.generation != first_region_info.generation + 1
            || region_frame.mailbox_info().generation != first_region_info.generation) {
            fail("ROI mailbox generation switch invalidated an active old lease");
        }
        if (!PostMessageW(roi_target, kPulseMessage, 0, 0)) {
            fail("failed to update fused ROI test target");
        }
        gpu::WgcFrameLease second_region_frame;
        const auto second_region_acquired = region_capture.acquire_latest(
            5'000, second_region_frame);
        if (!second_region_acquired) {
            fail("reconfigured fused ROI acquire failed: "
                + second_region_acquired.message);
        }
        const auto second_region_info = second_region_frame.mailbox_info();
        second_region_frame.texture()->GetDesc(&region_description);
        if (region_description.Width != 640 || region_description.Height != 640
            || second_region_info.width != 640 || second_region_info.height != 640
            || second_region_info.x != region_config.x
            || second_region_info.y != region_config.y
            || second_region_info.generation != second_region_state.generation) {
            fail("reconfigured fused ROI returned a stale mailbox frame");
        }
        verify_region_pixel(
            second_region_frame,
            0,
            0,
            region_config.x,
            region_config.y,
            1,
            "absolute ROI top-left");
        verify_region_pixel(
            second_region_frame,
            639,
            639,
            region_config.x + 639,
            region_config.y + 639,
            1,
            "absolute ROI bottom-right");
        verify_region_pixel(
            region_frame,
            160,
            160,
            first_region_info.x + 160,
            first_region_info.y + 160,
            0,
            "old generation retained content");

        if (!PostMessageW(roi_target, kResizePatternSmallMessage, 0, 0)) {
            fail("failed to shrink fused ROI test target");
        }
        const auto unavailable_deadline = std::chrono::steady_clock::now() + 3s;
        gpu::WgcMailboxState unavailable_state;
        do {
            unavailable_state = region_capture.mailbox_state();
            if (!unavailable_state.region_available
                && unavailable_state.source_width == 639
                && unavailable_state.source_height == 639) {
                break;
            }
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < unavailable_deadline);
        if (unavailable_state.region_available
            || unavailable_state.source_width != 639
            || unavailable_state.source_height != 639) {
            fail("source shrink did not make the fixed ROI unavailable");
        }
        const std::uint64_t shrink_size_changes = region_capture.stats().size_changes;
        gpu::WgcFrameLease unavailable_frame;
        const auto unavailable_acquire = region_capture.acquire_latest(
            0, unavailable_frame);
        if (unavailable_acquire
            || unavailable_acquire.status != gpu::WgcStatus::region_unavailable) {
            fail("source shrink did not return region_unavailable");
        }

        if (!PostMessageW(roi_target, kResizePatternRestoreMessage, 0, 0)) {
            fail("failed to restore fused ROI test target");
        }
        const auto restored_deadline = std::chrono::steady_clock::now() + 3s;
        gpu::WgcMailboxState restored_state;
        do {
            restored_state = region_capture.mailbox_state();
            if (restored_state.region_available
                && restored_state.source_width == kPatternWidth
                && restored_state.source_height == kPatternHeight
                && region_capture.stats().size_changes > shrink_size_changes) {
                break;
            }
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < restored_deadline);
        if (!restored_state.region_available
            || restored_state.source_width != kPatternWidth
            || restored_state.source_height != kPatternHeight
            || restored_state.generation != second_region_state.generation
            || region_capture.stats().size_changes <= shrink_size_changes) {
            fail("fixed ROI mailbox state did not recover with its source size");
        }
        if (!RedrawWindow(
                roi_target,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW)) {
            fail("failed to publish a frame after restoring the ROI source");
        }
        gpu::WgcFrameLease restored_region_frame;
        const auto restored_acquire = region_capture.acquire_latest(
            5'000, restored_region_frame);
        if (!restored_acquire
            || restored_region_frame.mailbox_info().generation
                != second_region_state.generation
            || restored_region_frame.info().width != 640
            || restored_region_frame.info().height != 640) {
            fail("fixed ROI did not recover after its source size was restored");
        }
        const auto restored_info = restored_region_frame.mailbox_info();
        verify_region_pixel(
            restored_region_frame,
            0,
            0,
            restored_info.x,
            restored_info.y,
            1,
            "restored absolute ROI");
        restored_region_frame.reset();
        second_region_frame.reset();
        region_frame.reset();
        region_capture.stop();
        const auto stopped_reconfigure = region_capture.set_mailbox_config(
            region_config);
        if (stopped_reconfigure
            || stopped_reconfigure.status != gpu::WgcStatus::invalid_state) {
            fail("stopped WGC capture accepted ROI mailbox reconfiguration");
        }

        POINT original_cursor{};
        const BOOL had_cursor_position = GetCursorPos(&original_cursor);
        RECT cursor_bounds{};
        if (!GetWindowRect(roi_target, &cursor_bounds)
            || !SetCursorPos(cursor_bounds.left + 80, cursor_bounds.top + 90)) {
            fail("failed to position the independent cursor test");
        }
        gpu::WgcCaptureOptions cursor_options = options;
        cursor_options.include_cursor = true;
        cursor_options.include_cursor_metadata = true;
        cursor_options.cursor_shape_refresh_interval_ms = 0;
        gpu::WgcCapture cursor_capture;
        const auto cursor_created = gpu::WgcCapture::create_for_window(
            roi_target,
            capture.device(),
            cursor_options,
            cursor_capture);
        if (!cursor_capture_control_present()) {
            if (cursor_created
                || cursor_created.status != gpu::WgcStatus::not_supported
                || cursor_created.hresult != E_NOINTERFACE
                || cursor_capture.device() != nullptr) {
                fail("private independent cursor capture did not fail closed without cursor control");
            }
        } else {
            if (!cursor_created) {
                fail("independent cursor WGC capture failed: "
                    + cursor_created.message);
            }
            const auto cursor_started = cursor_capture.start();
            if (!cursor_started) {
                fail("independent cursor WGC start failed: "
                    + cursor_started.message);
            }
            if (!RedrawWindow(
                    roi_target,
                    nullptr,
                    nullptr,
                    RDW_INVALIDATE | RDW_UPDATENOW)) {
                fail("failed to refresh the independent cursor target");
            }
            gpu::WgcFrameLease cursor_frame;
            const auto cursor_acquired = cursor_capture.acquire_latest(
                5'000, cursor_frame);
            if (!cursor_acquired) {
                fail("independent cursor metadata acquire failed: "
                    + cursor_acquired.message);
            }
            const auto& cursor_info = cursor_frame.cursor_info();
            if ((cursor_info.flags & gpu::wgc_cursor_position_valid) == 0
                || (cursor_info.flags & gpu::wgc_cursor_position_estimated) == 0
                || cursor_info.sample_qpc == 0
                || cursor_info.shape_sequence == 0
                || cursor_info.width == 0 || cursor_info.height == 0) {
                fail("independent cursor metadata is incomplete");
            }
            gpu::WgcCursorShape cursor_shape;
            const auto shape_result = cursor_capture.cursor_shape(
                cursor_info.shape_sequence, cursor_shape);
            if (!shape_result
                || cursor_shape.sequence != cursor_info.shape_sequence
                || cursor_shape.kind == gpu::WgcCursorShapeKind::none
                || cursor_shape.width != cursor_info.width
                || cursor_shape.height != cursor_info.height
                || cursor_shape.stride_bytes == 0
                || cursor_shape.data.empty()) {
                fail("independent cursor shape cache is inconsistent");
            }
            const auto cursor_stats = cursor_capture.stats();
            if (cursor_stats.cursor_metadata_frames == 0
                || cursor_stats.cursor_shape_updates == 0) {
                fail("independent cursor statistics did not advance");
            }
            cursor_frame.reset();
            cursor_capture.stop();
        }
        if (had_cursor_position) {
            (void)SetCursorPos(original_cursor.x, original_cursor.y);
        }

        gpu::WgcCaptureOptions hdr_options = options;
        hdr_options.pixel_format = gpu::WgcPixelFormat::rgba16_float;
        hdr_options.damage_mode = gpu::WgcDamageMode::disabled;
        gpu::WgcCapture hdr_capture;
        const auto hdr_created = gpu::WgcCapture::create_for_window(
            roi_target,
            capture.device(),
            hdr_options,
            hdr_capture);
        if (!hdr_created) {
            fail("FP16/scRGB WGC create failed: " + hdr_created.message);
        }
        const auto hdr_started = hdr_capture.start();
        if (!hdr_started) {
            fail("FP16/scRGB WGC start failed: " + hdr_started.message);
        }
        if (!RedrawWindow(
                roi_target,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW)) {
            fail("failed to refresh the FP16/scRGB WGC target");
        }
        gpu::WgcFrameLease hdr_frame;
        const auto hdr_acquired = hdr_capture.acquire_latest(5'000, hdr_frame);
        if (!hdr_acquired) {
            fail("FP16/scRGB WGC acquire failed: " + hdr_acquired.message);
        }
        D3D11_TEXTURE2D_DESC hdr_description{};
        hdr_frame.texture()->GetDesc(&hdr_description);
        if (hdr_description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT
            || hdr_frame.info().format != DXGI_FORMAT_R16G16B16A16_FLOAT
            || hdr_frame.info().color_space
                != DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
            fail("FP16 WGC frame did not preserve the scRGB contract");
        }
        gpu::GpuTransformConfig hdr_transform_config;
        hdr_transform_config.input_width = hdr_frame.info().width;
        hdr_transform_config.input_height = hdr_frame.info().height;
        hdr_transform_config.output_width = hdr_frame.info().width & ~1u;
        hdr_transform_config.output_height = hdr_frame.info().height & ~1u;
        hdr_transform_config.output_format = gpu::GpuPixelFormat::p010;
        hdr_transform_config.input_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hdr_transform_config.input_color_space = hdr_frame.info().color_space;
        hdr_transform_config.output_color_space =
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        gpu::GpuTransform hdr_transform;
        const auto hdr_transform_created = gpu::GpuTransform::create(
            hdr_capture.device(), hdr_transform_config, hdr_transform);
        if (hdr_transform_created) {
            const auto hdr_transformed = hdr_transform.process(
                hdr_frame.texture());
            if (!hdr_transformed) {
                fail(std::string("scRGB to P010 transform failed: ")
                    + hdr_transformed.what());
            }
            D3D11_TEXTURE2D_DESC p010_description{};
            hdr_transform.output_texture()->GetDesc(&p010_description);
            if (p010_description.Format != DXGI_FORMAT_P010) {
                fail("scRGB transform did not produce P010");
            }
        } else if (hdr_transform_created.status != gpu::GpuStatus::unsupported) {
            fail(std::string("scRGB/P010 transform failed unexpectedly: ")
                + hdr_transform_created.what());
        }
        hdr_frame.reset();
        hdr_capture.stop();

        test_wgc_direct_bus_api_validation(
            roi_target, capture.device(), options);
        test_wgc_direct_bus_cursor_shape_publication(
            roi_target, capture.device(), options);
        test_wgc_direct_bus_async_move_inference(
            roi_target, capture.device(), options);
        test_wgc_direct_bus_reconfigure(
            roi_target, capture.device(), options);
        test_wgc_direct_bus_absolute_640(
            roi_target, capture.device(), options);
        test_wgc_direct_bus_full_frame_resize_guard(
            roi_target, capture.device(), options);
        test_recoverable_wgc_full_frame_resize(
            roi_target, options);
        test_wgc_direct_bus_no_slot_recovery(
            roi_target, capture.device(), options);
        test_wgc_retains_bus_after_publisher_wrapper_destruction(
            roi_target, capture.device(), options);
        test_wgc_planar_bus_external_encoder(
            roi_target, capture.device(), options);
        test_wgc_hdr_p010_bus(
            roi_target, capture.device(), options);
        const bool dd_native_metadata_executed =
            test_desktop_duplication_native_metadata(
            roi_target, capture.device(), options);
        test_desktop_duplication_hdr_p010(
            roi_target, capture.device(), options);
        test_recoverable_desktop_duplication_zero_copy(roi_target);
        test_desktop_duplication_controller_bus(
            roi_target, capture.device(), options);
        test_gdi_monitor_capture_bus(
            roi_target, capture.device(), options);
        test_desktop_duplication_access_lost_recovery(
            roi_target, capture.device(), options);

        if (!PostMessageW(target, kMoveOffscreenMessage, 0, 0)) {
            fail("failed to request offscreen target update");
        }
        const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const auto moved_deadline = std::chrono::steady_clock::now() + 2s;
        RECT moved_bounds{};
        do {
            GetWindowRect(target, &moved_bounds);
            if (moved_bounds.right < virtual_left) break;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < moved_deadline);
        if (moved_bounds.right >= virtual_left) {
            fail("target window did not move outside the virtual desktop");
        }

        gpu::WgcCapture offscreen_capture;
        const auto offscreen_created = gpu::WgcCapture::create_for_window(
            target, capture.device(), options, offscreen_capture);
        if (!offscreen_created) {
            fail("offscreen WGC create failed: " + offscreen_created.message);
        }
        const auto offscreen_started = offscreen_capture.start();
        if (!offscreen_started) {
            fail("offscreen WGC start failed: " + offscreen_started.message);
        }
        gpu::WgcFrameLease offscreen_frame;
        const auto offscreen_acquired = offscreen_capture.acquire_latest(5'000, offscreen_frame);
        if (!offscreen_acquired) {
            fail("offscreen WGC acquire failed: " + offscreen_acquired.message);
        }
        const std::uint32_t offscreen_pixel = read_center_pixel(
            offscreen_capture.device(), offscreen_capture.context(), offscreen_frame.texture());
        const std::uint8_t offscreen_blue = static_cast<std::uint8_t>(offscreen_pixel & 0xffu);
        const std::uint8_t offscreen_red = static_cast<std::uint8_t>((offscreen_pixel >> 16) & 0xffu);
        if (offscreen_red <= offscreen_blue + 80) {
            fail("offscreen WGC capture lost the target window surface");
        }
        offscreen_frame.reset();
        offscreen_capture.stop();

        constexpr std::uint32_t crop_input_width = 801;
        constexpr std::uint32_t crop_input_height = 799;
        auto coordinate_texture = create_coordinate_texture(
            capture.device(), crop_input_width, crop_input_height);
        verify_center_crop(
            capture.device(),
            capture.context(),
            coordinate_texture.Get(),
            crop_input_width,
            crop_input_height,
            320);
        verify_center_crop(
            capture.device(),
            capture.context(),
            coordinate_texture.Get(),
            crop_input_width,
            crop_input_height,
            640);

        gpu::GpuCropConfig invalid_crop;
        invalid_crop.input_width = crop_input_width;
        invalid_crop.input_height = crop_input_height;
        invalid_crop.x = 700;
        invalid_crop.y = 0;
        invalid_crop.width = 320;
        invalid_crop.height = 320;
        gpu::GpuCrop rejected_crop;
        const auto rejected_crop_result = gpu::GpuCrop::create(
            capture.device(), invalid_crop, rejected_crop);
        if (rejected_crop_result
            || rejected_crop_result.status != gpu::GpuStatus::invalid_argument) {
            fail("GPU crop accepted an out-of-bounds source region");
        }

        auto unsupported_crop = invalid_crop;
        unsupported_crop.x = 0;
        unsupported_crop.width = 320;
        unsupported_crop.format = DXGI_FORMAT_NV12;
        const auto unsupported_crop_result = gpu::GpuCrop::create(
            capture.device(), unsupported_crop, rejected_crop);
        if (unsupported_crop_result
            || unsupported_crop_result.status != gpu::GpuStatus::invalid_argument) {
            fail("GPU crop accepted a non-BGRA texture format");
        }

        gpu::GpuCropConfig alias_config;
        alias_config.input_width = 320;
        alias_config.input_height = 320;
        alias_config.width = 320;
        alias_config.height = 320;
        gpu::GpuCrop alias_crop;
        const auto alias_created = gpu::GpuCrop::create(
            capture.device(), alias_config, alias_crop);
        if (!alias_created) {
            fail(std::string("GPU alias crop create failed: ") + alias_created.what());
        }
        const auto alias_result = alias_crop.process(alias_crop.output_texture());
        if (alias_result
            || alias_result.status != gpu::GpuStatus::invalid_argument
            || alias_crop.generation() != 0) {
            fail("GPU crop accepted a self-aliasing texture copy");
        }

        fluxcap::gpu::GpuTransform scaled;
        fluxcap::gpu::GpuTransformConfig scale_config;
        scale_config.input_width = frame.info().width;
        scale_config.input_height = frame.info().height;
        scale_config.output_width = (frame.info().width / 2u) & ~1u;
        scale_config.output_height = (frame.info().height / 2u) & ~1u;
        scale_config.output_format = fluxcap::gpu::GpuPixelFormat::bgra8;
        const auto scale_created = fluxcap::gpu::GpuTransform::create(
            capture.device(),
            scale_config,
            scaled);
        if (!scale_created) {
            fail(std::string("GPU BGRA scaler create failed: ") + scale_created.what());
        }
        const auto scaled_result = scaled.process(frame.texture());
        if (!scaled_result) {
            fail(std::string("GPU BGRA scale failed: ") + scaled_result.what());
        }
        (void)require_transform_output(
            scaled,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            scale_config.output_width,
            scale_config.output_height,
            "BGRA");
        const std::uint32_t scaled_pixel = read_center_pixel(
            capture.device(),
            capture.context(),
            scaled.output_texture());
        const std::uint8_t scaled_blue = static_cast<std::uint8_t>(scaled_pixel & 0xffu);
        const std::uint8_t scaled_red = static_cast<std::uint8_t>((scaled_pixel >> 16) & 0xffu);
        if (scaled_red <= scaled_blue + 60) {
            fail("GPU-scaled BGRA texture lost the target color");
        }

        fluxcap::gpu::GpuTransform nv12;
        auto nv12_config = scale_config;
        nv12_config.output_format = fluxcap::gpu::GpuPixelFormat::nv12;
        const auto nv12_created = fluxcap::gpu::GpuTransform::create(
            capture.device(),
            nv12_config,
            nv12);
        if (!nv12_created) {
            fail(std::string("GPU NV12 transformer create failed: ") + nv12_created.what());
        }
        const auto nv12_result = nv12.process(frame.texture());
        if (!nv12_result) {
            fail(std::string("GPU BGRA-to-NV12 conversion failed: ") + nv12_result.what());
        }
        const D3D11_TEXTURE2D_DESC nv12_desc = require_transform_output(
            nv12,
            DXGI_FORMAT_NV12,
            nv12_config.output_width,
            nv12_config.output_height,
            "NV12");
        const std::uint16_t nv12_luma = read_center_luma(
            capture.device(), capture.context(), nv12.output_texture());
        if (nv12_luma < 35 || nv12_luma > 120) {
            fail("NV12 conversion returned an implausible luma value for red input");
        }
        D3D11_TEXTURE2D_DESC unbound_output_desc = nv12_desc;
        unbound_output_desc.BindFlags = 0;
        ComPtr<ID3D11Texture2D> unbound_output;
        HRESULT unbound_created = capture.device()->CreateTexture2D(
            &unbound_output_desc, nullptr, &unbound_output);
        if (FAILED(unbound_created)) {
            fail("unbound NV12 validation texture creation failed");
        }
        const std::uint64_t generation_before_rejection = nv12.generation();
        const auto unbound_result = nv12.process_into(
            frame.texture(), unbound_output.Get());
        if (unbound_result
            || unbound_result.status != gpu::GpuStatus::invalid_argument
            || nv12.generation() != generation_before_rejection) {
            fail("GPU transform accepted an external output without render-target bind");
        }

        fluxcap::gpu::GpuTransform p010;
        auto p010_config = scale_config;
        p010_config.output_format = fluxcap::gpu::GpuPixelFormat::p010;
        const auto p010_created = fluxcap::gpu::GpuTransform::create(
            capture.device(),
            p010_config,
            p010);
        if (!p010_created) {
            fail(std::string("GPU P010 transformer create failed: ") + p010_created.what());
        }
        const auto p010_result = p010.process(frame.texture());
        if (!p010_result) {
            fail(std::string("GPU BGRA-to-P010 conversion failed: ") + p010_result.what());
        }
        (void)require_transform_output(
            p010,
            DXGI_FORMAT_P010,
            p010_config.output_width,
            p010_config.output_height,
            "P010");
        const std::uint16_t p010_luma = read_center_luma(
            capture.device(), capture.context(), p010.output_texture());
        if (p010_luma < 140 || p010_luma > 480) {
            fail("P010 conversion returned an implausible luma value for red input");
        }
        if (p010.generation() != 1) {
            fail("GPU P010 transform did not record its completed process call");
        }

        gpu::GpuEncoderConfig encoder_config;
        encoder_config.codec = gpu::VideoCodec::h264;
        encoder_config.width = nv12_desc.Width;
        encoder_config.height = nv12_desc.Height;
        encoder_config.frame_rate_numerator = 60;
        encoder_config.frame_rate_denominator = 1;
        encoder_config.bitrate = 2'000'000;
        encoder_config.gop_size = 60;
        encoder_config.input_format = DXGI_FORMAT_NV12;
        encoder_config.low_latency = true;
        encoder_config.input_pool_size = 2;
        gpu::GpuEncoderSupport encoder_support;
        const auto probed = gpu::GpuEncoder::probe(
            capture.device(),
            encoder_config,
            encoder_support);
        if (!probed || !encoder_support.supported || !encoder_support.d3d11_aware) {
            fail("no D3D11-aware H.264 hardware encoder is available: " + probed.message);
        }
        if (!encoder_support.external_planar_input) {
            fail("D3D11-aware NV12 encoder did not advertise external planar input");
        }
        const auto expected_evidence_capability =
            encoder_support.video_encoder_input_bind_supported
            ? gpu::GpuCopyEvidenceLevel::
                l2_external_surface_identity_and_encoder_bind
            : gpu::GpuCopyEvidenceLevel::l1_no_fluxcap_explicit_copy;
        if (encoder_support.runtime_copy_evidence_capability
            != expected_evidence_capability) {
            fail("encoder probe overstated or understated runtime copy evidence");
        }
        PacketCollector packets;
        gpu::GpuEncoder video_encoder;
        const auto encoder_initialized = video_encoder.initialize(
            capture.device(),
            encoder_config,
            &collect_packet,
            &packets);
        if (!encoder_initialized) {
            fail("hardware H.264 encoder initialize failed: " + encoder_initialized.message
                + " (hr=" + std::to_string(encoder_initialized.hresult) + ")");
        }
        const gpu::GpuEncoderMftIdentity encoder_identity =
            video_encoder.mft_identity();
        if (IsEqualGUID(encoder_identity.clsid, GUID_NULL)
            || encoder_identity.friendly_name[0] == '\0') {
            fail("initialized hardware encoder did not expose its exact MFT identity");
        }
        constexpr std::int64_t frame_duration = 10'000'000 / 60;

        gpu::GpuEncoderInputLease canceled_input;
        gpu::GpuEncoderInputLease held_input;
        auto input_acquired = video_encoder.acquire_input(canceled_input);
        if (!input_acquired || !canceled_input || canceled_input.texture() == nullptr) {
            fail("direct encoder input acquisition failed: " + input_acquired.message);
        }
        input_acquired = video_encoder.acquire_input(held_input);
        if (!input_acquired || !held_input) {
            fail("second direct encoder input acquisition failed: " + input_acquired.message);
        }
        D3D11_TEXTURE2D_DESC encoder_input_desc{};
        canceled_input.texture()->GetDesc(&encoder_input_desc);
        if ((encoder_input_desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
            fail("tracked encoder input cannot be a VideoProcessor output");
        }
        if (((encoder_input_desc.BindFlags & D3D11_BIND_VIDEO_ENCODER) != 0)
            != encoder_support.video_encoder_input_bind_supported) {
            fail("tracked encoder input did not honor the probed video-encoder bind capability");
        }
        canceled_input.reset();
        gpu::GpuEncoderInputLease direct_input;
        input_acquired = video_encoder.acquire_input(direct_input);
        held_input.reset();
        if (!input_acquired || !direct_input) {
            fail("canceled encoder input did not restore pool credit: "
                + input_acquired.message);
        }
        const auto direct_transform = nv12.process_into(
            frame.texture(), direct_input.texture());
        if (!direct_transform) {
            fail(std::string("direct-to-encoder transform failed: ")
                + direct_transform.what());
        }
        const auto direct_encoded = video_encoder.submit_input(
            std::move(direct_input), 0, frame_duration, true);
        if (!direct_encoded || direct_input) {
            fail("direct encoder input submission failed: " + direct_encoded.message);
        }

        const auto make_lifetime_token = [](
            const std::shared_ptr<std::atomic<std::uint32_t>>& releases) {
            return std::shared_ptr<void>(
                static_cast<void*>(new std::uint8_t{}),
                [releases](void* value) noexcept {
                    delete static_cast<std::uint8_t*>(value);
                    releases->fetch_add(1, std::memory_order_relaxed);
                });
        };
        auto rejected_releases = std::make_shared<std::atomic<std::uint32_t>>(0);
        const auto rejected_external = video_encoder.submit_external_texture(
            frame.texture(),
            make_lifetime_token(rejected_releases),
            frame_duration,
            frame_duration);
        if (rejected_external
            || rejected_external.status != gpu::GpuEncoderStatus::unsupported_input_format
            || rejected_releases->load(std::memory_order_relaxed) != 1) {
            fail("mismatched external encoder surface did not release its token synchronously");
        }
        const auto missing_token = video_encoder.submit_external_texture(
            nv12.output_texture(), {}, frame_duration, frame_duration);
        if (missing_token
            || missing_token.status != gpu::GpuEncoderStatus::invalid_argument) {
            fail("external encoder input accepted an empty lifetime token");
        }

        auto accepted_releases = std::make_shared<std::atomic<std::uint32_t>>(0);
        auto accepted_token = make_lifetime_token(accepted_releases);
        const auto external_encoded = video_encoder.submit_external_texture(
            nv12.output_texture(),
            std::move(accepted_token),
            frame_duration,
            frame_duration);
        if (!external_encoded || accepted_token) {
            fail("external planar encoder input submission failed: "
                + external_encoded.message);
        }

        for (std::int64_t index = 2; index <= 65; ++index) {
            const auto encoded_frame = video_encoder.encode_texture(
                nv12.output_texture(),
                index * frame_duration,
                frame_duration,
                false);
            if (!encoded_frame) {
                fail("hardware H.264 encode failed: " + encoded_frame.message
                    + " (hr=" + std::to_string(encoded_frame.hresult) + ")");
            }
        }
        const auto drained = video_encoder.drain();
        if (!drained) {
            fail("hardware H.264 drain failed: " + drained.message
                + " (hr=" + std::to_string(drained.hresult) + ")");
        }
        if (packets.packet_count == 0 || packets.total_bytes < 100) {
            fail("hardware H.264 encoder emitted no usable packets");
        }
        const auto encoder_stats = video_encoder.stats();
        if (encoder_stats.direct_submissions != 2
            || encoder_stats.external_submissions != 1
            || encoder_stats.external_identity_verified_submissions != 1
            || encoder_stats.external_video_encoder_bound_submissions != 0
            || encoder_stats.external_release_callbacks != 1
            || encoder_stats.external_submission_copy_evidence
                != gpu::GpuCopyEvidenceLevel::l1_no_fluxcap_explicit_copy
            || encoder_stats.copied_submissions != 64) {
            fail("hardware encoder external/direct/copy submission statistics are inconsistent");
        }
        video_encoder.close();
        const auto closed_encoder_identity = video_encoder.mft_identity();
        if (!IsEqualGUID(closed_encoder_identity.clsid, GUID_NULL)
            || closed_encoder_identity.friendly_name[0] != '\0') {
            fail("closed hardware encoder retained a stale MFT identity");
        }
        if (accepted_releases->load(std::memory_order_relaxed) != 1) {
            fail("external encoder input token outlived encoder close");
        }

        if (encoder_support.video_encoder_input_bind_supported) {
            auto strict_config = encoder_config;
            strict_config.require_video_encoder_input_bind = true;
            strict_config.input_pool_size = 1;
            gpu::GpuEncoder strict_encoder;
            const auto strict_initialized = strict_encoder.initialize(
                capture.device(), strict_config, &collect_packet, &packets);
            if (!strict_initialized) {
                fail("strict video-encoder-bind initialization failed: "
                    + strict_initialized.message);
            }
            gpu::GpuEncoderInputLease strict_input;
            const auto strict_acquired = strict_encoder.acquire_input(
                strict_input);
            D3D11_TEXTURE2D_DESC strict_desc{};
            if (strict_input) strict_input.texture()->GetDesc(&strict_desc);
            if (!strict_acquired || !strict_input
                || (strict_desc.BindFlags & D3D11_BIND_VIDEO_ENCODER) == 0) {
                fail("strict encoder pool did not allocate video-encoder-bound input");
            }
            strict_input.reset();
            auto strict_releases =
                std::make_shared<std::atomic<std::uint32_t>>(0);
            const auto strict_rejected =
                strict_encoder.submit_external_texture(
                    nv12.output_texture(),
                    make_lifetime_token(strict_releases),
                    0,
                    frame_duration);
            if (strict_rejected
                || strict_rejected.status
                    != gpu::GpuEncoderStatus::unsupported_input_format
                || strict_releases->load(std::memory_order_relaxed) != 1) {
                fail("strict encoder accepted a planar texture without video-encoder bind");
            }
            strict_encoder.close();
        }

        auto lifetime_config = encoder_config;
        lifetime_config.input_pool_size = 1;
        lifetime_config.event_timeout_ms = 10;
        gpu::GpuEncoder lifetime_encoder;
        const auto lifetime_initialized = lifetime_encoder.initialize(
            capture.device(), lifetime_config, &collect_packet, &packets);
        if (!lifetime_initialized) {
            fail("lease-lifetime encoder initialize failed: "
                + lifetime_initialized.message);
        }
        gpu::GpuEncoderInputLease surviving_input;
        const auto surviving_acquired = lifetime_encoder.acquire_input(
            surviving_input);
        if (!surviving_acquired || !surviving_input) {
            fail("lease-lifetime input acquisition failed: "
                + surviving_acquired.message);
        }
        lifetime_encoder.close();
        if (!surviving_input || surviving_input.texture() == nullptr) {
            fail("encoder destruction invalidated an outstanding input lease");
        }
        surviving_input.reset();

        for (const gpu::VideoCodec codec : {gpu::VideoCodec::hevc, gpu::VideoCodec::av1}) {
            auto optional_config = encoder_config;
            optional_config.codec = codec;
            gpu::GpuEncoderSupport optional_support;
            const auto optional_probe = gpu::GpuEncoder::probe(
                capture.device(), optional_config, optional_support);
            if (!optional_probe || !optional_support.supported
                || !optional_support.d3d11_aware) {
                continue;
            }
            PacketCollector optional_packets;
            gpu::GpuEncoder optional_encoder;
            const auto optional_initialized = optional_encoder.initialize(
                capture.device(),
                optional_config,
                &collect_packet,
                &optional_packets);
            if (!optional_initialized) {
                fail("advertised HEVC/AV1 encoder failed to initialize: "
                    + optional_initialized.message);
            }
            for (std::int64_t index = 0; index < 3; ++index) {
                const auto optional_encoded = optional_encoder.encode_texture(
                    nv12.output_texture(),
                    index * frame_duration,
                    frame_duration,
                    index == 0);
                if (!optional_encoded) {
                    fail("advertised HEVC/AV1 encoder failed: "
                        + optional_encoded.message);
                }
            }
            const auto optional_drained = optional_encoder.drain();
            if (!optional_drained || optional_packets.packet_count == 0) {
                fail("advertised HEVC/AV1 encoder emitted no packet");
            }
        }

        fluxcap::gpu::SharedTexturePublisher publisher;
        fluxcap::gpu::SharedTextureConfig shared_config;
        shared_config.width = frame.info().width;
        shared_config.height = frame.info().height;
        shared_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        shared_config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
        shared_config.producer_key = 17;
        shared_config.consumer_key = 23;
        const auto shared_created = fluxcap::gpu::SharedTexturePublisher::create(
            capture.device(),
            shared_config,
            publisher);
        if (!shared_created) {
            fail(std::string("shared texture create failed: ") + shared_created.what());
        }
        const auto published = publisher.publish(frame.texture(), 1000);
        if (!published) {
            fail(std::string("shared texture publish failed: ") + published.what());
        }
        const std::uint32_t shared_pixel = consume_shared_texture_in_child(publisher);
        if (((shared_pixel >> 16) & 0xffu) <= (shared_pixel & 0xffu) + 80) {
            fail("shared texture did not contain the captured target frame");
        }

        gpu::WicImageEncoder image_encoder;
        const auto image_initialized = image_encoder.initialize(capture.device());
        if (!image_initialized) {
            fail("WIC encoder initialize failed: " + image_initialized.message);
        }
        std::vector<std::uint8_t> png;
        gpu::ImageEncodeOptions image_options;
        image_options.format = gpu::ImageFormat::png;
        const auto encoded = image_encoder.encode_texture_to_memory_sync(
            frame.texture(),
            png,
            image_options);
        constexpr std::uint8_t png_signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        if (!encoded || png.size() < sizeof(png_signature)
            || std::memcmp(png.data(), png_signature, sizeof(png_signature)) != 0) {
            fail("GPU texture PNG compression failed: " + encoded.message);
        }

        std::vector<std::uint8_t> jpeg;
        image_options.format = gpu::ImageFormat::jpeg;
        image_options.jpeg_quality = 0.90F;
        const auto jpeg_encoded = image_encoder.encode_texture_to_memory_sync(
            frame.texture(), jpeg, image_options);
        if (!jpeg_encoded || jpeg.size() < 4
            || jpeg[0] != 0xff || jpeg[1] != 0xd8
            || jpeg[jpeg.size() - 2] != 0xff || jpeg.back() != 0xd9) {
            fail("GPU texture JPEG compression failed: " + jpeg_encoded.message);
        }

        gpu::AsyncGpuPipelineConfig async_config;
        async_config.transform = nv12_config;
        async_config.encoder = encoder_config;
        async_config.queue_depth = 4;
        PacketCollector async_packets;
        gpu::AsyncGpuPipeline async_pipeline;
        const auto async_created = gpu::AsyncGpuPipeline::create(
            capture.device(),
            async_config,
            &collect_packet,
            &async_packets,
            async_pipeline);
        if (!async_created) {
            fail("asynchronous GPU pipeline create failed: " + async_created.message);
        }
        D3D11_TEXTURE2D_DESC incompatible_copy_desc{};
        incompatible_copy_desc.Width = async_config.transform.input_width;
        incompatible_copy_desc.Height = async_config.transform.input_height;
        incompatible_copy_desc.MipLevels = 2;
        incompatible_copy_desc.ArraySize = 1;
        incompatible_copy_desc.Format = async_config.transform.input_format;
        incompatible_copy_desc.SampleDesc.Count = 1;
        incompatible_copy_desc.Usage = D3D11_USAGE_DEFAULT;
        incompatible_copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE
            | D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> incompatible_copy_source;
        const HRESULT incompatible_copy_created =
            capture.device()->CreateTexture2D(
                &incompatible_copy_desc, nullptr, &incompatible_copy_source);
        if (FAILED(incompatible_copy_created)) {
            fail("multi-mip async copy validation texture creation failed");
        }
        const auto copies_before_rejection =
            async_pipeline.stats().input_copy_submissions;
        const auto incompatible_copy = async_pipeline.submit_texture(
            incompatible_copy_source.Get(), 0, frame_duration, false);
        if (incompatible_copy
            || incompatible_copy.status
                != gpu::AsyncGpuPipelineStatus::invalid_argument
            || async_pipeline.stats().input_copy_submissions
                != copies_before_rejection) {
            fail("async pipeline counted an incompatible CopyResource request");
        }
        constexpr std::size_t async_producers = 4;
        constexpr std::size_t submissions_per_producer = 32;
        constexpr std::size_t async_submissions =
            async_producers * submissions_per_producer + 1;
        std::atomic<std::uint64_t> async_index{0};
        std::atomic<bool> async_submit_failed{false};
        std::vector<std::thread> producers;
        producers.reserve(async_producers);
        for (std::size_t producer = 0; producer < async_producers; ++producer) {
            producers.emplace_back([&] {
                for (std::size_t submission = 0;
                     submission < submissions_per_producer;
                     ++submission) {
                    const std::uint64_t index = async_index.fetch_add(
                        1, std::memory_order_relaxed);
                    const auto submitted = async_pipeline.submit_texture(
                        frame.texture(),
                        static_cast<std::int64_t>(index) * frame_duration,
                        frame_duration,
                        index == 0);
                    if (!submitted) {
                        async_submit_failed.store(true, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& producer : producers) producer.join();
        if (async_submit_failed.load(std::memory_order_relaxed)) {
            fail("concurrent asynchronous texture submission failed");
        }
        const auto lease_submitted = async_pipeline.submit_frame(
            std::move(frame),
            static_cast<std::int64_t>(async_submissions - 1) * frame_duration,
            frame_duration,
            false);
        if (!lease_submitted || frame) {
            fail("zero-copy WGC lease handoff failed: " + lease_submitted.message);
        }
        const auto async_drained = async_pipeline.drain(15'000);
        if (!async_drained) {
            fail("asynchronous GPU pipeline drain failed: " + async_drained.message);
        }
        const auto async_stats = async_pipeline.stats();
        if (async_stats.submission_attempts != async_submissions + 1
            || async_stats.accepted_frames != async_submissions
            || async_stats.rejected_frames != 1
            || async_stats.processed_frames == 0
            || async_stats.failed_frames != 0
            || async_stats.encoded_packets == 0
            || async_stats.encoded_bytes < 100
            || async_stats.accepted_frames
                != async_stats.processed_frames + async_stats.overwritten_frames
                    + async_stats.failed_frames
            || async_stats.input_copy_submissions != async_submissions - 1
            || async_stats.transform_submissions
                != async_stats.processed_frames
            || async_stats.encoder_copied_submissions != 0
            || async_stats.encoder_direct_submissions != async_stats.processed_frames
            || async_packets.packet_count == 0
            || async_pipeline.accepting()) {
            fail("asynchronous latest-wins pipeline statistics are inconsistent");
        }

        auto bus_source = create_coordinate_texture(
            capture.device(),
            async_config.transform.input_width,
            async_config.transform.input_height);
        gpu::SharedFrameBusConfig pipeline_bus_config;
        pipeline_bus_config.width = async_config.transform.input_width;
        pipeline_bus_config.height = async_config.transform.input_height;
        pipeline_bus_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        pipeline_bus_config.bind_flags = D3D11_BIND_SHADER_RESOURCE
            | D3D11_BIND_RENDER_TARGET;
        pipeline_bus_config.slot_count = 2;
        pipeline_bus_config.max_consumers = 1;
        gpu::SharedFrameBusFrameMetadata pipeline_metadata;
        pipeline_metadata.valid_fields =
            gpu::shared_frame_bus_metadata_source_timestamp
            | gpu::shared_frame_bus_metadata_qpc
            | gpu::shared_frame_bus_metadata_source_dimensions
            | gpu::shared_frame_bus_metadata_roi
            | gpu::shared_frame_bus_metadata_mailbox_generation
            | gpu::shared_frame_bus_metadata_color_space;
        pipeline_metadata.source_timestamp_100ns = 7'000'000;
        LARGE_INTEGER pipeline_qpc{};
        LARGE_INTEGER pipeline_qpc_frequency{};
        QueryPerformanceCounter(&pipeline_qpc);
        QueryPerformanceFrequency(&pipeline_qpc_frequency);
        pipeline_metadata.timestamp_qpc =
            static_cast<std::uint64_t>(pipeline_qpc.QuadPart);
        pipeline_metadata.qpc_frequency =
            static_cast<std::uint64_t>(pipeline_qpc_frequency.QuadPart);
        pipeline_metadata.mailbox_generation = 1;
        pipeline_metadata.source_width = pipeline_bus_config.width;
        pipeline_metadata.source_height = pipeline_bus_config.height;
        pipeline_metadata.roi_width = pipeline_bus_config.width;
        pipeline_metadata.roi_height = pipeline_bus_config.height;
        pipeline_metadata.color_space =
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        gpu::SharedFrameBusFrameSideData pipeline_side_data;
        pipeline_side_data.epoch = 7;
        pipeline_side_data.epoch_nonce = 0x1234'5678ull;
        pipeline_side_data.damage.flags = gpu::wgc_damage_valid
            | gpu::wgc_damage_full_frame
            | gpu::wgc_damage_discontinuity
            | gpu::wgc_damage_native_move_unavailable;
        pipeline_side_data.damage.dirty_count = 1;
        pipeline_side_data.damage.dirty_rects[0] = {
            0,
            0,
            pipeline_bus_config.width,
            pipeline_bus_config.height};

        gpu::SharedFrameBusPublisher pipeline_bus;
        auto bus_created = gpu::SharedFrameBusPublisher::create(
            capture.device(), pipeline_bus_config, pipeline_bus);
        if (!bus_created) {
            fail(std::string("async bus pipeline publisher create failed: ")
                + bus_created.what());
        }
        gpu::SharedFrameBusRegistration pipeline_registration;
        auto bus_registered = pipeline_bus.register_consumer(
            GetCurrentProcess(), pipeline_registration);
        if (!bus_registered) {
            fail(std::string("async bus pipeline registration failed: ")
                + bus_registered.what());
        }
        gpu::SharedFrameBusConsumer pipeline_consumer;
        auto bus_opened = gpu::SharedFrameBusConsumer::open(
            capture.device(),
            pipeline_registration,
            true,
            pipeline_consumer);
        if (!bus_opened) {
            fail(std::string("async bus pipeline consumer open failed: ")
                + bus_opened.what());
        }
        const auto bus_published = pipeline_bus.publish(
            bus_source.Get(), pipeline_metadata, pipeline_side_data, 5'000);
        if (!bus_published) {
            fail(std::string("async bus pipeline publish failed: ")
                + bus_published.what());
        }

        PacketCollector bus_packets;
        gpu::AsyncGpuPipeline bus_pipeline;
        const auto bus_pipeline_created =
            gpu::AsyncGpuPipeline::create_from_shared_bus(
                capture.device(),
                std::move(pipeline_consumer),
                async_config,
                &collect_packet,
                &bus_packets,
                bus_pipeline);
        if (!bus_pipeline_created || pipeline_consumer.initialized()) {
            fail("async bus pipeline did not take consumer ownership: "
                + bus_pipeline_created.message);
        }
        const auto bus_submit_rejected = bus_pipeline.submit_texture(
            bus_source.Get(), 0, frame_duration, false);
        if (bus_submit_rejected
            || bus_submit_rejected.status
                != gpu::AsyncGpuPipelineStatus::invalid_state) {
            fail("async bus-source pipeline accepted a submitted texture");
        }
        const auto bus_drained = bus_pipeline.drain(15'000);
        if (!bus_drained) {
            fail("async bus pipeline drain failed: " + bus_drained.message);
        }
        const auto bus_pipeline_stats = bus_pipeline.stats();
        if (bus_pipeline_stats.accepted_frames != 1
            || bus_pipeline_stats.processed_frames != 1
            || bus_pipeline_stats.failed_frames != 0
            || bus_pipeline_stats.encoded_packets == 0
            || bus_pipeline_stats.input_copy_submissions != 0
            || bus_pipeline_stats.transform_submissions != 1
            || bus_pipeline_stats.encoder_copied_submissions != 0
            || bus_pipeline_stats.encoder_direct_submissions != 1
            || bus_pipeline_stats.encoder_external_submissions != 0
            || bus_pipeline_stats
                    .encoder_external_identity_verified_submissions != 0
            || bus_pipeline_stats.bus_epoch_changes != 0
            || bus_pipeline_stats.forced_keyframes != 1
            || bus_packets.packet_count == 0
            || !bus_packets.saw_keyframe
            || bus_packets.total_bytes < 100
            || bus_packets.first_timestamp_100ns
                != pipeline_metadata.source_timestamp_100ns
            || bus_pipeline.accepting()) {
            fail("async bus pipeline statistics or metadata timestamp are inconsistent");
        }
        bus_pipeline.close();
        const auto pipeline_unregistered = pipeline_bus.unregister_consumer(
            pipeline_registration, 5'000);
        if (!pipeline_unregistered) {
            fail(std::string("async bus pipeline left a slot pinned: ")
                + pipeline_unregistered.what());
        }

        gpu::SharedFrameBusPublisher mismatch_bus;
        bus_created = gpu::SharedFrameBusPublisher::create(
            capture.device(), pipeline_bus_config, mismatch_bus);
        if (!bus_created) fail("mismatch bus publisher create failed");
        gpu::SharedFrameBusRegistration mismatch_registration;
        bus_registered = mismatch_bus.register_consumer(
            GetCurrentProcess(), mismatch_registration);
        if (!bus_registered) fail("mismatch bus registration failed");
        gpu::SharedFrameBusConsumer mismatch_consumer;
        bus_opened = gpu::SharedFrameBusConsumer::open(
            capture.device(), mismatch_registration, true, mismatch_consumer);
        if (!bus_opened) fail("mismatch bus consumer open failed");
        const auto mismatch_published = mismatch_bus.publish(
            bus_source.Get(), pipeline_metadata, 5'000);
        if (!mismatch_published) fail("mismatch bus publish failed");
        auto mismatch_config = async_config;
        mismatch_config.transform.input_width += 2;
        PacketCollector mismatch_packets;
        gpu::AsyncGpuPipeline mismatch_pipeline;
        const auto mismatch_created =
            gpu::AsyncGpuPipeline::create_from_shared_bus(
                capture.device(),
                std::move(mismatch_consumer),
                mismatch_config,
                &collect_packet,
                &mismatch_packets,
                mismatch_pipeline);
        if (!mismatch_created) {
            fail("mismatch bus pipeline startup failed before frame validation: "
                + mismatch_created.message);
        }
        const auto mismatch_drained = mismatch_pipeline.drain(15'000);
        const auto mismatch_stats = mismatch_pipeline.stats();
        if (mismatch_drained
            || mismatch_drained.status
                != gpu::AsyncGpuPipelineStatus::invalid_argument
            || mismatch_stats.failed_frames != 1
            || mismatch_stats.worker_failures != 1) {
            fail("mismatched bus frame did not fail and release deterministically");
        }
        mismatch_pipeline.close();
        const auto mismatch_unregistered = mismatch_bus.unregister_consumer(
            mismatch_registration, 5'000);
        if (!mismatch_unregistered) {
            fail(std::string("mismatch bus pipeline left its lease pinned: ")
                + mismatch_unregistered.what());
        }

        ComPtr<ID3D11Device> foreign_pipeline_device;
        ComPtr<ID3D11DeviceContext> foreign_pipeline_context;
        D3D_FEATURE_LEVEL foreign_feature_level{};
        HRESULT foreign_device_hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT
                | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &foreign_pipeline_device,
            &foreign_feature_level,
            &foreign_pipeline_context);
        if (FAILED(foreign_device_hr)) {
            fail("foreign pipeline D3D11 device creation failed");
        }
        gpu::SharedFrameBusPublisher device_mismatch_bus;
        bus_created = gpu::SharedFrameBusPublisher::create(
            capture.device(), pipeline_bus_config, device_mismatch_bus);
        if (!bus_created) fail("device-mismatch bus publisher create failed");
        gpu::SharedFrameBusRegistration device_mismatch_registration;
        bus_registered = device_mismatch_bus.register_consumer(
            GetCurrentProcess(), device_mismatch_registration);
        if (!bus_registered) fail("device-mismatch bus registration failed");
        gpu::SharedFrameBusConsumer device_mismatch_consumer;
        bus_opened = gpu::SharedFrameBusConsumer::open(
            capture.device(),
            device_mismatch_registration,
            true,
            device_mismatch_consumer);
        if (!bus_opened) fail("device-mismatch bus consumer open failed");
        const auto device_mismatch_published = device_mismatch_bus.publish(
            bus_source.Get(), pipeline_metadata, 5'000);
        if (!device_mismatch_published) fail("device-mismatch bus publish failed");
        PacketCollector device_mismatch_packets;
        gpu::AsyncGpuPipeline device_mismatch_pipeline;
        const auto device_mismatch_created =
            gpu::AsyncGpuPipeline::create_from_shared_bus(
                foreign_pipeline_device.Get(),
                std::move(device_mismatch_consumer),
                async_config,
                &collect_packet,
                &device_mismatch_packets,
                device_mismatch_pipeline);
        if (!device_mismatch_created) {
            fail("device-mismatch pipeline startup failed before frame validation: "
                + device_mismatch_created.message);
        }
        const auto device_mismatch_drained =
            device_mismatch_pipeline.drain(15'000);
        const auto device_mismatch_stats = device_mismatch_pipeline.stats();
        if (device_mismatch_drained
            || device_mismatch_drained.status
                != gpu::AsyncGpuPipelineStatus::invalid_argument
            || device_mismatch_stats.failed_frames != 1
            || device_mismatch_stats.worker_failures != 1) {
            fail("foreign-device bus frame did not fail and release deterministically");
        }
        device_mismatch_pipeline.close();
        const auto device_mismatch_unregistered =
            device_mismatch_bus.unregister_consumer(
                device_mismatch_registration, 5'000);
        if (!device_mismatch_unregistered) {
            fail(std::string("foreign-device pipeline left its lease pinned: ")
                + device_mismatch_unregistered.what());
        }

        auto failure_config = async_config;
        failure_config.transform.input_width = scale_config.output_width;
        failure_config.transform.input_height = scale_config.output_height;
        gpu::AsyncGpuPipeline failure_pipeline;
        const auto failure_created = gpu::AsyncGpuPipeline::create(
            capture.device(), failure_config, &reject_packet, nullptr, failure_pipeline);
        if (!failure_created) {
            fail("failure-path pipeline create failed: " + failure_created.message);
        }
        std::size_t failure_accepted = 0;
        for (std::size_t index = 0; index < 512; ++index) {
            const auto submitted = failure_pipeline.submit_texture(
                scaled.output_texture(),
                static_cast<std::int64_t>(index) * frame_duration,
                frame_duration,
                index == 0);
            if (!submitted) break;
            ++failure_accepted;
        }
        const auto failure_drained = failure_pipeline.drain(15'000);
        const auto failure_stats = failure_pipeline.stats();
        if (failure_drained
            || failure_drained.status != gpu::AsyncGpuPipelineStatus::encoder_error
            || failure_accepted == 0
            || failure_stats.worker_failures != 1
            || failure_stats.failed_frames != 1
            || failure_stats.accepted_frames
                != failure_stats.processed_frames + failure_stats.overwritten_frames
                    + failure_stats.failed_frames
            || failure_pipeline.accepting()) {
            fail("asynchronous worker-failure retirement is inconsistent");
        }

        PacketCollector close_packets;
        gpu::AsyncGpuPipeline close_pipeline;
        const auto close_created = gpu::AsyncGpuPipeline::create(
            capture.device(), failure_config, &collect_packet, &close_packets, close_pipeline);
        if (!close_created) {
            fail("concurrent-close pipeline create failed: " + close_created.message);
        }
        std::atomic<bool> close_submit_failed{false};
        std::vector<std::thread> close_producers;
        close_producers.reserve(4);
        for (std::size_t producer = 0; producer < 4; ++producer) {
            close_producers.emplace_back([&, producer] {
                for (std::size_t index = 0; index < 10'000; ++index) {
                    const auto submitted = close_pipeline.submit_texture(
                        scaled.output_texture(),
                        static_cast<std::int64_t>(producer * 10'000 + index)
                            * frame_duration,
                        frame_duration,
                        false);
                    if (!submitted) {
                        if (submitted.status != gpu::AsyncGpuPipelineStatus::invalid_state) {
                            close_submit_failed.store(true, std::memory_order_relaxed);
                        }
                        break;
                    }
                }
            });
        }
        std::this_thread::sleep_for(10ms);
        close_pipeline.close();
        for (auto& producer : close_producers) producer.join();
        if (close_submit_failed.load(std::memory_order_relaxed)
            || close_pipeline.initialized()) {
            fail("concurrent asynchronous close was not lifetime-safe");
        }

        frame.reset();
        capture.stop();
        std::cout << "[PASS] occluded/offscreen WGC, direct-bus v3 metadata, 320/640 GPU crop, transforms, codecs, cross-process texture, PNG/JPEG, async latest-wins, and bus-source pipelines; DD native metadata="
            << (dd_native_metadata_executed ? "executed" : "skipped")
            << '\n';
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        HWND target = windows.target.load(std::memory_order_acquire);
        if (target != nullptr) PostMessageW(target, kStopMessage, 0, 0);
        ui.join();
        CloseHandle(windows.ready);
        return 1;
    }

    HWND target = windows.target.load(std::memory_order_acquire);
    if (target != nullptr) PostMessageW(target, kStopMessage, 0, 0);
    ui.join();
    CloseHandle(windows.ready);
    return 0;
}
