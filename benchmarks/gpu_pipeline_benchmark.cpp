#include <fluxcap/gpu.hpp>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <windows.h>
#include <wrl/client.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
namespace gpu = fluxcap::gpu;

constexpr wchar_t kWindowClass[] = L"FluxCapGpuBenchmarkWindow";
constexpr UINT kStopMessage = WM_APP + 1;
constexpr std::size_t kDefaultFrames = 300;
constexpr std::size_t kDefaultWarmup = 30;
constexpr std::size_t kMaximumFrames = 1'000'000;

struct Options final {
    std::size_t frames = kDefaultFrames;
    std::size_t warmup = kDefaultWarmup;
    std::uint32_t media_fps = 240;
    std::uint32_t mailbox_size = 0;
    std::uint32_t post_crop_size = 0;
    bool show_help = false;
    bool source_only = false;
};

struct WindowState final {
    HANDLE ready = nullptr;
    std::atomic<HWND> window{nullptr};
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<std::uint32_t> phase{0};
};

class D3DWindowSource final {
public:
    HRESULT initialize(HWND window) noexcept {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL created_level{};
        constexpr UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            device_flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device_,
            &created_level,
            &context_);
        if (result == E_INVALIDARG) {
            result = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                device_flags,
                levels + 1,
                1,
                D3D11_SDK_VERSION,
                &device_,
                &created_level,
                &context_);
        }
        if (FAILED(result)) return result;

        ComPtr<IDXGIDevice1> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        result = device_.As(&dxgi_device);
        if (FAILED(result)) return result;
        result = dxgi_device->GetAdapter(&adapter);
        if (FAILED(result)) return result;
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(result)) return result;
        (void)dxgi_device->SetMaximumFrameLatency(1);

        RECT client{};
        if (!GetClientRect(window, &client)) return HRESULT_FROM_WIN32(GetLastError());
        const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
        const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        ComPtr<IDXGISwapChain1> swap_chain1;
        result = factory->CreateSwapChainForHwnd(
            device_.Get(), window, &description, nullptr, nullptr, &swap_chain1);
        if (FAILED(result)) {
            description.Flags = 0;
            result = factory->CreateSwapChainForHwnd(
                device_.Get(), window, &description, nullptr, nullptr, &swap_chain1);
        }
        if (FAILED(result)) return result;
        result = swap_chain1.As(&swap_chain_);
        if (FAILED(result)) return result;
        (void)factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        (void)swap_chain_->SetMaximumFrameLatency(1);

        ComPtr<ID3D11Texture2D> back_buffer;
        result = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        if (FAILED(result)) return result;
        return device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &render_target_);
    }

    HRESULT present(std::uint32_t phase) noexcept {
        // One 24-bit frame id per presentation keeps adjacent and long-run frames distinct.
        const std::uint32_t frame_id = phase & 0x00ff'ffffu;
        const float color[] = {
            static_cast<float>(frame_id & 0xffu) / 255.0F,
            static_cast<float>((frame_id >> 8) & 0xffu) / 255.0F,
            static_cast<float>((frame_id >> 16) & 0xffu) / 255.0F,
            1.0F};
        context_->ClearRenderTargetView(render_target_.Get(), color);
        return swap_chain_->Present(1, 0);
    }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain2> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_;
};

struct Summary final {
    double minimum_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double mean_ms = 0.0;
};

struct PacketTimings final {
    std::vector<Clock::time_point> submitted;
    std::vector<double> latency_ms;
    std::size_t packet_count = 0;
    std::size_t packet_bytes = 0;
    std::int64_t frame_duration = 0;
};

