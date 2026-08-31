// Runtime-L2 copy-evidence workload for the Desktop Duplication resilience
// paths: plain DD identity, DD with GPU cursor compositing, the multi-monitor
// controller, and the GDI fallback. One trace per invocation; with
// --runtime-evidence-json a schema-v2 JSON artifact is emitted and the
// process exit code reflects the trace gates.
#include <fluxcap/gpu.hpp>

#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#pragma comment(lib, "bcrypt.lib")

namespace {

namespace gpu = fluxcap::gpu;
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kRoiSize = 320;

[[noreturn]] void fail(std::string message) {
    std::fprintf(stderr, "[FAIL] %s\n", message.c_str());
    std::exit(1);
}

enum class TraceKind {
    dd_identity,
    dd_baked_cursor,
    dd_controller,
    gdi,
};

struct Options final {
    TraceKind trace = TraceKind::dd_identity;
    std::optional<std::wstring> runtime_evidence_json;
    std::string qualification_run_nonce;
    std::uint32_t duration_ms = 5'000;
    std::uint32_t warmup_ms = 1'000;
};

bool parse_trace(std::wstring_view value, TraceKind& trace) {
    if (value == L"dd-identity") {
        trace = TraceKind::dd_identity;
        return true;
    }
    if (value == L"dd-baked-cursor") {
        trace = TraceKind::dd_baked_cursor;
        return true;
    }
    if (value == L"dd-controller") {
        trace = TraceKind::dd_controller;
        return true;
    }
    if (value == L"gdi") {
        trace = TraceKind::gdi;
        return true;
    }
    return false;
}

const char* trace_name(TraceKind trace) noexcept {
    switch (trace) {
    case TraceKind::dd_identity: return "dd-identity";
    case TraceKind::dd_baked_cursor: return "dd-baked-cursor";
    case TraceKind::dd_controller: return "dd-controller";
    case TraceKind::gdi: return "gdi";
    default: return "unknown";
    }
}

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    bool trace_seen = false;
    const auto next_value = [&](int& index,
                                std::string_view option) -> std::wstring {
        if (index + 1 >= argc) {
            fail(std::string(option) + " requires a value");
        }
        return std::wstring(argv[++index]);
    };
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--trace") {
            if (trace_seen
                || !parse_trace(next_value(index, "--trace"), options.trace)) {
                fail("--trace requires exactly one known trace name");
            }
            trace_seen = true;
        } else if (argument.starts_with(L"--trace=")) {
            if (trace_seen) {
                fail("--trace may be specified only once");
            }
            const std::wstring value(
                argument.substr(std::wstring_view(L"--trace=").size()));
            if (!parse_trace(value, options.trace)) {
                fail("--trace requires a known trace name");
            }
            trace_seen = true;
        } else if (argument == L"--runtime-evidence-json"
                   || argument.starts_with(L"--runtime-evidence-json=")) {
            if (options.runtime_evidence_json.has_value()) {
                fail("--runtime-evidence-json may be specified only once");
            }
            std::wstring value =
                argument.starts_with(L"--runtime-evidence-json=")
                    ? std::wstring(argument.substr(
                          std::wstring_view(L"--runtime-evidence-json=")
                              .size()))
                    : next_value(index, "--runtime-evidence-json");
            if (value.empty()) {
                fail("--runtime-evidence-json requires a nonempty path");
            }
            options.runtime_evidence_json.emplace(std::move(value));
        } else if ((argument == L"--duration-ms"
                        && ++index < argc)
                   || argument.starts_with(L"--duration-ms=")) {
            const std::wstring value =
                argument.starts_with(L"--duration-ms=")
                    ? std::wstring(
                          argument.substr(
                              std::wstring_view(L"--duration-ms=").size()))
                    : std::wstring(argv[index]);
            options.duration_ms = static_cast<std::uint32_t>(
                std::wcstol(value.c_str(), nullptr, 10));
            if (options.duration_ms < 1'000
                || options.duration_ms > 600'000) {
                fail("--duration-ms must be between 1000 and 600000");
            }
        } else if ((argument == L"--warmup-ms" && ++index < argc)
                   || argument.starts_with(L"--warmup-ms=")) {
            const std::wstring value =
                argument.starts_with(L"--warmup-ms=")
                    ? std::wstring(
                          argument.substr(
                              std::wstring_view(L"--warmup-ms=").size()))
                    : std::wstring(argv[index]);
            options.warmup_ms = static_cast<std::uint32_t>(
                std::wcstol(value.c_str(), nullptr, 10));
        } else if (argument == L"--qualification-run-nonce"
                   || argument.starts_with(L"--qualification-run-nonce=")) {
            const std::wstring value =
                argument.starts_with(L"--qualification-run-nonce=")
                    ? std::wstring(argument.substr(
                          std::wstring_view(L"--qualification-run-nonce=")
                              .size()))
                    : next_value(index, "--qualification-run-nonce");
            if (value.size() < 16) {
                fail("--qualification-run-nonce requires at least 16 chars");
            }
            options.qualification_run_nonce.assign(
                value.begin(), value.end());
        } else {
            fail("unknown argument; usage: --trace NAME "
                 "[--runtime-evidence-json PATH] "
                 "[--qualification-run-nonce NONCE] "
                 "[--duration-ms N] [--warmup-ms N]");
        }
    }
    if (!trace_seen) fail("--trace is required");
    if (options.runtime_evidence_json.has_value()
        && options.qualification_run_nonce.empty()) {
        fail("--runtime-evidence-json requires --qualification-run-nonce");
    }
    return options;
}

