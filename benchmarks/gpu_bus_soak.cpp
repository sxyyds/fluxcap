#include <fluxcap/gpu.hpp>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <windows.h>
#include <wrl/client.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace gpu = fluxcap::gpu;
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"FluxCapGpuBusSoakWindow";
constexpr UINT kStopMessage = WM_APP + 71;
constexpr UINT kResizeMessage = WM_APP + 72;
constexpr std::uint32_t kEncoderFps = 60;
constexpr std::uint32_t kAcquireTimeoutMs = 20;
constexpr std::uint32_t kLifecycleTimeoutMs = 5'000;
constexpr std::chrono::seconds kProgressTimeout{10};
constexpr std::chrono::milliseconds kRoiObservationGrace{250};
constexpr std::uint64_t kMaxChurnControlContentionDrops = 16;
constexpr std::uint64_t kControlContentionRatioDivisor = 10'000;

enum class Profile { steady, churn };

struct Options final {
    double soak_minutes = 30.0;
    Profile profile = Profile::steady;
    std::uint32_t size = 320;
    std::uint32_t heartbeat_seconds = 10;
    std::uint32_t monitor_index = 0;
    bool monitor = false;
    bool expect_device_lost = false;
    bool show_help = false;
};

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

std::string hr_text(HRESULT hr) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase
           << static_cast<std::uint32_t>(hr);
    return stream.str();
}

std::string gpu_error_text(const gpu::GpuError& error) {
    return std::string(error.what()) + " (status="
        + std::to_string(static_cast<unsigned>(error.status)) + ", hr="
        + hr_text(error.hresult) + ')';
}

void require_gpu(const gpu::GpuError& result, std::string_view operation) {
    if (!result) {
        fail(std::string(operation) + " failed: " + gpu_error_text(result));
    }
}

std::wstring_view option_value(
    int& index,
    int argc,
    wchar_t* argv[],
    std::wstring_view argument,
    std::wstring_view name) {
    if (argument == name) {
        if (++index >= argc) fail("missing command-line option value");
        return argv[index];
    }
    const std::wstring prefix = std::wstring(name) + L"=";
    if (argument.starts_with(prefix)) return argument.substr(prefix.size());
    return {};
}

std::uint32_t parse_u32(std::wstring_view text, const char* option) {
    if (text.empty()) fail(std::string(option) + " requires a value");
    std::uint64_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            fail(std::string(option) + " must be an integer");
        }
        value = value * 10u + static_cast<std::uint64_t>(character - L'0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            fail(std::string(option) + " is too large");
        }
    }
    return static_cast<std::uint32_t>(value);
}

double parse_minutes(std::wstring_view text) {
    if (text.empty()) fail("--soak-minutes requires a value");
    const std::wstring owned(text);
    wchar_t* end = nullptr;
    const double value = std::wcstod(owned.c_str(), &end);
    if (end == owned.c_str() || *end != L'\0' || !std::isfinite(value)
        || value <= 0.0 || value > 10'080.0) {
        fail("--soak-minutes must be in (0, 10080]");
    }
    return value;
}

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.show_help = true;
        } else if (argument == L"--expect-device-lost") {
            options.expect_device_lost = true;
        } else if (argument == L"--soak-minutes"
            || argument.starts_with(L"--soak-minutes=")) {
            options.soak_minutes = parse_minutes(option_value(
                index, argc, argv, argument, L"--soak-minutes"));
        } else if (argument == L"--profile"
            || argument.starts_with(L"--profile=")) {
            const auto value = option_value(
                index, argc, argv, argument, L"--profile");
            if (value == L"steady") options.profile = Profile::steady;
            else if (value == L"churn") options.profile = Profile::churn;
            else fail("--profile must be steady or churn");
        } else if (argument == L"--size"
            || argument.starts_with(L"--size=")) {
            options.size = parse_u32(option_value(
                index, argc, argv, argument, L"--size"), "--size");
        } else if (argument == L"--heartbeat-seconds"
            || argument.starts_with(L"--heartbeat-seconds=")) {
            options.heartbeat_seconds = parse_u32(option_value(
                index, argc, argv, argument, L"--heartbeat-seconds"),
                "--heartbeat-seconds");
        } else if (argument == L"--monitor"
            || argument.starts_with(L"--monitor=")) {
            options.monitor_index = parse_u32(option_value(
                index, argc, argv, argument, L"--monitor"), "--monitor");
            options.monitor = true;
        } else {
            fail("unknown command-line option");
        }
    }
    if (options.size != 320 && options.size != 640) {
        fail("--size must be 320 or 640");
    }
    if (options.heartbeat_seconds == 0
        || options.heartbeat_seconds > 3'600) {
        fail("--heartbeat-seconds must be in [1, 3600]");
    }
    return options;
}

struct DeviceContext final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

DeviceContext create_device(IDXGIAdapter* adapter = nullptr) {
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
        | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    DeviceContext output;
    D3D_FEATURE_LEVEL created_level{};
    HRESULT hr = D3D11CreateDevice(
        adapter,
        adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &output.device,
        &created_level,
        &output.context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            adapter,
            adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &output.device,
            &created_level,
            &output.context);
    }
    if (FAILED(hr)) {
        fail("D3D11 hardware device creation failed: " + hr_text(hr));
    }
    return output;
}

DeviceContext create_device_for_registration(
    const gpu::SharedFrameBusRegistration& registration) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) fail("DXGI factory creation failed: " + hr_text(hr));

    ComPtr<IDXGIAdapter1> selected;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) fail("DXGI adapter enumeration failed: " + hr_text(hr));
        DXGI_ADAPTER_DESC1 description{};
        hr = candidate->GetDesc1(&description);
        if (FAILED(hr)) fail("DXGI adapter description failed: " + hr_text(hr));
        if (description.AdapterLuid.LowPart == registration.adapter_luid_low
            && description.AdapterLuid.HighPart == registration.adapter_luid_high) {
            selected = std::move(candidate);
            break;
        }
    }
    if (selected == nullptr) {
        fail("the SharedFrameBus adapter is no longer available");
    }
    return create_device(selected.Get());
}

struct MonitorEntry final {
    HMONITOR handle = nullptr;
    MONITORINFOEXW info{};
};

BOOL CALLBACK collect_monitor(
    HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& monitors = *reinterpret_cast<std::vector<MonitorEntry>*>(context);
    MonitorEntry entry;
    entry.handle = monitor;
    entry.info.cbSize = sizeof(entry.info);
    if (GetMonitorInfoW(monitor, &entry.info)) monitors.push_back(entry);
    return TRUE;
}

std::vector<MonitorEntry> enumerate_monitors() {
    std::vector<MonitorEntry> monitors;
    if (!EnumDisplayMonitors(
            nullptr, nullptr, &collect_monitor,
            reinterpret_cast<LPARAM>(&monitors))) {
        fail("monitor enumeration failed: " + std::to_string(GetLastError()));
    }
    return monitors;
}

struct WindowState final {
    HANDLE ready = nullptr;
    std::atomic<HWND> window{nullptr};
    std::atomic<HRESULT> error{S_OK};
    std::atomic<std::uint32_t> client_width{0};
    std::atomic<std::uint32_t> client_height{0};
    std::uint32_t initial_width = 0;
    std::uint32_t initial_height = 0;
    LONG initial_x = 64;
    LONG initial_y = 64;
};