class WindowThread final {
public:
    WindowThread() {
        state_.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (state_.ready == nullptr) {
            throw std::runtime_error("CreateEvent failed");
        }
        thread_ = std::thread(&WindowThread::run, &state_);
        if (WaitForSingleObject(state_.ready, 5'000) != WAIT_OBJECT_0) {
            stop();
            throw std::runtime_error("benchmark window creation timed out");
        }
        if (state_.error.load(std::memory_order_acquire) != ERROR_SUCCESS
            || state_.window.load(std::memory_order_acquire) == nullptr) {
            const DWORD error = state_.error.load(std::memory_order_relaxed);
            stop();
            throw std::runtime_error(
                "benchmark window creation failed: " + std::to_string(error));
        }
    }

    ~WindowThread() { stop(); }
    WindowThread(const WindowThread&) = delete;
    WindowThread& operator=(const WindowThread&) = delete;

    [[nodiscard]] HWND window() const noexcept {
        return state_.window.load(std::memory_order_acquire);
    }

private:
    static LRESULT CALLBACK window_proc(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
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
            state->error.store(GetLastError(), std::memory_order_release);
            SetEvent(state->ready);
            return;
        }

        RECT bounds{0, 0, 1280, 720};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0);
        HWND window = CreateWindowExW(
            0,
            kWindowClass,
            L"FluxCap GPU pipeline benchmark",
            WS_OVERLAPPEDWINDOW,
            64,
            64,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            nullptr,
            nullptr,
            window_class.hInstance,
            nullptr);
        if (window == nullptr) {
            state->error.store(GetLastError(), std::memory_order_release);
            SetEvent(state->ready);
            return;
        }
        state->window.store(window, std::memory_order_release);
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);

        D3DWindowSource source;
        HRESULT result = source.initialize(window);
        if (FAILED(result)) {
            state->error.store(static_cast<DWORD>(result), std::memory_order_release);
            DestroyWindow(window);
            SetEvent(state->ready);
            return;
        }
        const std::uint32_t first_phase =
            state->phase.fetch_add(1, std::memory_order_relaxed) + 1;
        result = source.present(first_phase);
        if (FAILED(result)) {
            state->error.store(static_cast<DWORD>(result), std::memory_order_release);
            DestroyWindow(window);
            SetEvent(state->ready);
            return;
        }
        SetEvent(state->ready);

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

            const std::uint32_t phase =
                state->phase.fetch_add(1, std::memory_order_relaxed) + 1;
            result = source.present(phase);
            if (result == DXGI_STATUS_OCCLUDED) {
                Sleep(16);
            } else if (FAILED(result)) {
                state->error.store(static_cast<DWORD>(result), std::memory_order_release);
                DestroyWindow(window);
            }
        }
        state->window.store(nullptr, std::memory_order_release);
    }

    void stop() noexcept {
        HWND window = state_.window.load(std::memory_order_acquire);
        if (window != nullptr) {
            PostMessageW(window, kStopMessage, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (state_.ready != nullptr) {
            CloseHandle(state_.ready);
            state_.ready = nullptr;
        }
    }

    WindowState state_{};
    std::thread thread_;
};

std::size_t parse_count(std::wstring_view text, const char* name) {
    if (text.empty()) {
        throw std::invalid_argument(std::string(name) + " requires a value");
    }
    std::size_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            throw std::invalid_argument(std::string(name) + " must be an integer");
        }
        const std::size_t digit = static_cast<std::size_t>(character - L'0');
        if (value > (kMaximumFrames - digit) / 10u) {
            throw std::invalid_argument(std::string(name) + " is too large");
        }
        value = value * 10u + digit;
    }
    return value;
}

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.show_help = true;
        } else if (argument == L"--source-only") {
            options.source_only = true;
        } else if (argument == L"--mailbox-size") {
            if (++index >= argc) {
                throw std::invalid_argument("missing option value");
            }
            options.mailbox_size = static_cast<std::uint32_t>(
                parse_count(argv[index], "--mailbox-size"));
        } else if (argument.starts_with(L"--mailbox-size=")) {
            options.mailbox_size = static_cast<std::uint32_t>(
                parse_count(argument.substr(15), "--mailbox-size"));
        } else if (argument == L"--post-crop-size") {
            if (++index >= argc) {
                throw std::invalid_argument("missing option value");
            }
            options.post_crop_size = static_cast<std::uint32_t>(
                parse_count(argv[index], "--post-crop-size"));
        } else if (argument.starts_with(L"--post-crop-size=")) {
            options.post_crop_size = static_cast<std::uint32_t>(
                parse_count(argument.substr(17), "--post-crop-size"));
        } else if (argument == L"--media-fps") {
            if (++index >= argc) {
                throw std::invalid_argument("missing option value");
            }
            options.media_fps = static_cast<std::uint32_t>(
                parse_count(argv[index], "--media-fps"));
        } else if (argument.starts_with(L"--media-fps=")) {
            options.media_fps = static_cast<std::uint32_t>(
                parse_count(argument.substr(12), "--media-fps"));
        } else if (argument == L"--frames" || argument == L"--warmup") {
            if (++index >= argc) {
                throw std::invalid_argument("missing option value");
            }
            const std::size_t value = parse_count(
                argv[index], argument == L"--frames" ? "--frames" : "--warmup");
            if (argument == L"--frames") options.frames = value;
            else options.warmup = value;
        } else if (argument.starts_with(L"--frames=")) {
            options.frames = parse_count(argument.substr(9), "--frames");
        } else if (argument.starts_with(L"--warmup=")) {
            options.warmup = parse_count(argument.substr(9), "--warmup");
        } else {
            throw std::invalid_argument("unknown option");
        }
    }
    if (options.frames == 0) {
        throw std::invalid_argument("--frames must be greater than zero");
    }
    if (options.media_fps == 0 || options.media_fps > 1'000) {
        throw std::invalid_argument("--media-fps must be between 1 and 1000");
    }
    if (options.mailbox_size == 1 || options.mailbox_size > 16'384) {
        throw std::invalid_argument(
            "--mailbox-size must be between 2 and 16384");
    }
    if (options.post_crop_size == 1 || options.post_crop_size > 16'384) {
        throw std::invalid_argument(
            "--post-crop-size must be between 2 and 16384");
    }
    if (options.mailbox_size != 0 && options.post_crop_size != 0) {
        throw std::invalid_argument(
            "--mailbox-size and --post-crop-size are mutually exclusive");
    }
    return options;
}

double milliseconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double seconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double>(duration).count();
}

double percentile(const std::vector<double>& sorted, double fraction) {
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[rank == 0 ? 0 : rank - 1];
}

Summary summarize(std::vector<double> values) {
    if (values.empty()) {
        throw std::runtime_error("a benchmark metric produced no samples");
    }
    const double total = std::accumulate(values.begin(), values.end(), 0.0);
    std::sort(values.begin(), values.end());
    return {
        values.front(),
        percentile(values, 0.50),
        percentile(values, 0.95),
        percentile(values, 0.99),
        total / static_cast<double>(values.size())};
}

void print_summary(std::string_view name, const std::vector<double>& samples) {
    const Summary summary = summarize(samples);
    std::cout << std::left << std::setw(27) << name
              << std::right << std::setw(11) << summary.minimum_ms
              << std::setw(11) << summary.p50_ms
              << std::setw(11) << summary.p95_ms
              << std::setw(11) << summary.p99_ms
              << std::setw(11) << summary.mean_ms << '\n';
}

void collect_packet(void* opaque, const gpu::EncodedPacket& packet) {
    auto& timings = *static_cast<PacketTimings*>(opaque);
    ++timings.packet_count;
    timings.packet_bytes += packet.size;
    if (timings.frame_duration <= 0 || packet.timestamp_100ns < 0) return;
    const std::size_t index = static_cast<std::size_t>(
        packet.timestamp_100ns / timings.frame_duration);
    if (index >= timings.submitted.size()
        || timings.submitted[index] == Clock::time_point{}
        || timings.latency_ms[index] >= 0.0) {
        return;
    }
    timings.latency_ms[index] = milliseconds(Clock::now() - timings.submitted[index]);
}

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.show_help) {
            std::cout
                << "Usage: fluxcap_gpu_bench [--frames N] [--warmup N] "
                   "[--media-fps N] [--mailbox-size N|--post-crop-size N] "
                   "[--source-only]\n";
            return 0;
        }
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        WindowThread benchmark_window;
        if (options.source_only) {
            std::cout
                << "FluxCap display-vsync D3D source is running; close its window to stop.\n";
            while (benchmark_window.window() != nullptr) Sleep(100);
            return 0;
        }

        gpu::WgcCapture capture;
        gpu::WgcCaptureOptions capture_options;
        capture_options.buffer_count = 3;
        capture_options.include_cursor = false;
        capture_options.min_update_interval_us = 1'000;
        gpu::WgcResult created;
        if (options.mailbox_size != 0) {
            gpu::WgcMailboxConfig mailbox;
            mailbox.mode = gpu::WgcMailboxMode::centered_region;
            mailbox.width = options.mailbox_size;
            mailbox.height = options.mailbox_size;
            created = gpu::WgcCapture::create_for_window(
                benchmark_window.window(),
                nullptr,
                capture_options,
                mailbox,
                capture);
        } else {
            created = gpu::WgcCapture::create_for_window(
                benchmark_window.window(), nullptr, capture_options, capture);
        }
        if (!created) fail("WGC create failed: " + created.message);
        const auto started = capture.start();
        if (!started) fail("WGC start failed: " + started.message);

        gpu::WgcFrameLease first;
        const auto first_result = capture.acquire_latest(5'000, first);
        if (!first_result) fail("first WGC frame failed: " + first_result.message);
        const std::uint32_t capture_width = first.info().width;
        const std::uint32_t capture_height = first.info().height;
        const auto first_mailbox = first.mailbox_info();
        if (first_mailbox.generation == 0
            || (options.mailbox_size != 0
                && (capture_width != options.mailbox_size
                    || capture_height != options.mailbox_size
                    || first_mailbox.width != options.mailbox_size
                    || first_mailbox.height != options.mailbox_size))) {
            fail("WGC mailbox returned an inconsistent first frame");
        }
        const std::uint32_t input_width = options.post_crop_size == 0
            ? capture_width : options.post_crop_size;
        const std::uint32_t input_height = options.post_crop_size == 0
            ? capture_height : options.post_crop_size;
        if (input_width > capture_width || input_height > capture_height) {
            fail("post-crop size exceeds the WGC source dimensions");
        }
        const std::uint32_t output_width = input_width & ~1u;
        const std::uint32_t output_height = input_height & ~1u;
        if (output_width == 0 || output_height == 0) {
            fail("capture target is too small for NV12");
        }

        gpu::GpuTransformConfig transform_config;
        transform_config.input_width = input_width;
        transform_config.input_height = input_height;
        transform_config.output_width = output_width;
        transform_config.output_height = output_height;
        transform_config.output_format = gpu::GpuPixelFormat::nv12;
        transform_config.frame_rate_numerator = options.media_fps;
        gpu::GpuTransform transform;
        const auto transform_created = gpu::GpuTransform::create(
            capture.device(), transform_config, transform);
        if (!transform_created) {
            fail(std::string("NV12 transform create failed: ") + transform_created.what());
        }
        gpu::GpuCrop post_crop;
        if (options.post_crop_size != 0) {
            gpu::GpuCropConfig crop_config;
            crop_config.input_width = capture_width;
            crop_config.input_height = capture_height;
            crop_config.x = (capture_width - input_width) / 2u;
            crop_config.y = (capture_height - input_height) / 2u;
            crop_config.width = input_width;
            crop_config.height = input_height;
            crop_config.bind_flags = D3D11_BIND_SHADER_RESOURCE
                | D3D11_BIND_RENDER_TARGET;
            const auto crop_created = gpu::GpuCrop::create(
                capture.device(), crop_config, post_crop);
            if (!crop_created) {
                fail(std::string("post-crop create failed: ") + crop_created.what());
            }
        }
        first.reset();

        gpu::GpuEncoderConfig encoder_config;
        encoder_config.codec = gpu::VideoCodec::h264;
        encoder_config.width = output_width;
        encoder_config.height = output_height;
        encoder_config.frame_rate_numerator = options.media_fps;
        encoder_config.frame_rate_denominator = 1;
        encoder_config.bitrate = 8'000'000;
        encoder_config.gop_size = 120;
        encoder_config.input_format = DXGI_FORMAT_NV12;
        encoder_config.low_latency = true;
        gpu::GpuEncoderSupport support;
        const auto probed = gpu::GpuEncoder::probe(capture.device(), encoder_config, support);
        if (!probed || !support.supported || !support.d3d11_aware) {
            fail("no D3D11-aware H.264 hardware encoder is available");
        }

        const std::size_t total_frames = options.warmup + options.frames;
        PacketTimings packet_timings;
        packet_timings.frame_duration = 10'000'000 / options.media_fps;
        packet_timings.submitted.resize(total_frames);
        packet_timings.latency_ms.assign(total_frames, -1.0);
        gpu::GpuEncoder encoder;
        const auto initialized = encoder.initialize(
            capture.device(), encoder_config, &collect_packet, &packet_timings);
        if (!initialized) fail("H.264 encoder initialize failed: " + initialized.message);

        std::vector<double> acquire_wait;
        std::vector<double> mailbox_age;
        std::vector<double> crop_submit;
        std::vector<double> transform_submit;
        std::vector<double> encode_call;
        std::vector<double> pipeline_call;
        acquire_wait.reserve(options.frames);
        mailbox_age.reserve(options.frames);
        if (options.post_crop_size != 0) crop_submit.reserve(options.frames);
        transform_submit.reserve(options.frames);
        encode_call.reserve(options.frames);
        pipeline_call.reserve(options.frames);

        std::uint64_t previous_sequence = 0;
        std::int64_t previous_source_timestamp = 0;
        std::uint64_t sequence_gaps = 0;
        Clock::time_point measured_begin{};
        Clock::time_point measured_end{};

        for (std::size_t index = 0; index < total_frames; ++index) {
            const auto pipeline_begin = Clock::now();
            if (index == options.warmup) measured_begin = pipeline_begin;
            gpu::WgcFrameLease frame;
            const auto acquire_begin = Clock::now();
            const auto acquired = capture.acquire_latest(5'000, frame);
            const auto acquire_end = Clock::now();
            if (!acquired) fail("WGC acquire failed: " + acquired.message);
            if (frame.info().width != capture_width
                || frame.info().height != capture_height) {
                fail("benchmark window changed size during the run");
            }
            if (previous_sequence != 0) {
                if (frame.info().sequence <= previous_sequence) {
                    fail("WGC publication sequence did not increase");
                }
                sequence_gaps += frame.info().sequence - previous_sequence - 1;
            }
            if (previous_source_timestamp != 0
                && frame.info().source_timestamp_100ns <= previous_source_timestamp) {
                fail("WGC source presentation timestamp did not increase");
            }
            previous_sequence = frame.info().sequence;
            previous_source_timestamp = frame.info().source_timestamp_100ns;

            LARGE_INTEGER qpc_now{};
            QueryPerformanceCounter(&qpc_now);
            const double age_ms = frame.info().qpc_frequency == 0
                ? 0.0
                : static_cast<double>(
                    static_cast<std::uint64_t>(qpc_now.QuadPart) - frame.info().timestamp_qpc)
                    * 1000.0 / static_cast<double>(frame.info().qpc_frequency);

            ID3D11Texture2D* transform_input = frame.texture();
            const auto crop_begin = Clock::now();
            if (options.post_crop_size != 0) {
                const auto cropped = post_crop.process(transform_input);
                if (!cropped) {
                    fail(std::string("post-crop failed: ") + cropped.what());
                }
                transform_input = post_crop.output_texture();
            }
            const auto crop_end = Clock::now();
            const auto transform_begin = crop_end;
            const auto transformed = transform.process(transform_input);
            const auto transform_end = Clock::now();
            if (!transformed) {
                fail(std::string("NV12 transform failed: ") + transformed.what());
            }

            packet_timings.submitted[index] = Clock::now();
            const auto encoded = encoder.encode_texture(
                transform.output_texture(),
                static_cast<std::int64_t>(index) * packet_timings.frame_duration,
                packet_timings.frame_duration,
                index == 0);
            const auto encode_end = Clock::now();
            if (!encoded) fail("H.264 encode failed: " + encoded.message);
            frame.reset();

            if (index >= options.warmup) {
                acquire_wait.push_back(milliseconds(acquire_end - acquire_begin));
                mailbox_age.push_back(age_ms);
                if (options.post_crop_size != 0) {
                    crop_submit.push_back(milliseconds(crop_end - crop_begin));
                }
                transform_submit.push_back(milliseconds(transform_end - transform_begin));
                encode_call.push_back(milliseconds(encode_end - packet_timings.submitted[index]));
                pipeline_call.push_back(milliseconds(encode_end - pipeline_begin));
            }
            measured_end = encode_end;
        }
        // Freeze capture counters at the end of the presentation-throughput
        // window; draining the encoder can continue receiving WGC frames.
        const auto stats = capture.stats();
        const auto drained = encoder.drain();
        if (!drained) fail("H.264 drain failed: " + drained.message);

        std::vector<double> packet_latency;
        packet_latency.reserve(options.frames);
        for (std::size_t index = options.warmup; index < total_frames; ++index) {
            if (packet_timings.latency_ms[index] >= 0.0) {
                packet_latency.push_back(packet_timings.latency_ms[index]);
            }
        }
        if (packet_latency.size() != options.frames) {
            fail(
                "measured packet latency coverage is "
                + std::to_string(packet_latency.size()) + '/'
                + std::to_string(options.frames)
                + "; every measured submission must produce a timed packet");
        }

        std::cout << "FluxCap GPU pipeline benchmark\n"
                  << "Target: animated WGC window, " << input_width << 'x'
                  << input_height << " BGRA -> " << output_width << 'x'
                  << output_height << " NV12 -> H.264\n"
                  << "WGC source surface: " << first_mailbox.source_width << 'x'
                  << first_mailbox.source_height << '\n'
                  << "Capture mailbox: "
                  << (options.mailbox_size != 0
                        ? "fused centered ROI"
                        : options.post_crop_size != 0
                            ? "full frame + centered post-crop"
                            : "full frame")
                  << (options.mailbox_size != 0
                        ? " " + std::to_string(options.mailbox_size) + "x"
                            + std::to_string(options.mailbox_size)
                        : options.post_crop_size != 0
                            ? " " + std::to_string(options.post_crop_size) + "x"
                                + std::to_string(options.post_crop_size)
                            : std::string{})
                  << '\n'
                  << "Frames: " << options.frames << ", warmup: " << options.warmup
                  << ", media timeline: " << options.media_fps << " fps"
                  << ", packets: " << packet_timings.packet_count
                  << ", bytes: " << packet_timings.packet_bytes << '\n'
                  << "Measured packet latency coverage: " << packet_latency.size()
                  << '/' << options.frames << "\n\n"
                  << std::fixed << std::setprecision(3)
                  << std::left << std::setw(27) << "Metric"
                  << std::right << std::setw(11) << "min ms"
                  << std::setw(11) << "p50 ms"
                  << std::setw(11) << "p95 ms"
                  << std::setw(11) << "p99 ms"
                  << std::setw(11) << "mean ms" << '\n';
        print_summary("acquire latest wait", acquire_wait);
        print_summary("captured mailbox age", mailbox_age);
        if (!crop_submit.empty()) {
            print_summary("post-crop CPU submission", crop_submit);
        }
        print_summary("transform CPU submission", transform_submit);
        print_summary("encoder call", encode_call);
        print_summary("submit to encoded packet", packet_latency);
        print_summary("acquire+process+encode", pipeline_call);

        const double measured_seconds = seconds(measured_end - measured_begin);
        std::cout << "\nCapture stats: received=" << stats.received_frames
                  << ", published=" << stats.published_frames
                  << ", overwritten=" << stats.overwritten_frames
                  << ", source_dropped=" << stats.dropped_at_source
                  << ", no_buffer=" << stats.skipped_no_buffer
                  << ", sequence_gaps=" << sequence_gaps << '\n'
                  << "Consumed unique WGC presentations: "
                  << (static_cast<double>(options.frames) / measured_seconds)
                  << " frames/s\n"
                  << "Timing note: crop/transform are CPU submission times; mailbox age "
                     "starts after FluxCap queues the WGC mailbox copy.\n";
        capture.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU benchmark failed: " << error.what() << '\n'
                  << "Usage: fluxcap_gpu_bench [--frames N] [--warmup N] "
                     "[--media-fps N] [--mailbox-size N|--post-crop-size N] "
                     "[--source-only]\n";
        return 1;
    }
}