std::wstring running_executable_path() {
    std::wstring buffer(64 * 1024, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        fail("GetModuleFileNameW failed");
    }
    buffer.resize(length);
    return buffer;
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<unsigned>(data[index]);
    }
    return output.str();
}

std::string sha256_file(const std::wstring& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    auto cleanup = [&]() noexcept {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    try {
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status != 0) fail("BCryptOpenAlgorithmProvider failed");
        DWORD object_bytes = 0;
        DWORD copied = 0;
        BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_bytes),
            sizeof(object_bytes),
            &copied,
            0);
        std::vector<std::uint8_t> object(object_bytes ? object_bytes : 1);
        status = BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            static_cast<ULONG>(object.size()),
            nullptr,
            0,
            0);
        if (status != 0) fail("BCryptCreateHash failed");
        file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            fail("opening the running executable for SHA-256 failed");
        }
        std::array<std::uint8_t, 64 * 1024> input{};
        for (;;) {
            DWORD bytes = 0;
            if (!ReadFile(
                    file,
                    input.data(),
                    static_cast<DWORD>(input.size()),
                    &bytes,
                    nullptr)) {
                fail("reading the running executable for SHA-256 failed");
            }
            if (bytes == 0) break;
            BCryptHashData(hash, input.data(), bytes, 0);
        }
        std::array<std::uint8_t, 32> digest{};
        status = BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        if (status != 0) fail("BCryptFinishHash failed");
        cleanup();
        return to_hex(digest.data(), digest.size());
    } catch (...) {
        cleanup();
        throw;
    }
}

std::string sha256_text(std::string_view value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto cleanup = [&]() noexcept {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status != 0) fail("BCryptOpenAlgorithmProvider failed");
    DWORD object_bytes = 0;
    DWORD copied = 0;
    BCryptGetProperty(
        algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_bytes),
        sizeof(object_bytes),
        &copied,
        0);
    std::vector<std::uint8_t> object(object_bytes ? object_bytes : 1);
    status = BCryptCreateHash(
        algorithm,
        &hash,
        object.data(),
        static_cast<ULONG>(object.size()),
        nullptr,
        0,
        0);
    if (status != 0) fail("BCryptCreateHash failed");
    BCryptHashData(
        hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
        static_cast<ULONG>(value.size()),
        0);
    std::array<std::uint8_t, 32> digest{};
    status = BCryptFinishHash(
        hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status != 0) fail("BCryptFinishHash failed");
    cleanup();
    return to_hex(digest.data(), digest.size());
}