class D3DWindowSource final {
public:
    HRESULT initialize(HWND window) noexcept {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL created_level{};
        constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &device_, &created_level, &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                levels + 1, 1, D3D11_SDK_VERSION,
                &device_, &created_level, &context_);
        }
        if (FAILED(hr)) return hr;

        ComPtr<IDXGIDevice1> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        hr = device_.As(&dxgi_device);
        if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;

        RECT client{};
        if (!GetClientRect(window, &client)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        width_ = static_cast<UINT>(std::max<LONG>(1, client.right));
        height_ = static_cast<UINT>(std::max<LONG>(1, client.bottom));

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
        hr = factory->CreateSwapChainForHwnd(
            device_.Get(), window, &description, nullptr, nullptr, &swap_chain_);
        if (FAILED(hr)) return hr;
        (void)factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        return create_render_target();
    }

    HRESULT resize(UINT width, UINT height) noexcept {
        if (width == 0 || height == 0 || (width == width_ && height == height_)) {
            return S_OK;
        }
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        render_target_.Reset();
        HRESULT hr = swap_chain_->ResizeBuffers(
            0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return hr;
        width_ = width;
        height_ = height;
        return create_render_target();
    }

    HRESULT present(std::uint64_t phase) noexcept {
        const std::uint32_t frame = static_cast<std::uint32_t>(
            phase & 0x00ff'ffffu);
        const float color[] = {
            static_cast<float>(frame & 0xffu) / 255.0F,
            static_cast<float>((frame >> 8u) & 0xffu) / 255.0F,
            static_cast<float>((frame >> 16u) & 0xffu) / 255.0F,
            1.0F};
        context_->ClearRenderTargetView(render_target_.Get(), color);
        return swap_chain_->Present(0, 0);
    }

private:
    HRESULT create_render_target() noexcept {
        ComPtr<ID3D11Texture2D> back_buffer;
        HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        if (FAILED(hr)) return hr;
        return device_->CreateRenderTargetView(
            back_buffer.Get(), nullptr, &render_target_);
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    UINT width_ = 0;
    UINT height_ = 0;
};

class WindowThread final {
public:
    WindowThread(
        std::uint32_t width,
        std::uint32_t height,
        LONG x,
        LONG y) {
        state_.initial_width = width;
        state_.initial_height = height;
        state_.initial_x = x;
        state_.initial_y = y;
        state_.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (state_.ready == nullptr) fail("CreateEventW failed");
        thread_ = std::thread(&WindowThread::run, &state_);
        if (WaitForSingleObject(state_.ready, 10'000) != WAIT_OBJECT_0) {
            stop();
            fail("animation window creation timed out");
        }
        if (FAILED(state_.error.load(std::memory_order_acquire))
            || window() == nullptr) {
            const HRESULT hr = state_.error.load(std::memory_order_relaxed);
            stop();
            fail("animation window creation failed: " + hr_text(hr));
        }
    }

    ~WindowThread() { stop(); }
    WindowThread(const WindowThread&) = delete;
    WindowThread& operator=(const WindowThread&) = delete;

    HWND window() const noexcept {
        return state_.window.load(std::memory_order_acquire);
    }

    HRESULT error() const noexcept {
        return state_.error.load(std::memory_order_acquire);
    }

    void request_client_size(std::uint32_t width, std::uint32_t height) {
        const HWND target = window();
        if (target == nullptr
            || !PostMessageW(target, kResizeMessage, width, height)) {
            fail("failed to post an animation-window resize");
        }
    }

private:
    static LRESULT CALLBACK window_proc(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        WindowState* state = reinterpret_cast<WindowState*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            state = static_cast<WindowState*>(create->lpCreateParams);
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        if (message == WM_SIZE && state != nullptr) {
            state->client_width.store(
                static_cast<std::uint32_t>(LOWORD(lparam)),
                std::memory_order_release);
            state->client_height.store(
                static_cast<std::uint32_t>(HIWORD(lparam)),
                std::memory_order_release);
            return 0;
        }
        if (message == kResizeMessage) {
            RECT bounds{
                0, 0, static_cast<LONG>(wparam), static_cast<LONG>(lparam)};
            if (!AdjustWindowRectEx(
                    &bounds, WS_OVERLAPPEDWINDOW, FALSE, 0)
                || !SetWindowPos(
                    window, nullptr, 0, 0,
                    bounds.right - bounds.left,
                    bounds.bottom - bounds.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
                if (state != nullptr) {
                    state->error.store(
                        HRESULT_FROM_WIN32(GetLastError()),
                        std::memory_order_release);
                }
            }
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == kStopMessage) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static void run(WindowState* state) noexcept {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = &window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = kWindowClass;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (RegisterClassW(&window_class) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            state->error.store(
                HRESULT_FROM_WIN32(GetLastError()), std::memory_order_release);
            SetEvent(state->ready);
            return;
        }

        RECT bounds{
            0, 0,
            static_cast<LONG>(state->initial_width),
            static_cast<LONG>(state->initial_height)};
        if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0)) {
            state->error.store(
                HRESULT_FROM_WIN32(GetLastError()), std::memory_order_release);
            SetEvent(state->ready);
            return;
        }
        HWND window = CreateWindowExW(
            0,
            kWindowClass,
            L"FluxCap SharedFrameBus production soak source",
            WS_OVERLAPPEDWINDOW,
            state->initial_x,
            state->initial_y,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            nullptr,
            nullptr,
            window_class.hInstance,
            state);
        if (window == nullptr) {
            state->error.store(
                HRESULT_FROM_WIN32(GetLastError()), std::memory_order_release);
            SetEvent(state->ready);
            return;
        }
        state->window.store(window, std::memory_order_release);
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);

        D3DWindowSource source;
        HRESULT hr = source.initialize(window);
        if (FAILED(hr)) {
            state->error.store(hr, std::memory_order_release);
            DestroyWindow(window);
            SetEvent(state->ready);
            return;
        }
        hr = source.present(1);
        if (FAILED(hr)) {
            state->error.store(hr, std::memory_order_release);
            DestroyWindow(window);
            SetEvent(state->ready);
            return;
        }
        SetEvent(state->ready);

        std::uint64_t phase = 1;
        MSG message{};
        bool running = true;
        while (running) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;
            const UINT width = state->client_width.load(std::memory_order_acquire);
            const UINT height = state->client_height.load(std::memory_order_acquire);
            if (width != 0 && height != 0) {
                hr = source.resize(width, height);
                if (SUCCEEDED(hr)) hr = source.present(++phase);
            }
            if (FAILED(hr)) {
                state->error.store(hr, std::memory_order_release);
                DestroyWindow(window);
                break;
            }
        }
        state->window.store(nullptr, std::memory_order_release);
    }

    void stop() noexcept {
        const HWND target = window();
        if (target != nullptr) PostMessageW(target, kStopMessage, 0, 0);
        if (thread_.joinable()) thread_.join();
        if (state_.ready != nullptr) {
            CloseHandle(state_.ready);
            state_.ready = nullptr;
        }
    }

    WindowState state_{};
    std::thread thread_;
};

class FixedHistogram final {
public:
    void add_nanoseconds(std::uint64_t nanoseconds) noexcept {
        const std::uint64_t microseconds = std::max<std::uint64_t>(
            1, (nanoseconds + 999u) / 1'000u);
        add_microseconds(microseconds);
    }

    void add_microseconds(std::uint64_t microseconds) noexcept {
        const std::uint64_t value = std::max<std::uint64_t>(1, microseconds);
        const unsigned index = std::min<unsigned>(
            static_cast<unsigned>(bins_.size() - 1),
            std::bit_width(value - 1));
        ++bins_[index];
        ++count_;
    }

    double percentile_ms(double percentile) const noexcept {
        if (count_ == 0) return 0.0;
        const std::uint64_t target = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(
                std::ceil(static_cast<double>(count_) * percentile)));
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < bins_.size(); ++index) {
            cumulative += bins_[index];
            if (cumulative >= target) {
                const long double upper = std::ldexp(1.0L,
                    static_cast<int>(index));
                return static_cast<double>(upper / 1'000.0L);
            }
        }
        return std::numeric_limits<double>::infinity();
    }

    std::uint64_t count() const noexcept { return count_; }

private:
    std::array<std::uint64_t, 64> bins_{};
    std::uint64_t count_ = 0;
};

class ProgressGate final {
public:
    ProgressGate(
        Clock::time_point started,
        std::uint64_t published,
        std::uint64_t consumed,
        std::uint64_t packets) noexcept
        : previous_published_(published),
          previous_consumed_(consumed),
          previous_packets_(packets),
          last_observed_(started) {}

    void observe(
        Clock::time_point now,
        std::uint64_t published,
        std::uint64_t consumed,
        std::uint64_t packets,
        bool roi_unavailable) {
        const Clock::duration elapsed = now - last_observed_;
        last_observed_ = now;
        update(
            "WGC direct-bus publication",
            published, previous_published_, stalled_published_,
            elapsed, roi_unavailable);
        update(
            "primary SharedFrameBus consumption",
            consumed, previous_consumed_, stalled_consumed_,
            elapsed, roi_unavailable);
        update(
            "hardware encoder frame coverage",
            packets, previous_packets_, stalled_packet_,
            elapsed, roi_unavailable);
    }

private:
    static void update(
        const char* operation,
        std::uint64_t current,
        std::uint64_t& previous,
        Clock::duration& stalled,
        Clock::duration elapsed,
        bool paused) {
        if (current < previous) {
            fail(std::string(operation) + " counter regressed");
        }
        if (current > previous) {
            previous = current;
            stalled = Clock::duration::zero();
            return;
        }
        if (!paused) stalled += elapsed;
        if (stalled >= kProgressTimeout) {
            fail(std::string(operation)
                + " made no progress for 10 seconds of ROI-available time");
        }
    }

    std::uint64_t previous_published_ = 0;
    std::uint64_t previous_consumed_ = 0;
    std::uint64_t previous_packets_ = 0;
    Clock::time_point last_observed_{};
    Clock::duration stalled_published_{};
    Clock::duration stalled_consumed_{};
    Clock::duration stalled_packet_{};
};

struct PacketCounters final {
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> keyframes{0};
    std::atomic<std::uint64_t> timestamp_regressions{0};
    std::atomic<std::uint64_t> covered_inputs{0};
    std::atomic<std::uint64_t> unmatched_packets{0};
    std::atomic<std::uint64_t> zero_byte_packets{0};
    std::atomic<std::int64_t> last_timestamp{-1};
    std::mutex pending_mutex;
    std::unordered_set<std::int64_t> pending_timestamps;

    void submit(std::int64_t timestamp) {
        std::lock_guard lock(pending_mutex);
        if (!pending_timestamps.insert(timestamp).second) {
            fail("encoder input timestamp was submitted more than once");
        }
    }

    void cancel(std::int64_t timestamp) noexcept {
        std::lock_guard lock(pending_mutex);
        pending_timestamps.erase(timestamp);
    }

    [[nodiscard]] std::size_t pending_count() {
        std::lock_guard lock(pending_mutex);
        return pending_timestamps.size();
    }
};

void collect_packet(void* context, const gpu::EncodedPacket& packet) {
    auto& counters = *static_cast<PacketCounters*>(context);
    counters.packets.fetch_add(1, std::memory_order_relaxed);
    if (packet.data == nullptr || packet.size == 0) {
        counters.zero_byte_packets.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    counters.bytes.fetch_add(packet.size, std::memory_order_relaxed);
    if (packet.keyframe) {
        counters.keyframes.fetch_add(1, std::memory_order_relaxed);
    }
    const std::int64_t previous = counters.last_timestamp.exchange(
        packet.timestamp_100ns, std::memory_order_relaxed);
    if (previous >= 0 && packet.timestamp_100ns < previous) {
        counters.timestamp_regressions.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard lock(counters.pending_mutex);
        if (counters.pending_timestamps.erase(packet.timestamp_100ns) != 0) {
            counters.covered_inputs.fetch_add(1, std::memory_order_relaxed);
        } else {
            counters.unmatched_packets.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

struct ConsumerMetrics final {
    std::uint64_t acquire_attempts = 0;
    std::uint64_t acquire_timeouts = 0;
    std::uint64_t frames = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t sequence_regressions = 0;
    std::uint64_t metadata_failures = 0;
    std::uint64_t transforms = 0;
    std::uint64_t releases = 0;
    std::uint64_t encode_calls = 0;
    std::uint64_t previous_sequence = 0;
    FixedHistogram acquire_wait;
    FixedHistogram publication_age;
    FixedHistogram transform_submit;
    FixedHistogram release_submit;
    FixedHistogram encode_call;
};

struct ChurnMetrics final {
    std::uint64_t registrations = 0;
    std::uint64_t unregistrations = 0;
    std::uint64_t successful_lifecycles = 0;
    std::uint64_t registration_contention = 0;
    std::uint64_t unregistration_contention = 0;
    std::uint64_t frames = 0;
    std::uint64_t acquire_timeouts = 0;
    std::uint64_t repeated_latest = 0;
    std::uint64_t sequence_regressions = 0;
    std::uint64_t previous_sequence = 0;
};

std::string metadata_problem(
    const gpu::SharedFrameBusFrameMetadata& metadata,
    std::uint32_t size) {
    constexpr std::uint64_t required =
        gpu::shared_frame_bus_metadata_source_timestamp
        | gpu::shared_frame_bus_metadata_qpc
        | gpu::shared_frame_bus_metadata_source_dimensions
        | gpu::shared_frame_bus_metadata_roi
        | gpu::shared_frame_bus_metadata_mailbox_generation
        | gpu::shared_frame_bus_metadata_color_space;
    if (metadata.structure_size != sizeof(metadata)
        || metadata.metadata_version != gpu::shared_frame_bus_metadata_version) {
        return "metadata structure/version mismatch";
    }
    if ((metadata.valid_fields & required) != required) {
        return "metadata valid_fields is incomplete";
    }
    if (metadata.timestamp_qpc == 0 || metadata.qpc_frequency == 0) {
        return "metadata QPC is invalid";
    }
    if (metadata.mailbox_generation == 0) {
        return "metadata mailbox generation is zero";
    }
    if (metadata.roi_width != size || metadata.roi_height != size
        || metadata.source_width < metadata.roi_width
        || metadata.source_height < metadata.roi_height
        || metadata.roi_x > metadata.source_width - metadata.roi_width
        || metadata.roi_y > metadata.source_height - metadata.roi_height) {
        return "metadata source/ROI dimensions are inconsistent";
    }
    if (metadata.color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        return "metadata color space is unexpected";
    }
    return {};
}

void add_publication_age(
    const gpu::SharedFrameBusFrameMetadata& metadata,
    FixedHistogram& histogram) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const std::uint64_t current = static_cast<std::uint64_t>(now.QuadPart);
    if (current < metadata.timestamp_qpc || metadata.qpc_frequency == 0) {
        fail("WGC metadata QPC moved backwards");
    }
    const long double microseconds =
        static_cast<long double>(current - metadata.timestamp_qpc)
        * 1'000'000.0L / static_cast<long double>(metadata.qpc_frequency);
    histogram.add_microseconds(static_cast<std::uint64_t>(
        std::max<long double>(1.0L, std::ceil(microseconds))));
}

class ChurnConsumer final {
public:
    void open(
        gpu::SharedFrameBusPublisher& publisher,
        ID3D11Device* device,
        std::uint32_t size,
        ChurnMetrics& metrics) {
        if (active_) fail("churn consumer is already active");
        const Clock::time_point deadline = Clock::now()
            + std::chrono::milliseconds(kLifecycleTimeoutMs);
        gpu::GpuError registered;
        do {
            registered = publisher.register_consumer(
                GetCurrentProcess(), registration_);
            if (registered || registered.status != gpu::GpuStatus::timeout) break;
            ++metrics.registration_contention;
            Sleep(1);
        } while (Clock::now() < deadline);
        require_gpu(registered, "register churn consumer");

        const gpu::GpuError opened = gpu::SharedFrameBusConsumer::open(
            device, registration_, true, consumer_);
        if (!opened) {
            const gpu::GpuError unregistered = unregister_registration(
                publisher, metrics);
            registration_ = {};
            require_gpu(unregistered, "roll back unopened churn consumer");
            require_gpu(opened, "open churn consumer");
        }

        gpu::GpuTransformConfig config;
        config.input_width = size;
        config.input_height = size;
        config.output_width = std::max<std::uint32_t>(2, size / 2u);
        config.output_height = std::max<std::uint32_t>(2, size / 2u);
        config.output_format = gpu::GpuPixelFormat::bgra8;
        config.frame_rate_numerator = kEncoderFps;
        const gpu::GpuError transformed = gpu::GpuTransform::create(
            device, config, transform_);
        if (!transformed) {
            const gpu::GpuError closed = consumer_.close();
            consumer_ = {};
            const gpu::GpuError unregistered = unregister_registration(
                publisher, metrics);
            registration_ = {};
            require_gpu(closed, "roll back churn consumer transform failure");
            require_gpu(unregistered, "unregister failed churn consumer");
            require_gpu(transformed, "create churn transform");
        }
        frames_this_registration_ = 0;
        active_ = true;
        ++metrics.registrations;
    }

    void process(ChurnMetrics& metrics) {
        if (!active_) return;
        gpu::SharedFrameBusFrameLease lease;
        const gpu::GpuError acquired = consumer_.acquire_latest(0, lease);
        if (!acquired) {
            if (acquired.status == gpu::GpuStatus::timeout) {
                ++metrics.acquire_timeouts;
                return;
            }
            fail("churn consumer acquire failed: " + gpu_error_text(acquired));
        }
        const std::uint64_t sequence = lease.info().sequence;
        if (metrics.previous_sequence != 0) {
            if (sequence < metrics.previous_sequence) {
                ++metrics.sequence_regressions;
            } else if (sequence == metrics.previous_sequence) {
                // A newly registered latest-wins consumer can legitimately
                // observe the last frame again before the next publication.
                ++metrics.repeated_latest;
            }
        }
        metrics.previous_sequence = sequence;

        const gpu::GpuError transformed = transform_.process(lease.texture());
        const gpu::GpuError released = consumer_.release(lease);
        require_gpu(released, "release churn frame");
        require_gpu(transformed, "transform churn frame");
        ++metrics.frames;
        ++frames_this_registration_;
    }

    void close(
        gpu::SharedFrameBusPublisher& publisher,
        ChurnMetrics& metrics,
        bool require_consumed_frame = true) {
        if (!active_) return;
        const gpu::GpuError closed = consumer_.close();
        transform_ = {};
        consumer_ = {};

        const gpu::GpuError unregistered = unregister_registration(
            publisher, metrics);
        require_gpu(unregistered, "unregister churn consumer");
        const bool consumed_frame = frames_this_registration_ != 0;
        registration_ = {};
        active_ = false;
        frames_this_registration_ = 0;
        ++metrics.unregistrations;
        if (consumed_frame) ++metrics.successful_lifecycles;
        require_gpu(closed, "close churn consumer");
        if (require_consumed_frame && !consumed_frame) {
            fail("churn consumer lifecycle closed without consuming a frame");
        }
    }

    bool active() const noexcept { return active_; }

private:
    gpu::GpuError unregister_registration(
        gpu::SharedFrameBusPublisher& publisher,
        ChurnMetrics& metrics) {
        const Clock::time_point deadline = Clock::now()
            + std::chrono::milliseconds(kLifecycleTimeoutMs);
        gpu::GpuError unregistered;
        do {
            unregistered = publisher.unregister_consumer(registration_, 100);
            if (unregistered || unregistered.status != gpu::GpuStatus::timeout) {
                break;
            }
            ++metrics.unregistration_contention;
            Sleep(1);
        } while (Clock::now() < deadline);
        return unregistered;
    }

    gpu::SharedFrameBusRegistration registration_{};
    gpu::SharedFrameBusConsumer consumer_;
    gpu::GpuTransform transform_;
    std::uint64_t frames_this_registration_ = 0;
    bool active_ = false;
};

struct RegionExercise final {
    bool waiting_unavailable = false;
    bool unavailable_seen = false;
    bool waiting_recovery = false;
    bool recovery_seen = false;
    std::uint64_t recovery_start_sequence = 0;
    std::uint64_t unavailable_events = 0;
    std::uint64_t recoveries = 0;
    std::uint64_t recovery_timeouts = 0;

    void observe(
        const gpu::WgcMailboxState& mailbox,
        std::uint64_t sequence) noexcept {
        if (waiting_unavailable && !mailbox.region_available) {
            unavailable_seen = true;
        }
        if (waiting_recovery && mailbox.region_available
            && sequence > recovery_start_sequence) {
            recovery_seen = true;
        }
    }

    void begin_small_phase() {
        if (waiting_recovery) {
            finish_recovery_phase();
        }
        waiting_unavailable = true;
        unavailable_seen = false;
    }

    void begin_recovery_phase(std::uint64_t sequence) {
        if (!waiting_unavailable || !unavailable_seen) {
            ++recovery_timeouts;
            fail("window shrink did not produce region_unavailable before its deadline");
        }
        ++unavailable_events;
        waiting_unavailable = false;
        unavailable_seen = false;
        waiting_recovery = true;
        recovery_seen = false;
        recovery_start_sequence = sequence;
    }

    void finish_recovery_phase() {
        if (!waiting_recovery) return;
        if (!recovery_seen) {
            ++recovery_timeouts;
            fail("ROI did not resume publishing before the recovery deadline");
        }
        ++recoveries;
        waiting_recovery = false;
        recovery_seen = false;
    }

    void finalize_at_run_end() {
        if (waiting_unavailable) {
            ++recovery_timeouts;
            fail("ROI unavailable phase was not advanced to recovery before run end");
        }
        finish_recovery_phase();
    }
};

std::uint64_t working_set_bytes() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
}

struct DeviceLoss final {
    bool observed = false;
    HRESULT producer_reason = S_OK;
    HRESULT consumer_reason = S_OK;
};

bool inspect_device_loss(
    ID3D11Device* producer,
    ID3D11Device* consumer,
    bool expected,
    DeviceLoss& output) {
    const HRESULT producer_reason = producer->GetDeviceRemovedReason();
    const HRESULT consumer_reason = consumer->GetDeviceRemovedReason();
    if (!FAILED(producer_reason) && !FAILED(consumer_reason)) return false;
    output.observed = true;
    output.producer_reason = producer_reason;
    output.consumer_reason = consumer_reason;
    if (!expected) {
        fail("D3D11 device removed unexpectedly (producer="
            + hr_text(producer_reason) + ", consumer="
            + hr_text(consumer_reason) + ')');
    }
    return true;
}

void print_heartbeat(
    double elapsed_seconds,
    const gpu::WgcCapture& capture,
    const gpu::SharedFrameBusPublisher& publisher,
    const ConsumerMetrics& consumer,
    const PacketCounters& packets,
    const ChurnConsumer& churn,
    const ChurnMetrics& churn_metrics,
    std::uint64_t& previous_published,
    std::uint64_t& previous_consumed,
    double interval_seconds) {
    const gpu::WgcCaptureStats wgc_stats = capture.stats();
    const gpu::SharedFrameBusStats bus_stats = publisher.stats();
    const gpu::WgcMailboxState mailbox = capture.mailbox_state();
    const double published_rate = interval_seconds <= 0.0 ? 0.0
        : static_cast<double>(wgc_stats.published_frames - previous_published)
            / interval_seconds;
    const double consumed_rate = interval_seconds <= 0.0 ? 0.0
        : static_cast<double>(consumer.frames - previous_consumed)
            / interval_seconds;
    previous_published = wgc_stats.published_frames;
    previous_consumed = consumer.frames;

    std::cout << std::fixed << std::setprecision(1)
              << "heartbeat elapsed=" << elapsed_seconds << "s"
              << " publish_rate=" << published_rate << "/s"
              << " consume_rate=" << consumed_rate << "/s"
              << " wgc_received=" << wgc_stats.received_frames
              << " wgc_published=" << wgc_stats.published_frames
              << " bus_sequence=" << publisher.sequence()
              << " packets=" << packets.packets.load(std::memory_order_relaxed)
              << " no_slot=" << bus_stats.no_slot
              << " quarantined=" << bus_stats.quarantined_slots
              << " active_consumers=" << bus_stats.active_consumers
              << " churn_active=" << (churn.active() ? 1 : 0)
              << " churn_cycles=" << churn_metrics.unregistrations
              << " roi=" << (mailbox.region_available ? "available" : "unavailable")
              << " age_p95_ms=" << consumer.publication_age.percentile_ms(0.95)
              << " working_set_mib="
              << static_cast<double>(working_set_bytes()) / (1024.0 * 1024.0)
              << '\n' << std::flush;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.show_help) {
            std::cout
                << "Usage: fluxcap_gpu_bus_soak [--soak-minutes=N] "
                   "[--profile=steady|churn] [--size=320|640] "
                   "[--heartbeat-seconds=10] [--monitor=N] "
                   "[--expect-device-lost]\n\n"
                   "The default 30-minute run is intentionally not part of CTest.\n"
                   "--expect-device-lost observes an externally induced loss; "
                   "this tool never triggers a TDR.\n";
            return 0;
        }

        (void)SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        const std::vector<MonitorEntry> monitors = enumerate_monitors();
        if (monitors.empty()) fail("no active monitor is available");
        const MonitorEntry* selected_monitor = nullptr;
        if (options.monitor) {
            if (options.monitor_index >= monitors.size()) {
                fail("--monitor index is outside the active monitor list");
            }
            selected_monitor = &monitors[options.monitor_index];
            const LONG monitor_width = selected_monitor->info.rcMonitor.right
                - selected_monitor->info.rcMonitor.left;
            const LONG monitor_height = selected_monitor->info.rcMonitor.bottom
                - selected_monitor->info.rcMonitor.top;
            if (monitor_width < static_cast<LONG>(options.size)
                || monitor_height < static_cast<LONG>(options.size)) {
                fail("selected monitor is smaller than the requested ROI");
            }
        }

        const std::uint32_t large_width = std::max<std::uint32_t>(
            960, options.size + 320u);
        const std::uint32_t large_height = std::max<std::uint32_t>(
            720, options.size + 240u);
        LONG window_x = 64;
        LONG window_y = 64;
        if (selected_monitor != nullptr) {
            const RECT area = selected_monitor->info.rcWork;
            window_x = area.left + (area.right - area.left
                - static_cast<LONG>(large_width)) / 2;
            window_y = area.top + (area.bottom - area.top
                - static_cast<LONG>(large_height)) / 2;
        }
        WindowThread animation_window(
            large_width, large_height, window_x, window_y);

        DeviceContext producer_device = create_device();
        gpu::SharedFrameBusConfig bus_config;
        bus_config.width = options.size;
        bus_config.height = options.size;
        bus_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bus_config.bind_flags = D3D11_BIND_SHADER_RESOURCE
            | D3D11_BIND_RENDER_TARGET;
        bus_config.slot_count = gpu::shared_frame_bus_max_slots;
        bus_config.max_consumers = 3;
        gpu::SharedFrameBusPublisher publisher;
        require_gpu(gpu::SharedFrameBusPublisher::create(
            producer_device.device.Get(), bus_config, publisher),
            "create SharedFrameBus publisher");

        gpu::SharedFrameBusRegistration primary_registration;
        require_gpu(publisher.register_consumer(
            GetCurrentProcess(), primary_registration),
            "register primary consumer");
        DeviceContext consumer_device = create_device_for_registration(
            primary_registration);
        gpu::SharedFrameBusConsumer primary_consumer;
        require_gpu(gpu::SharedFrameBusConsumer::open(
            consumer_device.device.Get(), primary_registration, true,
            primary_consumer),
            "open primary consumer");

        gpu::GpuTransformConfig transform_config;
        transform_config.input_width = options.size;
        transform_config.input_height = options.size;
        transform_config.output_width = options.size;
        transform_config.output_height = options.size;
        transform_config.output_format = gpu::GpuPixelFormat::nv12;
        transform_config.frame_rate_numerator = kEncoderFps;
        gpu::GpuTransform transform;
        require_gpu(gpu::GpuTransform::create(
            consumer_device.device.Get(), transform_config, transform),
            "create primary NV12 transform");

        gpu::GpuEncoderConfig encoder_config;
        encoder_config.codec = gpu::VideoCodec::h264;
        encoder_config.width = options.size;
        encoder_config.height = options.size;
        encoder_config.frame_rate_numerator = kEncoderFps;
        encoder_config.frame_rate_denominator = 1;
        encoder_config.bitrate = options.size == 640 ? 8'000'000 : 4'000'000;
        encoder_config.gop_size = 120;
        encoder_config.input_format = DXGI_FORMAT_NV12;
        encoder_config.low_latency = true;
        encoder_config.input_pool_size = 4;
        encoder_config.event_timeout_ms = 5'000;
        gpu::GpuEncoderSupport encoder_support;
        const gpu::GpuEncoderResult probed = gpu::GpuEncoder::probe(
            consumer_device.device.Get(), encoder_config, encoder_support);
        if (!probed || !encoder_support.supported
            || !encoder_support.d3d11_aware) {
            fail("production soak requires a D3D11-aware hardware H.264 encoder; "
                "probe failed: " + probed.message + " (hr="
                + hr_text(probed.hresult) + ')');
        }
        PacketCounters packets;
        gpu::GpuEncoder encoder;
        const gpu::GpuEncoderResult encoder_initialized = encoder.initialize(
            consumer_device.device.Get(), encoder_config,
            &collect_packet, &packets);
        if (!encoder_initialized) {
            fail("hardware H.264 encoder initialization failed: "
                + encoder_initialized.message + " (hr="
                + hr_text(encoder_initialized.hresult) + ')');
        }

        gpu::WgcCaptureOptions capture_options;
        capture_options.buffer_count = 3;
        capture_options.include_cursor = false;
        capture_options.require_border = false;
        capture_options.min_update_interval_us = 0;
        gpu::WgcMailboxConfig mailbox;
        mailbox.mode = gpu::WgcMailboxMode::centered_region;
        mailbox.width = options.size;
        mailbox.height = options.size;
        gpu::WgcCapture capture;
        const gpu::WgcResult capture_created = selected_monitor == nullptr
            ? gpu::WgcCapture::create_for_window_to_bus(
                animation_window.window(), publisher,
                capture_options, mailbox, capture)
            : gpu::WgcCapture::create_for_monitor_to_bus(
                selected_monitor->handle, publisher,
                capture_options, mailbox, capture);
        if (!capture_created) {
            fail("WGC direct-bus creation failed: " + capture_created.message
                + " (hr=" + hr_text(capture_created.hresult) + ')');
        }
        const gpu::WgcResult started = capture.start();
        if (!started) {
            fail("WGC direct-bus start failed: " + started.message
                + " (hr=" + hr_text(started.hresult) + ')');
        }

        const auto run_duration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(options.soak_minutes * 60.0));
        const Clock::time_point run_begin = Clock::now();
        const Clock::time_point run_end = run_begin + run_duration;
        const auto heartbeat_period = std::chrono::seconds(
            options.heartbeat_seconds);
        Clock::time_point next_heartbeat = run_begin + heartbeat_period;
        Clock::time_point last_heartbeat = run_begin;

        const auto dynamic_period = std::min(
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::seconds(5)),
            std::max(
                std::chrono::duration_cast<Clock::duration>(
                    std::chrono::milliseconds(300)),
                run_duration / 5));
        const Clock::duration roi_observation_grace = std::min(
            std::chrono::duration_cast<Clock::duration>(kRoiObservationGrace),
            dynamic_period / 4);
        Clock::time_point next_resize = run_begin + dynamic_period;
        Clock::time_point next_churn = run_begin + dynamic_period;
        bool roi_schedule_complete = false;
        std::uint64_t resize_phase = 0;

        ConsumerMetrics consumer_metrics;
        ChurnMetrics churn_metrics;
        ChurnConsumer churn;
        RegionExercise region;
        if (options.profile == Profile::churn) {
            churn.open(
                publisher, consumer_device.device.Get(),
                options.size, churn_metrics);
        }

        const std::int64_t frame_duration_100ns =
            10'000'000 / static_cast<std::int64_t>(kEncoderFps);
        std::int64_t last_encoder_timestamp = -1;
        std::uint64_t previous_published = 0;
        std::uint64_t previous_consumed = 0;
        DeviceLoss device_loss;
        ProgressGate progress(
            run_begin,
            capture.stats().published_frames,
            consumer_metrics.frames,
            packets.covered_inputs.load(std::memory_order_relaxed));

        std::cout << "FluxCap SharedFrameBus production soak\n"
                  << "  source=" << (selected_monitor == nullptr ? "window" : "monitor")
                  << " profile="
                  << (options.profile == Profile::steady ? "steady" : "churn")
                  << " size=" << options.size << 'x' << options.size
                  << " minutes=" << options.soak_minutes
                  << " protocol=" << primary_registration.protocol_version
                  << " slots=" << primary_registration.slot_count << '\n'
                  << "  consumer path: lease.texture -> GpuTransform(NV12) -> "
                     "release -> hardware H.264 encoder\n";

        while (Clock::now() < run_end) {
            if (inspect_device_loss(
                    producer_device.device.Get(),
                    consumer_device.device.Get(),
                    options.expect_device_lost,
                    device_loss)) {
                break;
            }
            if (FAILED(animation_window.error())
                || animation_window.window() == nullptr) {
                fail("animation source failed during soak: "
                    + hr_text(animation_window.error()));
            }

            const Clock::time_point acquire_begin = Clock::now();
            ++consumer_metrics.acquire_attempts;
            gpu::SharedFrameBusFrameLease lease;
            const gpu::GpuError acquired = primary_consumer.acquire_latest(
                kAcquireTimeoutMs, lease);
            const Clock::time_point acquire_end = Clock::now();
            consumer_metrics.acquire_wait.add_nanoseconds(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        acquire_end - acquire_begin).count()));
            if (!acquired) {
                if (acquired.status == gpu::GpuStatus::timeout) {
                    ++consumer_metrics.acquire_timeouts;
                } else if (inspect_device_loss(
                        producer_device.device.Get(),
                        consumer_device.device.Get(),
                        options.expect_device_lost,
                        device_loss)) {
                    break;
                } else {
                    fail("primary acquire failed: " + gpu_error_text(acquired));
                }
            } else {
                const gpu::SharedFrameBusFrameInfo info = lease.info();
                const gpu::SharedFrameBusFrameMetadata metadata = lease.metadata();
                const std::string problem = metadata_problem(
                    metadata, options.size);
                if (!problem.empty()) {
                    ++consumer_metrics.metadata_failures;
                    (void)primary_consumer.release(lease);
                    fail("invalid SharedFrameBus v2 metadata: " + problem);
                }
                if (consumer_metrics.previous_sequence != 0) {
                    if (info.sequence <= consumer_metrics.previous_sequence) {
                        ++consumer_metrics.sequence_regressions;
                        (void)primary_consumer.release(lease);
                        fail("primary SharedFrameBus sequence regressed");
                    }
                    consumer_metrics.sequence_gaps += info.sequence
                        - consumer_metrics.previous_sequence - 1;
                }
                consumer_metrics.previous_sequence = info.sequence;
                add_publication_age(metadata, consumer_metrics.publication_age);

                const Clock::time_point transform_begin = Clock::now();
                const gpu::GpuError transformed = transform.process(
                    lease.texture());
                const Clock::time_point transform_end = Clock::now();
                consumer_metrics.transform_submit.add_nanoseconds(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            transform_end - transform_begin).count()));

                const Clock::time_point release_begin = Clock::now();
                const gpu::GpuError released = primary_consumer.release(lease);
                const Clock::time_point release_end = Clock::now();
                consumer_metrics.release_submit.add_nanoseconds(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            release_end - release_begin).count()));
                if (!released) {
                    if (inspect_device_loss(
                            producer_device.device.Get(),
                            consumer_device.device.Get(),
                            options.expect_device_lost,
                            device_loss)) {
                        break;
                    }
                    fail("primary release failed: " + gpu_error_text(released));
                }
                ++consumer_metrics.releases;
                if (!transformed) {
                    if (inspect_device_loss(
                            producer_device.device.Get(),
                            consumer_device.device.Get(),
                            options.expect_device_lost,
                            device_loss)) {
                        break;
                    }
                    fail("primary transform failed: "
                        + gpu_error_text(transformed));
                }
                ++consumer_metrics.transforms;

                std::int64_t timestamp = metadata.source_timestamp_100ns;
                if (timestamp < 0 || timestamp <= last_encoder_timestamp) {
                    timestamp = last_encoder_timestamp < 0
                        ? 0
                        : last_encoder_timestamp + frame_duration_100ns;
                }
                last_encoder_timestamp = timestamp;
                packets.submit(timestamp);
                const Clock::time_point encode_begin = Clock::now();
                const gpu::GpuEncoderResult encoded = encoder.encode_texture(
                    transform.output_texture(), timestamp,
                    frame_duration_100ns,
                    consumer_metrics.frames == 0);
                const Clock::time_point encode_end = Clock::now();
                consumer_metrics.encode_call.add_nanoseconds(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            encode_end - encode_begin).count()));
                if (!encoded) {
                    packets.cancel(timestamp);
                    if (inspect_device_loss(
                            producer_device.device.Get(),
                            consumer_device.device.Get(),
                            options.expect_device_lost,
                            device_loss)) {
                        break;
                    }
                    fail("hardware H.264 encode failed: " + encoded.message
                        + " (hr=" + hr_text(encoded.hresult) + ')');
                }
                ++consumer_metrics.encode_calls;
                ++consumer_metrics.frames;
            }

            if (options.profile == Profile::churn) {
                try {
                    churn.process(churn_metrics);
                } catch (...) {
                    if (!inspect_device_loss(
                            producer_device.device.Get(),
                            consumer_device.device.Get(),
                            options.expect_device_lost,
                            device_loss)) {
                        throw;
                    }
                    break;
                }
            }

            Clock::time_point now = Clock::now();
            if (options.profile == Profile::churn && now >= next_churn) {
                try {
                    if (churn.active()) churn.close(publisher, churn_metrics);
                    else churn.open(
                        publisher, consumer_device.device.Get(),
                        options.size, churn_metrics);
                } catch (...) {
                    if (!inspect_device_loss(
                            producer_device.device.Get(),
                            consumer_device.device.Get(),
                            options.expect_device_lost,
                            device_loss)) {
                        throw;
                    }
                    break;
                }
                next_churn = Clock::now() + dynamic_period;
            }

            // Churn registration can wait on protocol cleanup. Sample both the
            // clock and mailbox again before enforcing ROI deadlines.
            now = Clock::now();
            gpu::WgcMailboxState mailbox_state = capture.mailbox_state();
            region.observe(mailbox_state, publisher.sequence());
            if (options.profile == Profile::churn
                && selected_monitor == nullptr
                && !roi_schedule_complete
                && now >= next_resize) {
                if (!region.waiting_unavailable && !region.waiting_recovery) {
                    if (now + dynamic_period * 2 + roi_observation_grace * 2
                        > run_end) {
                        roi_schedule_complete = true;
                    } else {
                        region.begin_small_phase();
                        animation_window.request_client_size(
                            std::max<std::uint32_t>(64, options.size / 2u),
                            std::max<std::uint32_t>(64, options.size / 2u));
                        next_resize = now + dynamic_period;
                    }
                } else if (region.waiting_unavailable) {
                    if (region.unavailable_seen
                        || now >= next_resize + roi_observation_grace) {
                        region.begin_recovery_phase(publisher.sequence());
                        ++resize_phase;
                        const std::uint32_t growth =
                            (resize_phase & 1u) != 0 ? 64u : 0u;
                        animation_window.request_client_size(
                            large_width + growth, large_height + growth);
                        next_resize = Clock::now() + dynamic_period;
                    }
                } else {
                    if (region.recovery_seen
                        || now >= next_resize + roi_observation_grace) {
                        region.finish_recovery_phase();
                        now = Clock::now();
                        if (now + dynamic_period * 2 + roi_observation_grace * 2
                            > run_end) {
                            roi_schedule_complete = true;
                        } else {
                            region.begin_small_phase();
                            animation_window.request_client_size(
                                std::max<std::uint32_t>(64, options.size / 2u),
                                std::max<std::uint32_t>(64, options.size / 2u));
                            next_resize = now + dynamic_period;
                        }
                    }
                }
            }
            mailbox_state = capture.mailbox_state();
            region.observe(mailbox_state, publisher.sequence());

            const bool roi_phase_active =
                options.profile == Profile::churn
                && selected_monitor == nullptr
                && (region.waiting_unavailable || region.waiting_recovery);
            const bool expected_roi_unavailable = roi_phase_active
                && !mailbox_state.region_available;
            if (capture.target_closed()) {
                fail("WGC capture target closed during soak");
            }
            if (!capture.running()) {
                if (inspect_device_loss(
                        producer_device.device.Get(),
                        consumer_device.device.Get(),
                        options.expect_device_lost,
                        device_loss)) {
                    break;
                }
                fail("WGC capture stopped unexpectedly during soak");
            }
            const gpu::WgcResult capture_error = capture.last_error();
            if (!capture_error
                && !(roi_phase_active
                    && capture_error.status == gpu::WgcStatus::region_unavailable)) {
                if (inspect_device_loss(
                        producer_device.device.Get(),
                        consumer_device.device.Get(),
                        options.expect_device_lost,
                        device_loss)) {
                    break;
                }
                fail("WGC capture reported an error: " + capture_error.message
                    + " (hr=" + hr_text(capture_error.hresult) + ')');
            }
            const gpu::WgcCaptureStats progress_stats = capture.stats();
            progress.observe(
                Clock::now(),
                progress_stats.published_frames,
                consumer_metrics.frames,
                packets.covered_inputs.load(std::memory_order_relaxed),
                expected_roi_unavailable);

            if (now >= next_heartbeat) {
                const double elapsed = std::chrono::duration<double>(
                    now - run_begin).count();
                const double interval = std::chrono::duration<double>(
                    now - last_heartbeat).count();
                print_heartbeat(
                    elapsed, capture, publisher, consumer_metrics,
                    packets, churn, churn_metrics,
                    previous_published, previous_consumed, interval);
                const gpu::SharedFrameBusStats heartbeat_stats = publisher.stats();
                if (heartbeat_stats.quarantined_slots != 0) {
                    fail("SharedFrameBus slot quarantine detected during soak");
                }
                last_heartbeat = now;
                next_heartbeat = now + heartbeat_period;
            }
        }

        if (!device_loss.observed
            && options.profile == Profile::churn
            && selected_monitor == nullptr) {
            region.observe(capture.mailbox_state(), publisher.sequence());
            region.finalize_at_run_end();
        }

        capture.stop();
        std::string cleanup_warnings;
        const auto note_cleanup_warning = [&cleanup_warnings](std::string message) {
            if (!cleanup_warnings.empty()) cleanup_warnings += "; ";
            cleanup_warnings += std::move(message);
        };
        if (churn.active()) {
            if (device_loss.observed) {
                try {
                    churn.close(publisher, churn_metrics, false);
                } catch (const std::exception& error) {
                    note_cleanup_warning(
                        std::string("churn cleanup: ") + error.what());
                }
            } else {
                churn.close(publisher, churn_metrics);
            }
        }
        if (!device_loss.observed) {
            const gpu::GpuEncoderResult drained = encoder.drain();
            if (!drained) {
                fail("hardware H.264 encoder drain failed: " + drained.message
                    + " (hr=" + hr_text(drained.hresult) + ')');
            }
            const std::uint64_t covered_inputs = packets.covered_inputs.load(
                std::memory_order_relaxed);
            if (packets.pending_count() != 0
                || covered_inputs != consumer_metrics.encode_calls) {
                fail("hardware H.264 drain left an accepted input timestamp without output coverage");
            }
        }
        encoder.close();
        transform = {};

        if (device_loss.observed) {
            const gpu::GpuError closed = primary_consumer.close();
            if (!closed) {
                note_cleanup_warning(
                    "primary close: " + gpu_error_text(closed));
            }
            primary_consumer = {};
            const gpu::GpuError unregistered = publisher.unregister_consumer(
                primary_registration, kLifecycleTimeoutMs);
            if (!unregistered) {
                note_cleanup_warning(
                    "primary unregister: " + gpu_error_text(unregistered));
            }
        } else {
            require_gpu(primary_consumer.close(), "close primary consumer");
            primary_consumer = {};
            require_gpu(publisher.unregister_consumer(
                primary_registration, kLifecycleTimeoutMs),
                "unregister primary consumer");
        }

        const double elapsed_seconds = std::chrono::duration<double>(
            Clock::now() - run_begin).count();
        const gpu::WgcCaptureStats wgc_stats = capture.stats();
        const gpu::SharedFrameBusStats bus_stats = publisher.stats();
        const std::uint64_t packet_count = packets.packets.load(
            std::memory_order_relaxed);
        const bool skip_counters_consistent = wgc_stats.skipped_no_buffer
            >= bus_stats.no_slot;
        const std::uint64_t control_contention = skip_counters_consistent
            ? wgc_stats.skipped_no_buffer - bus_stats.no_slot
            : 0;
        const std::uint64_t control_contention_limit =
            options.profile == Profile::churn
            ? std::min(
                kMaxChurnControlContentionDrops,
                std::max<std::uint64_t>(
                    1,
                    wgc_stats.received_frames
                        / kControlContentionRatioDivisor))
            : 0;

        std::cout << std::fixed << std::setprecision(3)
                  << "\nsoak summary\n"
                  << "  elapsed_s=" << elapsed_seconds
                  << " wgc_received=" << wgc_stats.received_frames
                  << " wgc_published=" << wgc_stats.published_frames
                  << " wgc_skipped_no_buffer=" << wgc_stats.skipped_no_buffer
                  << " bus_published=" << bus_stats.published_frames
                  << " bus_no_slot=" << bus_stats.no_slot
                  << " control_contention=" << control_contention
                  << '/' << control_contention_limit << '\n'
                  << "  consumed=" << consumer_metrics.frames
                  << " consume_rate="
                  << (elapsed_seconds > 0.0
                        ? static_cast<double>(consumer_metrics.frames)
                            / elapsed_seconds
                        : 0.0)
                  << "/s acquire_timeouts=" << consumer_metrics.acquire_timeouts
                  << " sequence_gaps=" << consumer_metrics.sequence_gaps
                  << " sequence_regressions="
                  << consumer_metrics.sequence_regressions << '\n'
                  << "  publication_age_ms p50="
                  << consumer_metrics.publication_age.percentile_ms(0.50)
                  << " p95="
                  << consumer_metrics.publication_age.percentile_ms(0.95)
                  << " p99="
                  << consumer_metrics.publication_age.percentile_ms(0.99)
                  << " transform_p95_ms="
                  << consumer_metrics.transform_submit.percentile_ms(0.95)
                  << " release_p95_ms="
                  << consumer_metrics.release_submit.percentile_ms(0.95)
                  << " encode_p95_ms="
                  << consumer_metrics.encode_call.percentile_ms(0.95) << '\n'
                  << "  packets=" << packet_count
                  << " bytes=" << packets.bytes.load(std::memory_order_relaxed)
                  << " keyframes="
                  << packets.keyframes.load(std::memory_order_relaxed)
                  << " covered_inputs="
                  << packets.covered_inputs.load(std::memory_order_relaxed)
                  << '/' << consumer_metrics.encode_calls
                  << " unmatched_packets="
                  << packets.unmatched_packets.load(std::memory_order_relaxed)
                  << " zero_byte_packets="
                  << packets.zero_byte_packets.load(std::memory_order_relaxed)
                  << " packet_timestamp_regressions="
                  << packets.timestamp_regressions.load(std::memory_order_relaxed)
                  << '\n'
                  << "  churn_registrations=" << churn_metrics.registrations
                  << " churn_unregistrations=" << churn_metrics.unregistrations
                  << " churn_successful_lifecycles="
                  << churn_metrics.successful_lifecycles
                  << " churn_frames=" << churn_metrics.frames
                  << " churn_repeated_latest=" << churn_metrics.repeated_latest
                  << " churn_sequence_regressions="
                  << churn_metrics.sequence_regressions
                  << " unavailable_events=" << region.unavailable_events
                  << " recoveries=" << region.recoveries
                  << " recovery_timeouts=" << region.recovery_timeouts << '\n'
                  << "  quarantined_slots=" << bus_stats.quarantined_slots
                  << " active_consumers=" << bus_stats.active_consumers
                  << " device_lost=" << (device_loss.observed ? 1 : 0)
                  << " producer_reason=" << hr_text(device_loss.producer_reason)
                  << " consumer_reason=" << hr_text(device_loss.consumer_reason)
                  << '\n';

        if (options.expect_device_lost) {
            if (!device_loss.observed) {
                fail("--expect-device-lost was set, but no external device loss was observed");
            }
            if (bus_stats.active_consumers != 0
                && !FAILED(device_loss.producer_reason)) {
                fail("a SharedFrameBus consumer remained registered after device loss");
            }
            if (bus_stats.active_consumers != 0) {
                std::cout << "  producer-device-loss terminal accounting retained "
                          << bus_stats.active_consumers
                          << " inactive consumer record(s) until publisher destruction\n";
            }
            if (!cleanup_warnings.empty()) {
                std::cout << "  device-loss cleanup warnings="
                          << cleanup_warnings << '\n';
            }
            std::cout << "[PASS] expected external device loss observed; no TDR was induced\n";
            return 0;
        }
        if (wgc_stats.published_frames == 0 || consumer_metrics.frames == 0) {
            fail("soak produced no usable direct-bus frames");
        }
        if (packet_count == 0 || packets.bytes.load(std::memory_order_relaxed) == 0) {
            fail("hardware encoder emitted no packets");
        }
        if (consumer_metrics.sequence_regressions != 0
            || churn_metrics.sequence_regressions != 0
            || packets.timestamp_regressions.load(std::memory_order_relaxed) != 0) {
            fail("sequence or packet timestamp regression detected");
        }
        if (bus_stats.quarantined_slots != 0) {
            fail("SharedFrameBus slot quarantine detected");
        }
        if (!skip_counters_consistent) {
            fail("WGC no-buffer and SharedFrameBus no-slot counters are inconsistent");
        }
        if (bus_stats.no_slot != 0) {
            fail("SharedFrameBus direct publication encountered slot starvation");
        }
        if (control_contention > control_contention_limit) {
            fail("WGC direct publication exceeded the control-contention drop budget");
        }
        if (bus_stats.active_consumers != 0) {
            fail("a SharedFrameBus consumer remained registered after shutdown");
        }
        if (options.profile == Profile::churn) {
            if (churn_metrics.registrations < 2
                || churn_metrics.unregistrations < 2) {
                fail("churn profile completed too few consumer lifecycles");
            }
            if (churn_metrics.registrations != churn_metrics.unregistrations
                || churn_metrics.successful_lifecycles
                    != churn_metrics.unregistrations) {
                fail("not every churn consumer lifecycle consumed a frame and closed cleanly");
            }
            if (selected_monitor == nullptr
                && (region.unavailable_events == 0 || region.recoveries == 0)) {
                fail("churn window did not complete an unavailable/recovery cycle");
            }
        }
        if (FAILED(producer_device.device->GetDeviceRemovedReason())
            || FAILED(consumer_device.device->GetDeviceRemovedReason())) {
            fail("device removal was detected during final validation");
        }

        std::cout << "[PASS] SharedFrameBus production soak completed\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "SharedFrameBus soak failed: "
                  << winrt::to_string(error.message())
                  << " (hr=" << hr_text(error.code()) << ")\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "SharedFrameBus soak failed: " << error.what() << '\n';
        return 1;
    }
}