std::string json_escape(std::string_view value) {
    std::string output;
    for (const char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                std::ostringstream escaped;
                escaped << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0')
                        << static_cast<unsigned>(character);
                output += escaped.str();
            } else {
                output += character;
            }
            break;
        }
    }
    return output;
}

// Deterministic animated source: a topmost popup window repainted by an 8 ms
// timer so every Desktop Duplication/GDI trace has continuous desktop
// content. Presentation counting is informational, not a gate.
struct SourceWindowState final {
    std::atomic<HWND> window{nullptr};
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<std::uint64_t> presentations{0};
    HANDLE ready = nullptr;
    std::jthread thread;
};

constexpr UINT kStopMessage = WM_APP + 1;
constexpr wchar_t kSourceClass[] = L"FluxCapDdResilienceSource";

LRESULT CALLBACK source_window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* state = reinterpret_cast<SourceWindowState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_TIMER: {
        if (state != nullptr) {
            state->presentations.fetch_add(1, std::memory_order_relaxed);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const std::uint64_t phase =
            state != nullptr
                ? state->presentations.load(std::memory_order_relaxed)
                : 0;
        const std::uint8_t level =
            static_cast<std::uint8_t>((phase * 7u) & 0xffu);
        HBRUSH brush = CreateSolidBrush(
            RGB(level, static_cast<std::uint8_t>(255 - level), 0x40));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    case kStopMessage:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void source_thread(SourceWindowState* state) noexcept {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &source_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kSourceClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassW(&window_class) == 0
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        state->error.store(GetLastError(), std::memory_order_release);
        SetEvent(state->ready);
        return;
    }
    RECT bounds{0, 0, 640, 640};
    AdjustWindowRectEx(
        &bounds, WS_POPUP | WS_VISIBLE, FALSE, WS_EX_TOPMOST);
    HWND window = CreateWindowExW(
        WS_EX_TOPMOST,
        kSourceClass,
        L"FluxCap DD resilience source",
        WS_POPUP | WS_VISIBLE,
        96,
        96,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        nullptr,
        nullptr,
        window_class.hInstance,
        state);
    if (window == nullptr) {
        state->error.store(GetLastError(), std::memory_order_release);
        SetEvent(state->ready);
        return;
    }
    SetWindowLongPtrW(
        window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    state->window.store(window, std::memory_order_release);
    SetTimer(window, 1, 8, nullptr);
    SetEvent(state->ready);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    state->window.store(nullptr, std::memory_order_release);
}

struct ConsumerStats final {
    std::uint64_t frames = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t monotonic_violations = 0;
    std::uint64_t invalid_damage_frames = 0;
};

struct VirtualDesktop final {
    RECT bounds{0, 0, 0, 0};
    bool known = false;
};

VirtualDesktop query_virtual_desktop(ID3D11Device* device) {
    VirtualDesktop desktop;
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_description{};
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))
        || FAILED(dxgi_device->GetAdapter(&adapter))
        || FAILED(adapter->GetDesc(&adapter_description))
        || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return desktop;
    }
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(adapter_index, &candidate)
            == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC description{};
        if (FAILED(candidate->GetDesc(&description))
            || description.AdapterLuid.LowPart
                != adapter_description.AdapterLuid.LowPart
            || description.AdapterLuid.HighPart
                != adapter_description.AdapterLuid.HighPart) {
            continue;
        }
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            if (candidate->EnumOutputs(output_index, &output)
                == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC output_description{};
            if (FAILED(output->GetDesc(&output_description))
                || !output_description.AttachedToDesktop) {
                continue;
            }
            if (!desktop.known) {
                desktop.bounds = output_description.DesktopCoordinates;
                desktop.known = true;
            } else {
                desktop.bounds.left = std::min(
                    desktop.bounds.left,
                    output_description.DesktopCoordinates.left);
                desktop.bounds.top = std::min(
                    desktop.bounds.top,
                    output_description.DesktopCoordinates.top);
                desktop.bounds.right = std::max(
                    desktop.bounds.right,
                    output_description.DesktopCoordinates.right);
                desktop.bounds.bottom = std::max(
                    desktop.bounds.bottom,
                    output_description.DesktopCoordinates.bottom);
            }
        }
    }
    return desktop;
}

std::string adapter_luid_text(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))
        || FAILED(dxgi_device->GetAdapter(&adapter))
        || FAILED(adapter->GetDesc(&description))) {
        fail("resolving the adapter LUID failed");
    }
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(8) << std::setfill('0')
           << description.AdapterLuid.HighPart << ":0x" << std::setw(8)
           << description.AdapterLuid.LowPart;
    return output.str();
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    (void)SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Options options = parse_options(argc, argv);

    SourceWindowState source;
    source.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (source.ready == nullptr) fail("CreateEvent failed");
    source.thread = std::jthread(source_thread, &source);
    if (WaitForSingleObject(source.ready, 5'000) != WAIT_OBJECT_0
        || source.error.load(std::memory_order_acquire) != ERROR_SUCCESS
        || source.window.load(std::memory_order_acquire) == nullptr) {
        fail("source window creation failed");
    }
    const HWND window = source.window.load(std::memory_order_acquire);
    const HMONITOR monitor =
        MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &feature_level,
        &context);
    if (FAILED(hr)) {
        fail("D3D11CreateDevice failed: " + std::to_string(hr));
    }
    const std::string luid_text = adapter_luid_text(device.Get());

    // Resolve the 320x320 ROI over the source window's client area.
    MONITORINFO monitor_info{sizeof(monitor_info)};
    RECT client_rect{};
    POINT client_origin{};
    if (!GetMonitorInfoW(monitor, &monitor_info)
        || !GetClientRect(window, &client_rect)
        || !ClientToScreen(window, &client_origin)) {
        fail("source/monitor geometry query failed");
    }
    gpu::WgcMailboxConfig mailbox;
    mailbox.mode = gpu::WgcMailboxMode::absolute_region;
    mailbox.width = kRoiSize;
    mailbox.height = kRoiSize;
    mailbox.x = static_cast<std::uint32_t>(
                    client_origin.x - monitor_info.rcMonitor.left)
        + (client_rect.right - client_rect.left
            - static_cast<LONG>(kRoiSize)) / 2;
    mailbox.y = static_cast<std::uint32_t>(
                    client_origin.y - monitor_info.rcMonitor.top)
        + (client_rect.bottom - client_rect.top
            - static_cast<LONG>(kRoiSize)) / 2;

    gpu::SharedFrameBusConfig bus_config;
    bus_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bus_config.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bus_config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
    bus_config.slot_count = 3;
    const VirtualDesktop desktop = query_virtual_desktop(device.Get());
    if (options.trace == TraceKind::dd_controller) {
        if (!desktop.known) fail("virtual desktop enumeration failed");
        bus_config.width = static_cast<std::uint32_t>(
            desktop.bounds.right - desktop.bounds.left);
        bus_config.height = static_cast<std::uint32_t>(
            desktop.bounds.bottom - desktop.bounds.top);
    } else {
        bus_config.width = kRoiSize;
        bus_config.height = kRoiSize;
    }

    gpu::SharedFrameBusPublisher publisher;
    const gpu::GpuError bus_created = gpu::SharedFrameBusPublisher::create(
        device.Get(), bus_config, publisher);
    if (!bus_created) {
        fail(std::string("bus create failed: ") + bus_created.what());
    }
    gpu::SharedFrameBusRegistration registration;
    gpu::GpuError result =
        publisher.register_consumer(GetCurrentProcess(), registration);
    if (!result) {
        fail(std::string("consumer registration failed: ") + result.what());
    }
    gpu::SharedFrameBusConsumer consumer;
    result = gpu::SharedFrameBusConsumer::open(
        device.Get(), registration, true, consumer);
    if (!result) {
        fail(std::string("consumer open failed: ") + result.what());
    }

    gpu::WgcCaptureOptions capture_options;
    capture_options.pixel_format = gpu::WgcPixelFormat::bgra8;
    capture_options.include_cursor = false;
    capture_options.damage_mode = gpu::WgcDamageMode::native_report_only;
    capture_options.capture_epoch = 101;
    capture_options.capture_epoch_nonce = 0xddea'bb1eull;
    capture_options.frame_timeout_ms = 20;
    capture_options.access_lost_retry_limit = 0;
    capture_options.idle_republish_interval_ms = 100;
    capture_options.monitor_session_events = true;
    if (options.trace == TraceKind::dd_baked_cursor) {
        capture_options.include_cursor = true;
    }
    if (options.trace == TraceKind::gdi) {
        capture_options.damage_mode = gpu::WgcDamageMode::disabled;
        capture_options.gdi_poll_interval_ms = 8;
        capture_options.monitor_session_events = false;
    }

    std::atomic<bool> draining{true};
    ConsumerStats consumer_stats;
    std::thread consumer_thread([&]() noexcept {
        std::uint64_t previous = 0;
        while (draining.load(std::memory_order_relaxed)) {
            gpu::SharedFrameBusFrameLease frame;
            const gpu::GpuError acquired =
                consumer.acquire_latest(50, frame);
            if (acquired.status == gpu::GpuStatus::timeout) continue;
            if (!acquired) break;
            const std::uint64_t sequence = frame.info().sequence;
            if (consumer_stats.frames != 0 && sequence <= previous) {
                consumer_stats.monotonic_violations += 1;
            }
            if ((frame.side_data().damage.flags
                    & gpu::wgc_damage_valid)
                == 0) {
                consumer_stats.invalid_damage_frames += 1;
            }
            previous = sequence;
            consumer_stats.last_sequence = sequence;
            consumer_stats.frames += 1;
            const gpu::GpuError released = consumer.release(frame);
            if (!released) break;
        }
    });

    gpu::DesktopDuplicationCapture duplication;
    gpu::DesktopDuplicationController controller;
    gpu::GdiMonitorCapture gdi;
    const auto start = std::chrono::steady_clock::now();
    gpu::WgcResult started;
    switch (options.trace) {
    case TraceKind::dd_controller: {
        const gpu::WgcResult created =
            gpu::DesktopDuplicationController::create_for_bus(
                publisher, capture_options, controller);
        if (!created) {
            fail("controller create failed: " + created.message);
        }
        started = controller.start();
        break;
    }
    case TraceKind::gdi: {
        const gpu::WgcResult created =
            gpu::GdiMonitorCapture::create_for_monitor_to_bus(
                monitor, publisher, capture_options, mailbox, gdi);
        if (!created) {
            fail("GDI create failed: " + created.message);
        }
        started = gdi.start();
        break;
    }
    default: {
        const gpu::WgcResult created =
            gpu::DesktopDuplicationCapture::create_for_monitor_to_bus(
                monitor, publisher, capture_options, mailbox, duplication);
        if (!created) {
            fail("duplication create failed: " + created.message);
        }
        started = duplication.start();
        break;
    }
    }
    if (!started) {
        fail(std::string("trace start failed: ") + started.message);
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.warmup_ms));
    const auto measured_start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.duration_ms));
    const auto measured_end = std::chrono::steady_clock::now();

    gpu::WgcCaptureStats capture_stats;
    std::uint32_t monitor_count = 1;
    switch (options.trace) {
    case TraceKind::dd_controller:
        monitor_count = controller.monitor_count();
        controller.stop();
        capture_stats = controller.stats();
        break;
    case TraceKind::gdi:
        gdi.stop();
        capture_stats = gdi.stats();
        break;
    default:
        duplication.stop();
        capture_stats = duplication.stats();
        break;
    }
    draining.store(false);
    consumer_thread.join();
    // Drain the final frame so the consumer's last sequence matches the bus.
    gpu::SharedFrameBusFrameLease drain_frame;
    std::uint64_t drain_sequence = consumer_stats.last_sequence;
    const gpu::GpuError drained = consumer.acquire_latest(500, drain_frame);
    if (drained) {
        drain_sequence = drain_frame.info().sequence;
        (void)consumer.release(drain_frame);
    }
    (void)consumer.close();
    (void)publisher.unregister_consumer(registration, 5'000);
    const gpu::SharedFrameBusStats bus_stats = publisher.stats();
    const auto total_end = std::chrono::steady_clock::now();
    if (source.window.load(std::memory_order_acquire) != nullptr) {
        PostMessageW(window, kStopMessage, 0, 0);
    }
    source.thread.join();
    CloseHandle(source.ready);

    const std::uint64_t measured_duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            measured_end - measured_start)
            .count();
    const std::uint64_t total_duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            total_end - start)
            .count();
    const std::uint64_t source_presentations =
        source.presentations.load(std::memory_order_relaxed);

    // Gates.
    const bool published_gate = capture_stats.published_frames != 0;
    const bool monotonic_gate = consumer_stats.monotonic_violations == 0;
    const bool damage_gate = consumer_stats.invalid_damage_frames == 0;
    const bool coverage_gate = consumer_stats.frames >= 30;
    const bool drain_gate = drain_sequence >= consumer_stats.last_sequence;
    // The idle heartbeat feature keeps a private copy of every committed
    // slot, adding one bus-sized copy per real publish when enabled.
    const std::uint64_t copies_per_publish =
        1 + (capture_options.idle_republish_interval_ms != 0 ? 1 : 0);
    bool copy_contract_gate = true;
    std::string copy_contract_note;
    switch (options.trace) {
    case TraceKind::dd_identity:
        copy_contract_gate =
            capture_stats.ingress_copy_submissions
                + capture_stats.ingress_transform_submissions
            == capture_stats.published_frames * copies_per_publish
            && bus_stats.direct_publishes
                == capture_stats.published_frames;
        copy_contract_note =
            "identity: copies+transforms == published x "
            + std::to_string(copies_per_publish)
            + " (heartbeat capture) and direct bus publishes";
        break;
    case TraceKind::dd_baked_cursor:
        copy_contract_gate =
            capture_stats.ingress_copy_submissions
                + capture_stats.ingress_transform_submissions
            == capture_stats.published_frames
                * (1 + copies_per_publish)
            && capture_stats.cursor_metadata_frames == 0;
        copy_contract_note =
            "baked cursor: one extra full-surface copy per published frame "
            "plus the heartbeat capture copy when enabled";
        break;
    case TraceKind::dd_controller:
        copy_contract_gate =
            capture_stats.ingress_copy_submissions
                >= capture_stats.published_frames
            && monitor_count != 0;
        copy_contract_note =
            "controller: per-monitor compose copies plus one bus copy";
        break;
    case TraceKind::gdi:
        copy_contract_gate =
            capture_stats.ingress_copy_submissions
                == capture_stats.published_frames
            && capture_stats.full_damage_frames
                == capture_stats.published_frames;
        copy_contract_note = "gdi: one staging-to-slot copy per full frame";
        break;
    }
    const bool running_gate =
        capture_stats.received_frames != 0
        || capture_stats.idle_republished_frames != 0;
    const bool passed = published_gate && monotonic_gate && damage_gate
        && coverage_gate && drain_gate && copy_contract_gate && running_gate;

    std::ostringstream stdout_report;
    stdout_report << "trace=" << trace_name(options.trace)
                  << " adapter=" << luid_text
                  << " bus=" << bus_config.width << "x" << bus_config.height
                  << "\n";
    stdout_report << "published=" << capture_stats.published_frames
                  << " received=" << capture_stats.received_frames
                  << " copies=" << capture_stats.ingress_copy_submissions
                  << " transforms="
                  << capture_stats.ingress_transform_submissions
                  << " rebuilds=" << capture_stats.session_rebuilds
                  << " idle=" << capture_stats.idle_republished_frames
                  << " session_events=" << capture_stats.session_events
                  << " no_slot=" << capture_stats.skipped_no_buffer << "\n";
    stdout_report << "consumer frames=" << consumer_stats.frames
                  << " last_sequence=" << consumer_stats.last_sequence
                  << " drain_sequence=" << drain_sequence
                  << " monotonic_violations="
                  << consumer_stats.monotonic_violations
                  << " invalid_damage="
                  << consumer_stats.invalid_damage_frames << "\n";
    stdout_report << "bus direct=" << bus_stats.direct_publishes
                  << " copied=" << bus_stats.copied_publishes << "\n";
    stdout_report << "source presentations=" << source_presentations
                  << " measured_ms=" << measured_duration_ms
                  << " total_ms=" << total_duration_ms << "\n";
    stdout_report << "gates: published=" << published_gate
                  << " monotonic=" << monotonic_gate
                  << " damage=" << damage_gate
                  << " coverage=" << coverage_gate
                  << " drain=" << drain_gate
                  << " copy_contract=" << copy_contract_gate
                  << " running=" << running_gate << "\n";
    stdout_report << "copy contract: " << copy_contract_note << "\n";
    stdout_report << "result: " << (passed ? "PASS" : "FAIL") << "\n";
    const std::string report = stdout_report.str();
    std::fputs(report.c_str(), stdout);

    if (options.runtime_evidence_json.has_value()) {
        const std::string executable_sha256 =
            sha256_file(running_executable_path());
        std::ostringstream tuple;
        tuple << "adapterLuid=" << luid_text
              << "|captureBackend=";
        switch (options.trace) {
        case TraceKind::dd_identity: tuple << "desktop_duplication"; break;
        case TraceKind::dd_baked_cursor:
            tuple << "desktop_duplication_baked_cursor";
            break;
        case TraceKind::dd_controller:
            tuple << "desktop_duplication_controller";
            break;
        case TraceKind::gdi: tuple << "gdi"; break;
        }
        tuple << "|captureTarget="
              << (options.trace == TraceKind::dd_controller
                      ? "virtual_desktop"
                      : "monitor")
              << "|format=BGRA8"
              << "|colorSpace=RGB_FULL_G22_NONE_P709"
              << "|outputWidth=" << bus_config.width
              << "|outputHeight=" << bus_config.height
              << "|idleRepublishIntervalMs="
              << capture_options.idle_republish_interval_ms;
        const std::string tuple_sha256 = sha256_text(tuple.str());

        std::ostringstream json;
        json << std::setfill('0');
        json << "{\n";
        json << "  \"schemaVersion\": 2,\n";
        json << "  \"evidenceLevel\": 2,\n";
        json << "  \"evidenceName\": \"l2_bus_publish_copy_evidence\",\n";
        json << "  \"processId\": " << GetCurrentProcessId() << ",\n";
        json << "  \"executableSha256\": \"" << executable_sha256
             << "\",\n";
        json << "  \"qualificationRunNonce\": \""
             << json_escape(options.qualification_run_nonce) << "\",\n";
        json << "  \"adapterLuid\": \"" << luid_text << "\",\n";
        json << "  \"countersScope\": "
                "\"capture_instance_lifetime_including_warmup\",\n";
        json << "  \"tupleSha256\": \"" << tuple_sha256 << "\",\n";
        json << "  \"qualificationTuple\": {\n";
        json << "    \"adapterLuid\": \"" << luid_text << "\",\n";
        json << "    \"captureBackend\": \"";
        switch (options.trace) {
        case TraceKind::dd_identity:
            json << "desktop_duplication";
            break;
        case TraceKind::dd_baked_cursor:
            json << "desktop_duplication_baked_cursor";
            break;
        case TraceKind::dd_controller:
            json << "desktop_duplication_controller";
            break;
        case TraceKind::gdi: json << "gdi"; break;
        }
        json << "\",\n";
        json << "    \"captureTarget\": "
             << (options.trace == TraceKind::dd_controller
                     ? "\"virtual_desktop\""
                     : "\"monitor\"")
             << ",\n";
        json << "    \"outputWidth\": " << bus_config.width << ",\n";
        json << "    \"outputHeight\": " << bus_config.height << ",\n";
        json << "    \"format\": \"BGRA8\",\n";
        json << "    \"colorSpace\": \"RGB_FULL_G22_NONE_P709\",\n";
        json << "    \"idleRepublishIntervalMs\": "
             << capture_options.idle_republish_interval_ms << "\n";
        json << "  },\n";
        json << "  \"producerIngressCopySubmissions\": "
             << capture_stats.ingress_copy_submissions << ",\n";
        json << "  \"producerIngressTransformSubmissions\": "
             << capture_stats.ingress_transform_submissions << ",\n";
        json << "  \"busPublishedFrames\": "
             << capture_stats.published_frames << ",\n";
        json << "  \"busDirectPublishes\": "
             << bus_stats.direct_publishes << ",\n";
        json << "  \"busCopiedPublishes\": "
             << bus_stats.copied_publishes << ",\n";
        json << "  \"captureReceivedFrames\": "
             << capture_stats.received_frames << ",\n";
        json << "  \"sessionRebuilds\": "
             << capture_stats.session_rebuilds << ",\n";
        json << "  \"recoveryAttempts\": "
             << capture_stats.recovery_attempts << ",\n";
        json << "  \"recoverySuccesses\": "
             << capture_stats.recovery_successes << ",\n";
        json << "  \"idleRepublishedFrames\": "
             << capture_stats.idle_republished_frames << ",\n";
        json << "  \"sessionEvents\": " << capture_stats.session_events
             << ",\n";
        json << "  \"protectedContentFrames\": "
             << capture_stats.protected_content_frames << ",\n";
        json << "  \"skippedNoBuffer\": "
             << capture_stats.skipped_no_buffer << ",\n";
        json << "  \"measuredConsumerFrames\": "
             << consumer_stats.frames << ",\n";
        json << "  \"measuredConsumerLastSequence\": "
             << consumer_stats.last_sequence << ",\n";
        json << "  \"measuredSourcePresentations\": "
             << source_presentations << ",\n";
        json << "  \"measuredMonotonicViolations\": "
             << consumer_stats.monotonic_violations << ",\n";
        json << "  \"measuredInvalidDamageFrames\": "
             << consumer_stats.invalid_damage_frames << ",\n";
        json << "  \"measuredDurationMs\": " << measured_duration_ms
             << ",\n";
        json << "  \"warmupMs\": " << options.warmup_ms << ",\n";
        json << "  \"copyContractNote\": \""
             << json_escape(copy_contract_note) << "\",\n";
        json << "  \"gates\": {\n";
        json << "    \"published\": " << (published_gate ? "true" : "false")
             << ",\n";
        json << "    \"monotonic\": "
             << (monotonic_gate ? "true" : "false") << ",\n";
        json << "    \"damageValid\": "
             << (damage_gate ? "true" : "false") << ",\n";
        json << "    \"coverage\": " << (coverage_gate ? "true" : "false")
             << ",\n";
        json << "    \"finalDrain\": " << (drain_gate ? "true" : "false")
             << ",\n";
        json << "    \"copyContract\": "
             << (copy_contract_gate ? "true" : "false") << ",\n";
        json << "    \"workerAlive\": "
             << (running_gate ? "true" : "false") << "\n";
        json << "  },\n";
        json << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
        json << "  \"notes\": \"Runtime L2 bus-publish copy evidence for the "
                "Desktop Duplication resilience paths. No encoder is "
                "involved; MFT/driver internals remain unobservable. Idle "
                "heartbeat and session rebuilds are observed counters, not "
                "forced gates, because system desktop activity is not "
                "controlled by this workload.\"\n";
        json << "}\n";

        const std::wstring& path = *options.runtime_evidence_json;
        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            fail("writing runtime evidence failed: "
                + std::to_string(GetLastError()));
        }
        const std::string payload = json.str();
        DWORD written = 0;
        if (!WriteFile(
                file,
                payload.data(),
                static_cast<DWORD>(payload.size()),
                &written,
                nullptr)
            || written != payload.size()) {
            fail("writing runtime evidence payload failed");
        }
        CloseHandle(file);
        std::fprintf(
            stdout,
            "runtime evidence written: %llu bytes\n",
            static_cast<unsigned long long>(payload.size()));
    }

    return passed ? 0 : 1;
}
