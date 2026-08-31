#include <fluxcap/gpu.hpp>

#include "../test_support/win32_child_process.hpp"

#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <bcrypt.h>
#include <windows.h>
#include <wrl/client.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

namespace gpu = fluxcap::gpu;
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
using fluxcap::test_support::ChildProcess;
using fluxcap::test_support::parse_inherited_handle;
using fluxcap::test_support::read_exact;
using fluxcap::test_support::write_exact;

constexpr wchar_t kWindowClass[] = L"FluxCapWgcBusBenchmarkWindow";
constexpr UINT kStopMessage = WM_APP + 41;
constexpr std::uint32_t kSourceWidth = 1280;
constexpr std::uint32_t kSourceHeight = 720;
constexpr std::uint32_t kMediaFps = 240;
constexpr std::uint32_t kMaximumDurationMs = 60'000;
constexpr std::uint32_t kMaximumPairs = 20;
constexpr std::uint32_t kChildWireVersion = 3;
constexpr std::uint32_t kDefaultChildControlTimeoutMs = 15'000;
constexpr std::uint32_t kChildArmLeadMs = 50;
constexpr std::uint32_t kMaximumBoundarySkewMs = 250;
constexpr std::uint32_t kChildMeasureCommand = 0x4d45'4153u;
constexpr std::uint32_t kChildCloseCommand = 0x434c'4f53u;
constexpr std::uint32_t kChildExitCommand = 0x4558'4954u;
constexpr std::size_t kChildSamplesPerChunk = 480;
constexpr std::uint64_t kMaximumMeasuredFramesPerMillisecond = 10;
constexpr std::uint64_t kMinimumChildLatencySampleLimit = 64;
constexpr double kMaximumLatencySampleMs = 60'000.0;

enum class Topology : std::uint8_t { staged, direct };
enum class ConsumerMode : std::uint8_t { inproc, child };
enum class BusFormat : std::uint32_t { bgra = 0, nv12 = 1, p010 = 2 };
enum class CaptureBackend : std::uint8_t { wgc, desktop_duplication };
enum class ChildFault : std::uint32_t {
    none = 0,
    exit_before_ready = 1,
    hang_after_ready = 2,
    close_response_after_ready = 3,
};
enum class ChildSampleKind : std::uint32_t {
    source_age = 1,
    capture_age = 2,
    gpu_completion = 3,
    packet_latency = 4,
};

struct Options final {
    std::uint32_t size = 320;
    std::uint32_t source_size = 0;
    std::uint32_t duration_ms = 2'000;
    std::uint32_t warmup_ms = 500;
    std::uint32_t pairs = 2;
    std::uint32_t ipc_timeout_ms = kDefaultChildControlTimeoutMs;
    std::optional<std::uint32_t> adapter_index;
    std::optional<std::uint32_t> output_index;
    ConsumerMode consumer_mode = ConsumerMode::inproc;
    BusFormat bus_format = BusFormat::bgra;
    CaptureBackend capture_backend = CaptureBackend::wgc;
    gpu::VideoCodec codec = gpu::VideoCodec::h264;
    gpu::GpuTransformBackend planar_backend =
        gpu::GpuTransformBackend::automatic;
    bool planar_backend_explicit = false;
    ChildFault child_fault = ChildFault::none;
    std::optional<std::wstring> runtime_evidence_json;
    std::string qualification_run_nonce;
    bool list_adapters = false;
    bool help = false;
};

struct ChildSetup final {
    std::uint32_t structure_size = sizeof(ChildSetup);
    std::uint32_t wire_version = kChildWireVersion;
    std::uint32_t size = 0;
    std::uint32_t topology = 0;
    std::uint32_t test_fault = 0;
    std::uint32_t bus_format = 0;
    std::uint64_t run_nonce = 0;
    gpu::SharedFrameBusRegistration registration{};
};

struct ChildMeasure final {
    std::uint32_t structure_size = sizeof(ChildMeasure);
    std::uint32_t wire_version = kChildWireVersion;
    std::uint32_t command = kChildMeasureCommand;
    std::uint32_t topology = 0;
    std::uint64_t run_nonce = 0;
    std::uint64_t measure_begin_qpc = 0;
    std::uint64_t measure_end_qpc = 0;
    std::uint64_t qpc_frequency = 0;
};

struct ChildReady final {
    std::uint32_t structure_size = sizeof(ChildReady);
    std::uint32_t wire_version = kChildWireVersion;
    std::uint32_t success = 0;
    std::uint32_t consumer_index = 0;
    std::uint32_t topology = 0;
    std::uint32_t child_process_id = 0;
    std::uint64_t run_nonce = 0;
    std::uint32_t gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::system_error);
    HRESULT hresult = E_FAIL;
    char message[384]{};
};

struct ChildArmed final {
    std::uint32_t structure_size = sizeof(ChildArmed);
    std::uint32_t wire_version = kChildWireVersion;
    std::uint32_t success = 0;
    std::uint32_t consumer_index = 0;
    std::uint32_t topology = 0;
    std::uint32_t gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::system_error);
    std::uint64_t run_nonce = 0;
    HRESULT hresult = E_FAIL;
    std::uint64_t command_received_qpc = 0;
    std::uint64_t armed_sent_qpc = 0;
    char message[384]{};
};

struct ChildResultHeader final {
    std::uint32_t structure_size = sizeof(ChildResultHeader);
    std::uint32_t wire_version = kChildWireVersion;
    std::uint32_t success = 0;
    std::uint32_t consumer_index = 0;
    std::uint32_t topology = 0;
    std::uint32_t reserved = 0;
    std::uint64_t run_nonce = 0;
    std::uint32_t gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::system_error);
    HRESULT hresult = E_FAIL;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint64_t consumer_frames = 0;
    std::uint64_t distinct_source_timestamps = 0;
    std::uint64_t bus_gaps = 0;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t packet_submissions = 0;
    std::uint64_t packets = 0;
    std::uint64_t packet_bytes = 0;
    std::uint64_t encoder_copied_submissions = 0;
    std::uint64_t encoder_direct_submissions = 0;
    std::uint64_t encoder_external_submissions = 0;
    std::uint64_t encoder_external_identity_verified_submissions = 0;
    std::uint64_t encoder_external_video_encoder_bound_submissions = 0;
    std::uint64_t gpu_query_dropped = 0;
    std::uint64_t source_age_samples = 0;
    std::uint64_t capture_age_samples = 0;
    std::uint64_t gpu_completion_samples = 0;
    std::uint64_t packet_latency_samples = 0;
    std::uint64_t scheduled_begin_qpc = 0;
    std::uint64_t scheduled_end_qpc = 0;
    std::uint64_t actual_begin_qpc = 0;
    std::uint64_t actual_end_qpc = 0;
    std::uint64_t result_send_qpc = 0;
    char message[384]{};
};

struct ChildSampleChunk final {
    std::uint32_t structure_size = sizeof(ChildSampleChunk);
    std::uint32_t wire_version = kChildWireVersion;
    ChildSampleKind kind = ChildSampleKind::source_age;
    std::uint32_t count = 0;
    std::uint32_t topology = 0;
    std::uint32_t reserved = 0;
    std::uint64_t run_nonce = 0;
    std::uint64_t first_sample = 0;
    std::array<double, kChildSamplesPerChunk> samples{};
};

struct ChildCommand final {
    std::uint32_t structure_size = sizeof(ChildCommand);
    std::uint32_t wire_version = kChildWireVersion;
    std::uint32_t command = 0;
    std::uint32_t topology = 0;
    std::uint64_t run_nonce = 0;
};

struct ChildClosed final {
    std::uint32_t structure_size = sizeof(ChildClosed);
    std::uint32_t wire_version = kChildWireVersion;
    std::uint32_t success = 0;
    std::uint32_t consumer_index = 0;
    std::uint32_t topology = 0;
    std::uint32_t reserved = 0;
    std::uint64_t run_nonce = 0;
    std::uint32_t gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::system_error);
    HRESULT hresult = E_FAIL;
    char message[384]{};
};

static_assert(std::is_trivially_copyable_v<ChildSetup>);
static_assert(std::is_trivially_copyable_v<ChildMeasure>);
static_assert(std::is_trivially_copyable_v<ChildReady>);
static_assert(std::is_trivially_copyable_v<ChildArmed>);
static_assert(std::is_trivially_copyable_v<ChildResultHeader>);
static_assert(std::is_trivially_copyable_v<ChildSampleChunk>);
static_assert(std::is_trivially_copyable_v<ChildCommand>);
static_assert(std::is_trivially_copyable_v<ChildClosed>);
static_assert(sizeof(ChildSampleChunk)
    <= fluxcap::test_support::child_process_max_wire_message_bytes);

struct DeviceContext final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

struct WindowState final {
    HANDLE ready = nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    HMONITOR target_monitor = nullptr;
    RECT target_desktop{};
    std::atomic<HWND> window{nullptr};
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<std::uint64_t> presentations{0};
};

struct Summary final {
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
};

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

std::string gpu_error_text(const gpu::GpuError& error) {
    std::string text = error.what();
    if (text.empty()) text = "GPU status " + std::to_string(static_cast<int>(error.status));
    text += " (hr=" + std::to_string(static_cast<long long>(error.hresult)) + ')';
    return text;
}

void require_gpu(const gpu::GpuError& result, std::string_view operation) {
    if (!result) fail(std::string(operation) + " failed: " + gpu_error_text(result));
}

void require_wgc(const gpu::WgcResult& result, std::string_view operation) {
    if (!result) {
        fail(std::string(operation) + " failed: " + result.message + " (status="
            + gpu::wgc_status_string(result.status) + ", hr="
            + std::to_string(static_cast<long long>(result.hresult)) + ')');
    }
}

void require_encoder(
    const gpu::GpuEncoderResult& result,
    std::string_view operation) {
    if (!result) {
        fail(std::string(operation) + " failed: " + result.message + " (status="
            + gpu::gpu_encoder_status_string(result.status) + ", hr="
            + std::to_string(static_cast<long long>(result.hresult)) + ')');
    }
}

void copy_message(char (&destination)[384], std::string_view message) noexcept {
    const std::size_t count = std::min(message.size(), sizeof(destination) - 1);
    std::memcpy(destination, message.data(), count);
    destination[count] = '\0';
}

std::uint64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t qpc_frequency() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0) {
        fail("QueryPerformanceFrequency failed");
    }
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t qpc_ticks_for_ms(
    std::uint32_t milliseconds_value,
    std::uint64_t frequency) noexcept {
    return (static_cast<std::uint64_t>(milliseconds_value) * frequency + 999u)
        / 1'000u;
}

double qpc_milliseconds(
    std::uint64_t end,
    std::uint64_t begin,
    std::uint64_t frequency) noexcept {
    if (frequency == 0 || end < begin) return 0.0;
    return static_cast<double>(end - begin) * 1'000.0
        / static_cast<double>(frequency);
}

double qpc_skew_milliseconds(
    std::uint64_t actual,
    std::uint64_t scheduled,
    std::uint64_t frequency) noexcept {
    if (frequency == 0) return 0.0;
    if (actual >= scheduled) {
        return qpc_milliseconds(actual, scheduled, frequency);
    }
    return -qpc_milliseconds(scheduled, actual, frequency);
}

std::uint64_t make_run_nonce(Topology topology, std::uint32_t pair) noexcept {
    std::uint64_t nonce = qpc_now()
        ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32)
        ^ (static_cast<std::uint64_t>(pair) << 8)
        ^ static_cast<std::uint64_t>(topology);
    return nonce == 0 ? 1 : nonce;
}

std::string make_qualification_nonce() {
    const std::uint64_t first = qpc_now()
        ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const std::uint64_t second = static_cast<std::uint64_t>(counter.QuadPart)
        ^ reinterpret_cast<std::uintptr_t>(&counter)
        ^ (first << 17u) ^ (first >> 11u);
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << first << std::setw(16) << second;
    return output.str();
}

const char* consumer_mode_name(ConsumerMode mode) noexcept {
    return mode == ConsumerMode::child ? "child" : "inproc";
}

const char* bus_format_name(BusFormat format) noexcept {
    switch (format) {
    case BusFormat::bgra: return "bgra";
    case BusFormat::nv12: return "nv12";
    case BusFormat::p010: return "p010";
    }
    return "unknown";
}

const char* evidence_format_name(BusFormat format) noexcept {
    switch (format) {
    case BusFormat::nv12: return "NV12";
    case BusFormat::p010: return "P010";
    case BusFormat::bgra: return "BGRA8";
    }
    return "UNKNOWN";
}

bool planar_bus_format(BusFormat format) noexcept {
    return format == BusFormat::nv12 || format == BusFormat::p010;
}

DXGI_FORMAT bus_dxgi_format(BusFormat format) noexcept {
    switch (format) {
    case BusFormat::nv12: return DXGI_FORMAT_NV12;
    case BusFormat::p010: return DXGI_FORMAT_P010;
    case BusFormat::bgra: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

const char* codec_name(gpu::VideoCodec codec) noexcept {
    switch (codec) {
    case gpu::VideoCodec::h264: return "h264";
    case gpu::VideoCodec::hevc: return "hevc";
    case gpu::VideoCodec::av1: return "av1";
    }
    return "unknown";
}

const char* encoder_input_subtype_name(BusFormat format) noexcept {
    switch (format) {
    case BusFormat::nv12: return "MFVideoFormat_NV12";
    case BusFormat::p010: return "MFVideoFormat_P010";
    case BusFormat::bgra: return "MFVideoFormat_NV12";
    }
    return "unknown";
}

const char* encoder_output_subtype_name(gpu::VideoCodec codec) noexcept {
    switch (codec) {
    case gpu::VideoCodec::h264: return "MFVideoFormat_H264";
    case gpu::VideoCodec::hevc: return "MFVideoFormat_HEVC";
    case gpu::VideoCodec::av1: return "MFVideoFormat_AV1";
    }
    return "unknown";
}

const char* capture_backend_name(CaptureBackend backend) noexcept {
    return backend == CaptureBackend::desktop_duplication
        ? "desktop_duplication" : "wgc";
}

const char* planar_backend_name(gpu::GpuTransformBackend backend) noexcept {
    switch (backend) {
    case gpu::GpuTransformBackend::deterministic_planar:
        return "deterministic_planar";
    case gpu::GpuTransformBackend::video_processor:
        return "video_processor";
    case gpu::GpuTransformBackend::automatic:
        return "automatic";
    }
    return "unknown";
}

bool valid_wire_bus_format(std::uint32_t format) noexcept {
    return format == static_cast<std::uint32_t>(BusFormat::bgra)
        || format == static_cast<std::uint32_t>(BusFormat::nv12);
}

std::uint32_t wire_topology(Topology topology) noexcept {
    return static_cast<std::uint32_t>(topology);
}

bool valid_wire_topology(std::uint32_t topology) noexcept {
    return topology == wire_topology(Topology::staged)
        || topology == wire_topology(Topology::direct);
}

ChildFault parse_child_fault(std::wstring_view value) {
    if (value == L"exit-before-ready") return ChildFault::exit_before_ready;
    if (value == L"hang-after-ready") return ChildFault::hang_after_ready;
    if (value == L"close-response-after-ready") {
        return ChildFault::close_response_after_ready;
    }
    throw std::invalid_argument("invalid --test-child-fault value");
}

std::uint32_t parse_u32(std::wstring_view text, const char* name) {
    if (text.empty()) throw std::invalid_argument(std::string(name) + " requires a value");
    std::uint64_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            throw std::invalid_argument(std::string(name) + " must be an integer");
        }
        value = value * 10u + static_cast<unsigned>(character - L'0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument(std::string(name) + " is too large");
        }
    }
    return static_cast<std::uint32_t>(value);
}

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        auto value_after = [&](std::wstring_view prefix, const char* name) {
            if (argument.starts_with(prefix)) return parse_u32(argument.substr(prefix.size()), name);
            if (++index >= argc) throw std::invalid_argument(std::string(name) + " requires a value");
            return parse_u32(argv[index], name);
        };
        if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else if (argument == L"--list-adapters") {
            options.list_adapters = true;
        } else if (argument == L"--size" || argument.starts_with(L"--size=")) {
            options.size = value_after(L"--size=", "--size");
        } else if (argument == L"--duration-ms" || argument.starts_with(L"--duration-ms=")) {
            options.duration_ms = value_after(L"--duration-ms=", "--duration-ms");
        } else if (argument == L"--source-size"
            || argument.starts_with(L"--source-size=")) {
            options.source_size = value_after(
                L"--source-size=", "--source-size");
        } else if (argument == L"--warmup-ms" || argument.starts_with(L"--warmup-ms=")) {
            options.warmup_ms = value_after(L"--warmup-ms=", "--warmup-ms");
        } else if (argument == L"--pairs" || argument.starts_with(L"--pairs=")) {
            options.pairs = value_after(L"--pairs=", "--pairs");
        } else if (argument == L"--consumer-mode"
            || argument.starts_with(L"--consumer-mode=")) {
            std::wstring_view value;
            if (argument.starts_with(L"--consumer-mode=")) {
                value = argument.substr(std::wstring_view(L"--consumer-mode=").size());
            } else {
                if (++index >= argc) {
                    throw std::invalid_argument("--consumer-mode requires a value");
                }
                value = argv[index];
            }
            if (value == L"inproc") {
                options.consumer_mode = ConsumerMode::inproc;
            } else if (value == L"child") {
                options.consumer_mode = ConsumerMode::child;
            } else {
                throw std::invalid_argument(
                    "--consumer-mode must be inproc or child");
            }
        } else if (argument == L"--bus-format"
            || argument.starts_with(L"--bus-format=")) {
            std::wstring_view value;
            if (argument.starts_with(L"--bus-format=")) {
                value = argument.substr(
                    std::wstring_view(L"--bus-format=").size());
            } else {
                if (++index >= argc) {
                    throw std::invalid_argument("--bus-format requires a value");
                }
                value = argv[index];
            }
            if (value == L"bgra") {
                options.bus_format = BusFormat::bgra;
            } else if (value == L"nv12") {
                options.bus_format = BusFormat::nv12;
            } else if (value == L"p010") {
                options.bus_format = BusFormat::p010;
            } else {
                throw std::invalid_argument(
                    "--bus-format must be bgra, nv12, or p010");
            }
        } else if (argument == L"--codec"
            || argument.starts_with(L"--codec=")) {
            std::wstring_view value;
            if (argument.starts_with(L"--codec=")) {
                value = argument.substr(std::wstring_view(L"--codec=").size());
            } else {
                if (++index >= argc) {
                    throw std::invalid_argument("--codec requires a value");
                }
                value = argv[index];
            }
            if (value == L"h264") {
                options.codec = gpu::VideoCodec::h264;
            } else if (value == L"hevc") {
                options.codec = gpu::VideoCodec::hevc;
            } else if (value == L"av1") {
                options.codec = gpu::VideoCodec::av1;
            } else {
                throw std::invalid_argument(
                    "--codec must be h264, hevc, or av1");
            }
        } else if (argument == L"--capture-backend"
            || argument.starts_with(L"--capture-backend=")) {
            std::wstring_view value;
            if (argument.starts_with(L"--capture-backend=")) {
                value = argument.substr(
                    std::wstring_view(L"--capture-backend=").size());
            } else {
                if (++index >= argc) {
                    throw std::invalid_argument(
                        "--capture-backend requires a value");
                }
                value = argv[index];
            }
            if (value == L"wgc") {
                options.capture_backend = CaptureBackend::wgc;
            } else if (value == L"desktop-duplication") {
                options.capture_backend = CaptureBackend::desktop_duplication;
            } else {
                throw std::invalid_argument(
                    "--capture-backend must be wgc or desktop-duplication");
            }
        } else if (argument == L"--planar-backend"
            || argument.starts_with(L"--planar-backend=")) {
            std::wstring_view value;
            if (argument.starts_with(L"--planar-backend=")) {
                value = argument.substr(
                    std::wstring_view(L"--planar-backend=").size());
            } else {
                if (++index >= argc) {
                    throw std::invalid_argument(
                        "--planar-backend requires a value");
                }
                value = argv[index];
            }
            options.planar_backend_explicit = true;
            if (value == L"automatic") {
                options.planar_backend = gpu::GpuTransformBackend::automatic;
            } else if (value == L"deterministic-planar") {
                options.planar_backend =
                    gpu::GpuTransformBackend::deterministic_planar;
            } else if (value == L"video-processor") {
                options.planar_backend =
                    gpu::GpuTransformBackend::video_processor;
            } else {
                throw std::invalid_argument(
                    "--planar-backend must be automatic, deterministic-planar, or video-processor");
            }
        } else if (argument == L"--ipc-timeout-ms"
            || argument.starts_with(L"--ipc-timeout-ms=")) {
            options.ipc_timeout_ms = value_after(
                L"--ipc-timeout-ms=", "--ipc-timeout-ms");
        } else if (argument == L"--adapter-index"
            || argument.starts_with(L"--adapter-index=")) {
            options.adapter_index = value_after(
                L"--adapter-index=", "--adapter-index");
        } else if (argument == L"--output-index"
            || argument.starts_with(L"--output-index=")) {
            options.output_index = value_after(
                L"--output-index=", "--output-index");
        } else if (argument == L"--test-child-fault"
            || argument.starts_with(L"--test-child-fault=")) {
            std::wstring_view value;
            if (argument.starts_with(L"--test-child-fault=")) {
                value = argument.substr(
                    std::wstring_view(L"--test-child-fault=").size());
            } else {
                if (++index >= argc) {
                    throw std::invalid_argument(
                        "--test-child-fault requires a value");
                }
                value = argv[index];
            }
            options.child_fault = parse_child_fault(value);
        } else if (argument == L"--runtime-evidence-json"
            || argument.starts_with(L"--runtime-evidence-json=")) {
            if (options.runtime_evidence_json.has_value()) {
                throw std::invalid_argument(
                    "--runtime-evidence-json may be specified only once");
            }
            std::wstring_view value;
            if (argument.starts_with(L"--runtime-evidence-json=")) {
                value = argument.substr(
                    std::wstring_view(L"--runtime-evidence-json=").size());
            } else {
                if (++index >= argc) {
                    throw std::invalid_argument(
                        "--runtime-evidence-json requires a value");
                }
                value = argv[index];
            }
            if (value.empty()) {
                throw std::invalid_argument(
                    "--runtime-evidence-json requires a nonempty path");
            }
            options.runtime_evidence_json.emplace(value);
        } else if (argument == L"--qualification-run-nonce"
            || argument.starts_with(L"--qualification-run-nonce=")) {
            std::wstring_view value;
            if (argument.starts_with(L"--qualification-run-nonce=")) {
                value = argument.substr(
                    std::wstring_view(L"--qualification-run-nonce=").size());
            } else {
                if (++index >= argc) {
                    throw std::invalid_argument(
                        "--qualification-run-nonce requires a value");
                }
                value = argv[index];
            }
            if (value.size() != 32) {
                throw std::invalid_argument(
                    "--qualification-run-nonce must be 32 lowercase hex characters");
            }
            options.qualification_run_nonce.clear();
            for (const wchar_t character : value) {
                if (!((character >= L'0' && character <= L'9')
                        || (character >= L'a' && character <= L'f'))) {
                    throw std::invalid_argument(
                        "--qualification-run-nonce must be 32 lowercase hex characters");
                }
                options.qualification_run_nonce.push_back(
                    static_cast<char>(character));
            }
        } else {
            throw std::invalid_argument("unknown option");
        }
    }
    if (options.size != 320 && options.size != 640) {
        throw std::invalid_argument("--size must be 320 or 640");
    }
    if (options.source_size == 0) options.source_size = options.size;
    if (options.source_size != 320 && options.source_size != 640) {
        throw std::invalid_argument("--source-size must be 320 or 640");
    }
    if (options.bus_format == BusFormat::bgra
        && options.source_size != options.size) {
        throw std::invalid_argument(
            "scaled source/output requires --bus-format=nv12");
    }
    if (options.consumer_mode == ConsumerMode::child
        && options.source_size != options.size) {
        throw std::invalid_argument(
            "scaled source/output currently requires --consumer-mode=inproc");
    }
    if (options.duration_ms == 0 || options.duration_ms > kMaximumDurationMs) {
        throw std::invalid_argument("--duration-ms must be between 1 and 60000");
    }
    if (options.warmup_ms > kMaximumDurationMs) {
        throw std::invalid_argument("--warmup-ms must be at most 60000");
    }
    if (options.pairs == 0 || options.pairs > kMaximumPairs) {
        throw std::invalid_argument("--pairs must be between 1 and 20");
    }
    if (options.ipc_timeout_ms < 100 || options.ipc_timeout_ms > 60'000) {
        throw std::invalid_argument(
            "--ipc-timeout-ms must be between 100 and 60000");
    }
    if (options.adapter_index.has_value() && *options.adapter_index > 31) {
        throw std::invalid_argument("--adapter-index must be between 0 and 31");
    }
    if (options.output_index.has_value() && *options.output_index > 31) {
        throw std::invalid_argument("--output-index must be between 0 and 31");
    }
    if (options.capture_backend == CaptureBackend::desktop_duplication
        && (!options.output_index.has_value()
            || !options.adapter_index.has_value())) {
        throw std::invalid_argument(
            "desktop-duplication requires explicit --adapter-index and --output-index");
    }
    if (options.capture_backend == CaptureBackend::wgc
        && options.output_index.has_value()) {
        throw std::invalid_argument(
            "--output-index is valid only with --capture-backend=desktop-duplication");
    }
    if (options.child_fault != ChildFault::none
        && options.consumer_mode != ConsumerMode::child) {
        throw std::invalid_argument(
            "--test-child-fault requires --consumer-mode=child");
    }
    if (options.consumer_mode == ConsumerMode::child
        && (options.bus_format == BusFormat::p010
            || options.codec != gpu::VideoCodec::h264
            || options.capture_backend != CaptureBackend::wgc)) {
        throw std::invalid_argument(
            "child mode currently supports WGC with BGRA/NV12 and H.264 only");
    }
    if (options.capture_backend == CaptureBackend::desktop_duplication
        && (!planar_bus_format(options.bus_format)
            || options.consumer_mode != ConsumerMode::inproc)) {
        throw std::invalid_argument(
            "desktop-duplication workload requires inproc NV12/P010 direct mode");
    }
    if (!planar_bus_format(options.bus_format)
        && options.planar_backend_explicit) {
        throw std::invalid_argument(
            "--planar-backend requires --bus-format=nv12 or p010");
    }
    if (options.runtime_evidence_json.has_value()
        && !options.planar_backend_explicit) {
        throw std::invalid_argument(
            "--runtime-evidence-json requires an explicit --planar-backend");
    }
    if (!options.runtime_evidence_json.has_value()
        && !options.qualification_run_nonce.empty()) {
        throw std::invalid_argument(
            "--qualification-run-nonce requires --runtime-evidence-json");
    }
    return options;
}

double seconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double>(duration).count();
}

double milliseconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double rate(std::uint64_t count, double elapsed) noexcept {
    return elapsed > 0.0 ? static_cast<double>(count) / elapsed : 0.0;
}

std::uint64_t delta(std::uint64_t after, std::uint64_t before) noexcept {
    return after >= before ? after - before : 0;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[rank == 0 ? 0 : std::min(rank - 1, sorted.size() - 1)];
}

Summary summarize(std::vector<double> values) {
    if (values.empty()) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan};
    }
    std::sort(values.begin(), values.end());
    return {percentile(values, 0.50), percentile(values, 0.95), percentile(values, 0.99)};
}

const char* topology_name(Topology topology) noexcept {
    return topology == Topology::staged ? "staged" : "direct";
}

DeviceContext create_device(IDXGIAdapter* adapter = nullptr) {
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    DeviceContext output;
    D3D_FEATURE_LEVEL created_level{};
    const D3D_DRIVER_TYPE driver = adapter == nullptr
        ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN;
    HRESULT hr = D3D11CreateDevice(
        adapter,
        driver,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &output.device,
        &created_level,
        &output.context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            adapter,
            driver,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &output.device,
            &created_level,
            &output.context);
    }
    if (FAILED(hr)) fail("D3D11CreateDevice failed: " + std::to_string(hr));
    return output;
}

ComPtr<IDXGIAdapter1> adapter_at_index(std::uint32_t index) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        fail("CreateDXGIFactory1 failed while selecting adapter: "
            + std::to_string(static_cast<long long>(hr)));
    }
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(index, &adapter);
    if (FAILED(hr)) {
        fail("DXGI adapter index is unavailable: "
            + std::to_string(index));
    }
    DXGI_ADAPTER_DESC1 description{};
    hr = adapter->GetDesc1(&description);
    if (FAILED(hr) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
        fail("selected DXGI adapter is unavailable or software-only");
    }
    return adapter;
}

std::string utf8_text(const wchar_t* value) {
    if (value == nullptr || value[0] == L'\0') return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string output(static_cast<std::size_t>(bytes), '\0');
    (void)WideCharToMultiByte(
        CP_UTF8, 0, value, -1, output.data(), bytes, nullptr, nullptr);
    output.pop_back();
    return output;
}

const char* rotation_name(DXGI_MODE_ROTATION rotation) noexcept {
    switch (rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY: return "identity";
    case DXGI_MODE_ROTATION_ROTATE90: return "rotate90";
    case DXGI_MODE_ROTATION_ROTATE180: return "rotate180";
    case DXGI_MODE_ROTATION_ROTATE270: return "rotate270";
    default: return "invalid";
    }
}

struct MonitorTarget final {
    std::uint32_t output_index = 0;
    HMONITOR monitor = nullptr;
    std::string device_name;
    RECT desktop{};
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    std::uint32_t logical_width = 0;
    std::uint32_t logical_height = 0;
    std::uint32_t surface_width = 0;
    std::uint32_t surface_height = 0;
};

MonitorTarget select_monitor_target(
    ID3D11Device* device,
    std::uint32_t output_index) {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC description{};
    HRESULT hr = device == nullptr
        ? E_INVALIDARG : device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->EnumOutputs(output_index, &output);
    if (SUCCEEDED(hr)) hr = output->GetDesc(&description);
    if (FAILED(hr)) {
        fail("selected adapter output is unavailable: "
            + std::to_string(output_index));
    }
    if (!description.AttachedToDesktop || description.Monitor == nullptr) {
        fail("selected adapter output is not attached to the desktop");
    }
    const LONG logical_width = description.DesktopCoordinates.right
        - description.DesktopCoordinates.left;
    const LONG logical_height = description.DesktopCoordinates.bottom
        - description.DesktopCoordinates.top;
    if (logical_width <= 0 || logical_height <= 0
        || std::string_view(rotation_name(description.Rotation)) == "invalid") {
        fail("selected adapter output has invalid desktop geometry or rotation");
    }
    MonitorTarget target;
    target.output_index = output_index;
    target.monitor = description.Monitor;
    target.device_name = utf8_text(description.DeviceName);
    target.desktop = description.DesktopCoordinates;
    target.rotation = description.Rotation;
    target.logical_width = static_cast<std::uint32_t>(logical_width);
    target.logical_height = static_cast<std::uint32_t>(logical_height);
    if (description.Rotation == DXGI_MODE_ROTATION_ROTATE90
        || description.Rotation == DXGI_MODE_ROTATION_ROTATE270) {
        target.surface_width = target.logical_height;
        target.surface_height = target.logical_width;
    } else {
        target.surface_width = target.logical_width;
        target.surface_height = target.logical_height;
    }
    return target;
}

bool same_monitor_target(
    const MonitorTarget& left,
    const MonitorTarget& right) noexcept {
    return left.output_index == right.output_index
        && left.monitor == right.monitor
        && left.device_name == right.device_name
        && left.desktop.left == right.desktop.left
        && left.desktop.top == right.desktop.top
        && left.desktop.right == right.desktop.right
        && left.desktop.bottom == right.desktop.bottom
        && left.rotation == right.rotation
        && left.logical_width == right.logical_width
        && left.logical_height == right.logical_height
        && left.surface_width == right.surface_width
        && left.surface_height == right.surface_height;
}

void print_adapters() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) fail("CreateDXGIFactory1 failed while listing adapters");
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) fail("EnumAdapters1 failed while listing adapters");
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) {
            fail("GetDesc1 failed while listing adapters");
        }
        const int bytes = WideCharToMultiByte(
            CP_UTF8, 0, description.Description, -1, nullptr, 0, nullptr, nullptr);
        std::string name = "unknown";
        if (bytes > 1) {
            name.resize(static_cast<std::size_t>(bytes));
            (void)WideCharToMultiByte(
                CP_UTF8, 0, description.Description, -1,
                name.data(), bytes, nullptr, nullptr);
            name.pop_back();
        }
        std::ostringstream luid;
        luid << "0x" << std::hex << std::uppercase << std::setfill('0')
             << std::setw(8)
             << static_cast<std::uint32_t>(description.AdapterLuid.HighPart)
             << ":0x" << std::setw(8) << description.AdapterLuid.LowPart;
        std::cout << index << '\t' << name << '\t' << luid.str()
                  << ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0
                        ? "\tsoftware" : "\thardware") << '\n';
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_hr = adapter->EnumOutputs(
                output_index, &output);
            if (output_hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(output_hr)) {
                fail("EnumOutputs failed while listing adapter outputs");
            }
            DXGI_OUTPUT_DESC output_description{};
            if (FAILED(output->GetDesc(&output_description))) {
                fail("GetDesc failed while listing adapter output");
            }
            std::cout << "  output " << output_index << '\t'
                      << utf8_text(output_description.DeviceName) << '\t'
                      << output_description.DesktopCoordinates.left << ','
                      << output_description.DesktopCoordinates.top << '-'
                      << output_description.DesktopCoordinates.right << ','
                      << output_description.DesktopCoordinates.bottom << '\t'
                      << rotation_name(output_description.Rotation) << '\t'
                      << (output_description.AttachedToDesktop
                            ? "attached" : "detached") << '\n';
        }
    }
}

DeviceContext create_device_for_registration(
    const gpu::SharedFrameBusRegistration& registration) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        fail("CreateDXGIFactory1 failed: "
            + std::to_string(static_cast<long long>(hr)));
    }
    ComPtr<IDXGIAdapter1> selected;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) {
            fail("EnumAdapters1 failed: "
                + std::to_string(static_cast<long long>(hr)));
        }
        DXGI_ADAPTER_DESC1 description{};
        hr = candidate->GetDesc1(&description);
        if (FAILED(hr)) {
            fail("IDXGIAdapter1::GetDesc1 failed: "
                + std::to_string(static_cast<long long>(hr)));
        }
        if (description.AdapterLuid.LowPart == registration.adapter_luid_low
            && description.AdapterLuid.HighPart == registration.adapter_luid_high) {
            selected = std::move(candidate);
            break;
        }
    }
    if (selected == nullptr) {
        fail("registration adapter was not found in the child process");
    }
    return create_device(selected.Get());
}

std::string adapter_name(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (device == nullptr
        || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))
        || FAILED(dxgi_device->GetAdapter(&adapter))
        || FAILED(adapter->GetDesc(&description))) {
        return "unknown";
    }
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, description.Description, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return "unknown";
    std::string output(static_cast<std::size_t>(bytes), '\0');
    (void)WideCharToMultiByte(
        CP_UTF8, 0, description.Description, -1, output.data(), bytes, nullptr, nullptr);
    output.pop_back();
    return output;
}

std::string adapter_luid_string(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (device == nullptr
        || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))
        || FAILED(dxgi_device->GetAdapter(&adapter))
        || FAILED(adapter->GetDesc(&description))) {
        fail("failed to query the benchmark adapter LUID");
    }
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8)
           << static_cast<std::uint32_t>(description.AdapterLuid.HighPart)
           << ":0x" << std::setw(8) << description.AdapterLuid.LowPart;
    return output.str();
}

ComPtr<IDXGIAdapter1> device_adapter(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter1> adapter1;
    HRESULT hr = device == nullptr
        ? E_INVALIDARG : device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter.As(&adapter1);
    if (FAILED(hr)) fail("failed to query benchmark device adapter");
    return adapter1;
}

std::wstring running_executable_path() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            fail("GetModuleFileNameW failed: "
                + std::to_string(GetLastError()));
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        if (buffer.size() >= 32'768) {
            fail("running executable path exceeds the supported length");
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2u, 32'768u));
    }
}

std::string sha256_file(const std::wstring& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    auto cleanup = [&]() noexcept {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
            hash = nullptr;
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            algorithm = nullptr;
        }
    };
    try {
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status != 0) {
            fail("BCryptOpenAlgorithmProvider(SHA-256) failed: "
                + std::to_string(status));
        }
        DWORD object_bytes = 0;
        DWORD copied_bytes = 0;
        status = BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_bytes),
            sizeof(object_bytes),
            &copied_bytes,
            0);
        if (status != 0 || copied_bytes != sizeof(object_bytes)
            || object_bytes == 0) {
            fail("BCryptGetProperty(SHA-256 object length) failed");
        }
        std::vector<std::uint8_t> object(object_bytes);
        status = BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            static_cast<ULONG>(object.size()),
            nullptr,
            0,
            0);
        if (status != 0) {
            fail("BCryptCreateHash(SHA-256) failed: "
                + std::to_string(status));
        }
        file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            fail("opening the running executable for SHA-256 failed: "
                + std::to_string(GetLastError()));
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
                fail("reading the running executable for SHA-256 failed: "
                    + std::to_string(GetLastError()));
            }
            if (bytes == 0) break;
            status = BCryptHashData(hash, input.data(), bytes, 0);
            if (status != 0) {
                fail("BCryptHashData(SHA-256) failed: "
                    + std::to_string(status));
            }
        }
        std::array<std::uint8_t, 32> digest{};
        status = BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        if (status != 0) {
            fail("BCryptFinishHash(SHA-256) failed: "
                + std::to_string(status));
        }
        cleanup();
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const std::uint8_t byte : digest) {
            output << std::setw(2) << static_cast<unsigned>(byte);
        }
        return output.str();
    } catch (...) {
        cleanup();
        throw;
    }
}

std::string sha256_text(std::string_view value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto cleanup = [&]() noexcept {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
            hash = nullptr;
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            algorithm = nullptr;
        }
    };
    try {
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status != 0) fail("opening SHA-256 provider for tuple failed");
        DWORD object_bytes = 0;
        DWORD copied_bytes = 0;
        status = BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_bytes),
            sizeof(object_bytes),
            &copied_bytes,
            0);
        if (status != 0 || copied_bytes != sizeof(object_bytes)
            || object_bytes == 0) {
            fail("querying SHA-256 tuple object length failed");
        }
        std::vector<std::uint8_t> object(object_bytes);
        status = BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            static_cast<ULONG>(object.size()),
            nullptr,
            0,
            0);
        if (status != 0) fail("creating SHA-256 tuple hash failed");
        if (value.size() > std::numeric_limits<ULONG>::max()) {
            fail("qualification tuple is too large to hash");
        }
        status = BCryptHashData(
            hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
            static_cast<ULONG>(value.size()),
            0);
        if (status != 0) fail("hashing qualification tuple failed");
        std::array<std::uint8_t, 32> digest{};
        status = BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        if (status != 0) fail("finishing qualification tuple hash failed");
        cleanup();
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const std::uint8_t byte : digest) {
            output << std::setw(2) << static_cast<unsigned>(byte);
        }
        return output.str();
    } catch (...) {
        cleanup();
        throw;
    }
}

void truncate_runtime_evidence_file(const std::wstring& path) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail("creating runtime evidence output failed: "
            + std::to_string(GetLastError()));
    }
    if (!CloseHandle(file)) {
        fail("closing runtime evidence output failed: "
            + std::to_string(GetLastError()));
    }
}

class D3DWindowSource final {
public:
    HRESULT initialize(HWND window, IDXGIAdapter* adapter) noexcept {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL created_level{};
        const D3D_DRIVER_TYPE driver = adapter == nullptr
            ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN;
        HRESULT hr = D3D11CreateDevice(
            adapter,
            driver,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device_,
            &created_level,
            &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(
                adapter,
                driver,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels + 1,
                1,
                D3D11_SDK_VERSION,
                &device_,
                &created_level,
                &context_);
        }
        if (FAILED(hr)) return hr;

        ComPtr<IDXGIDevice1> dxgi_device;
        ComPtr<IDXGIAdapter> swap_adapter;
        ComPtr<IDXGIFactory2> factory;
        hr = device_.As(&dxgi_device);
        if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&swap_adapter);
        if (SUCCEEDED(hr)) hr = swap_adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        (void)dxgi_device->SetMaximumFrameLatency(1);

        RECT client{};
        if (!GetClientRect(window, &client)) return HRESULT_FROM_WIN32(GetLastError());
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
        description.Height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        ComPtr<IDXGISwapChain1> swap_chain1;
        hr = factory->CreateSwapChainForHwnd(
            device_.Get(), window, &description, nullptr, nullptr, &swap_chain1);
        if (FAILED(hr)) {
            description.Flags = 0;
            hr = factory->CreateSwapChainForHwnd(
                device_.Get(), window, &description, nullptr, nullptr, &swap_chain1);
        }
        if (FAILED(hr)) return hr;
        hr = swap_chain1.As(&swap_chain_);
        if (FAILED(hr)) return hr;
        (void)factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        (void)swap_chain_->SetMaximumFrameLatency(1);

        ComPtr<ID3D11Texture2D> back_buffer;
        hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        if (FAILED(hr)) return hr;
        return device_->CreateRenderTargetView(
            back_buffer.Get(), nullptr, &render_target_);
    }

    HRESULT present(std::uint64_t frame_id) noexcept {
        const std::uint32_t id = static_cast<std::uint32_t>(frame_id & 0x00ff'ffffu);
        const float color[] = {
            static_cast<float>(id & 0xffu) / 255.0F,
            static_cast<float>((id >> 8) & 0xffu) / 255.0F,
            static_cast<float>((id >> 16) & 0xffu) / 255.0F,
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

class WindowThread final {
public:
    WindowThread(
        IDXGIAdapter1* adapter = nullptr,
        const MonitorTarget* target = nullptr) {
        state_.adapter = adapter;
        if (target != nullptr) {
            state_.target_monitor = target->monitor;
            state_.target_desktop = target->desktop;
        }
        state_.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (state_.ready == nullptr) fail("CreateEvent failed for benchmark window");
        thread_ = std::thread(&WindowThread::run, &state_);
        if (WaitForSingleObject(state_.ready, 5'000) != WAIT_OBJECT_0) {
            stop();
            fail("benchmark window creation timed out");
        }
        if (state_.error.load(std::memory_order_acquire) != ERROR_SUCCESS
            || state_.window.load(std::memory_order_acquire) == nullptr) {
            const DWORD error = state_.error.load(std::memory_order_relaxed);
            stop();
            fail("benchmark window creation failed: " + std::to_string(error));
        }
    }

    ~WindowThread() { stop(); }
    WindowThread(const WindowThread&) = delete;
    WindowThread& operator=(const WindowThread&) = delete;

    HWND window() const noexcept {
        return state_.window.load(std::memory_order_acquire);
    }

    std::uint64_t presentations() const noexcept {
        return state_.presentations.load(std::memory_order_relaxed);
    }

    void require_healthy(std::string_view stage) const {
        const DWORD error = state_.error.load(std::memory_order_acquire);
        const HWND window = state_.window.load(std::memory_order_acquire);
        if (error == ERROR_SUCCESS && window != nullptr) return;
        fail(std::string("benchmark source window failed during ")
            + std::string(stage) + ": " + std::to_string(error));
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

        RECT bounds{0, 0, static_cast<LONG>(kSourceWidth), static_cast<LONG>(kSourceHeight)};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0);
        const LONG window_width = bounds.right - bounds.left;
        const LONG window_height = bounds.bottom - bounds.top;
        LONG window_x = 64;
        LONG window_y = 64;
        if (state->target_monitor != nullptr) {
            window_x = state->target_desktop.left
                + (state->target_desktop.right - state->target_desktop.left
                    - window_width) / 2;
            window_y = state->target_desktop.top
                + (state->target_desktop.bottom - state->target_desktop.top
                    - window_height) / 2;
        }
        HWND window = CreateWindowExW(
            0,
            kWindowClass,
            L"FluxCap real WGC bus A/B source",
            WS_OVERLAPPEDWINDOW,
            window_x,
            window_y,
            window_width,
            window_height,
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
        // A qualification runner may hide the process through STARTUPINFO.
        // The first ShowWindow call is then allowed to ignore SW_SHOW, so make
        // the independently captured Desktop Duplication workload explicitly
        // visible without activating it.
        if (!SetWindowPos(
                window,
                nullptr,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                    | SWP_SHOWWINDOW)) {
            state->error.store(GetLastError(), std::memory_order_release);
            state->window.store(nullptr, std::memory_order_release);
            DestroyWindow(window);
            SetEvent(state->ready);
            return;
        }
        UpdateWindow(window);
        if (state->target_monitor != nullptr
            && MonitorFromWindow(window, MONITOR_DEFAULTTONULL)
                != state->target_monitor) {
            state->error.store(ERROR_INVALID_MONITOR_HANDLE,
                std::memory_order_release);
            state->window.store(nullptr, std::memory_order_release);
            DestroyWindow(window);
            SetEvent(state->ready);
            return;
        }

        D3DWindowSource source;
        HRESULT hr = source.initialize(window, state->adapter.Get());
        if (FAILED(hr)) {
            state->error.store(static_cast<DWORD>(hr), std::memory_order_release);
            state->window.store(nullptr, std::memory_order_release);
            DestroyWindow(window);
            SetEvent(state->ready);
            return;
        }
        hr = source.present(1);
        if (FAILED(hr)) {
            state->error.store(static_cast<DWORD>(hr), std::memory_order_release);
            state->window.store(nullptr, std::memory_order_release);
            DestroyWindow(window);
            SetEvent(state->ready);
            return;
        }
        state->presentations.store(1, std::memory_order_release);
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
            const std::uint64_t next =
                state->presentations.load(std::memory_order_relaxed) + 1;
            hr = source.present(next);
            if (hr == DXGI_STATUS_OCCLUDED) {
                Sleep(16);
            } else if (FAILED(hr)) {
                state->error.store(static_cast<DWORD>(hr), std::memory_order_release);
                DestroyWindow(window);
            } else {
                state->presentations.store(next, std::memory_order_release);
            }
        }
        state->window.store(nullptr, std::memory_order_release);
    }

    void stop() noexcept {
        HWND window = state_.window.load(std::memory_order_acquire);
        if (window != nullptr) PostMessageW(window, kStopMessage, 0, 0);
        if (thread_.joinable()) thread_.join();
        if (state_.ready != nullptr) {
            CloseHandle(state_.ready);
            state_.ready = nullptr;
        }
    }

    WindowState state_{};
    std::thread thread_;
};

struct PacketTracker final {
    struct Pending final {
        Clock::time_point submitted{};
        bool measured = false;
    };

    void submit(std::int64_t timestamp, bool measured) {
        std::lock_guard lock(mutex);
        pending[timestamp] = {Clock::now(), measured};
        if (measured) ++measured_submissions;
    }

    void cancel(std::int64_t timestamp) {
        std::lock_guard lock(mutex);
        pending.erase(timestamp);
    }

    static void callback(void* opaque, const gpu::EncodedPacket& packet) {
        auto& self = *static_cast<PacketTracker*>(opaque);
        const Clock::time_point now = Clock::now();
        std::lock_guard lock(self.mutex);
        ++self.total_packets;
        self.total_bytes += packet.size;
        if (packet.codec != self.expected_codec) {
            ++self.codec_mismatches;
        }
        const auto found = self.pending.find(packet.timestamp_100ns);
        if (found == self.pending.end()) return;
        if (found->second.measured) {
            self.packet_latency_ms.push_back(
                milliseconds(now - found->second.submitted));
            ++self.measured_packets;
            self.measured_bytes += packet.size;
        }
        self.pending.erase(found);
    }

    std::mutex mutex;
    std::unordered_map<std::int64_t, Pending> pending;
    std::vector<double> packet_latency_ms;
    std::uint64_t total_packets = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t measured_submissions = 0;
    std::uint64_t measured_packets = 0;
    std::uint64_t measured_bytes = 0;
    std::uint64_t codec_mismatches = 0;
    gpu::VideoCodec expected_codec = gpu::VideoCodec::h264;
};

class GpuCompletionTracker final {
public:
    void initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        context_ = context;
        D3D11_QUERY_DESC description{};
        description.Query = D3D11_QUERY_EVENT;
        for (Entry& entry : entries_) {
            const HRESULT hr = device->CreateQuery(&description, &entry.query);
            if (FAILED(hr)) {
                fail("D3D11 event query creation failed: "
                    + std::to_string(static_cast<long long>(hr)));
            }
        }
    }

    void submit(bool measured) {
        collect_ready();
        for (Entry& entry : entries_) {
            if (entry.pending) continue;
            context_->End(entry.query.Get());
            entry.submitted = Clock::now();
            entry.measured = measured;
            entry.pending = true;
            return;
        }
        if (measured) ++dropped_samples_;
    }

    void collect_ready() {
        const Clock::time_point now = Clock::now();
        for (Entry& entry : entries_) {
            if (!entry.pending) continue;
            BOOL complete = FALSE;
            const HRESULT hr = context_->GetData(
                entry.query.Get(),
                &complete,
                sizeof(complete),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE || (hr == S_OK && complete == FALSE)) continue;
            if (FAILED(hr)) {
                fail("D3D11 event query read failed: "
                    + std::to_string(static_cast<long long>(hr)));
            }
            if (entry.measured) {
                latency_ms_.push_back(milliseconds(now - entry.submitted));
            }
            entry.pending = false;
        }
    }

    void drain() {
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
        for (;;) {
            collect_ready();
            const bool any_pending = std::any_of(
                entries_.begin(), entries_.end(),
                [](const Entry& entry) { return entry.pending; });
            if (!any_pending) return;
            if (Clock::now() >= deadline) {
                fail("D3D11 event query drain timed out");
            }
            SwitchToThread();
        }
    }

    const std::vector<double>& latency_ms() const noexcept {
        return latency_ms_;
    }

    std::uint64_t dropped_samples() const noexcept {
        return dropped_samples_;
    }

private:
    struct Entry final {
        ComPtr<ID3D11Query> query;
        Clock::time_point submitted{};
        bool measured = false;
        bool pending = false;
    };

    ID3D11DeviceContext* context_ = nullptr;
    std::array<Entry, 8> entries_{};
    std::vector<double> latency_ms_;
    std::uint64_t dropped_samples_ = 0;
};

struct RunResult final {
    Topology topology = Topology::staged;
    ConsumerMode consumer_mode = ConsumerMode::inproc;
    BusFormat bus_format = BusFormat::bgra;
    CaptureBackend capture_backend = CaptureBackend::wgc;
    gpu::VideoCodec codec = gpu::VideoCodec::h264;
    gpu::GpuTransformBackend planar_backend =
        gpu::GpuTransformBackend::automatic;
    std::uint32_t pair = 0;
    std::uint32_t consumer_process_id = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t source_region_x = 0;
    std::uint32_t source_region_y = 0;
    std::uint32_t source_region_width = 0;
    std::uint32_t source_region_height = 0;
    std::uint64_t mailbox_generation = 0;
    std::uint32_t output_index = 0;
    std::string output_device_name;
    RECT output_desktop{};
    DXGI_MODE_ROTATION output_rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    std::uint32_t logical_monitor_width = 0;
    std::uint32_t logical_monitor_height = 0;
    std::uint32_t capture_surface_width = 0;
    std::uint32_t capture_surface_height = 0;
    std::string order;
    double elapsed_seconds = 0.0;
    std::uint64_t source_presentations = 0;
    std::uint64_t wgc_received = 0;
    std::uint64_t wgc_published = 0;
    std::uint64_t wgc_private_overwritten = 0;
    std::uint64_t wgc_skipped_no_buffer = 0;
    std::uint64_t desktop_present_frames = 0;
    std::uint64_t producer_ingress_copies = 0;
    std::uint64_t producer_ingress_transforms = 0;
    std::uint64_t bus_published = 0;
    std::uint64_t bus_no_slot = 0;
    std::uint64_t derived_control_contention = 0;
    std::uint64_t consumer_frames = 0;
    std::uint64_t distinct_source_timestamps = 0;
    std::uint64_t bus_gaps = 0;
    std::uint64_t first_bus_sequence = 0;
    std::uint64_t last_bus_sequence = 0;
    std::uint64_t packet_submissions = 0;
    std::uint64_t packets = 0;
    std::uint64_t packet_bytes = 0;
    std::uint64_t packet_codec_mismatches = 0;
    std::uint64_t encoder_copied_submissions = 0;
    std::uint64_t encoder_direct_submissions = 0;
    std::uint64_t encoder_external_submissions = 0;
    std::uint64_t encoder_external_identity_verified_submissions = 0;
    std::uint64_t encoder_external_video_encoder_bound_submissions = 0;
    std::uint64_t lifetime_producer_ingress_copies = 0;
    std::uint64_t lifetime_producer_ingress_transforms = 0;
    std::uint64_t lifetime_desktop_present_frames = 0;
    std::uint64_t lifetime_bus_published = 0;
    std::uint64_t lifetime_bus_copied_publishes = 0;
    std::uint64_t lifetime_bus_direct_publishes = 0;
    std::uint64_t lifetime_encoder_copied_submissions = 0;
    std::uint64_t lifetime_encoder_direct_submissions = 0;
    std::uint64_t lifetime_encoder_external_submissions = 0;
    std::uint64_t lifetime_encoder_external_identity_verified_submissions = 0;
    std::uint64_t lifetime_encoder_external_video_encoder_bound_submissions = 0;
    bool mft_internal_copy_observable = false;
    std::uint32_t encoder_matching_transform_count = 0;
    gpu::GpuEncoderMftIdentity encoder_mft_identity{};
    std::uint64_t gpu_query_dropped = 0;
    double setup_ready_ms = 0.0;
    double measure_armed_rtt_ms = 0.0;
    double result_header_delivery_ms = 0.0;
    double result_chunks_transfer_ms = 0.0;
    double close_closed_rtt_ms = 0.0;
    double parent_start_skew_ms = 0.0;
    double parent_end_skew_ms = 0.0;
    double child_start_skew_ms = 0.0;
    double child_end_skew_ms = 0.0;
    std::vector<double> source_age_ms;
    std::vector<double> capture_age_ms;
    std::vector<double> gpu_completion_ms;
    std::vector<double> packet_latency_ms;
};

struct ExternalBusCompletion final {
    void signal(const gpu::GpuError& result) noexcept {
        {
            std::lock_guard lock(mutex);
            release_result = result;
            completed = Clock::now();
            complete = true;
        }
        condition.notify_all();
    }

    bool wait(std::uint32_t timeout_ms) {
        std::unique_lock lock(mutex);
        if (complete) return true;
        if (timeout_ms == 0) return false;
        return condition.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            [&] { return complete; });
    }

    gpu::GpuError result() const noexcept {
        std::lock_guard lock(mutex);
        return release_result;
    }

    double completion_latency_ms() const noexcept {
        std::lock_guard lock(mutex);
        return milliseconds(completed - submitted);
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    Clock::time_point submitted{};
    Clock::time_point completed{};
    gpu::GpuError release_result{};
    bool measured = false;
    bool complete = false;
};

struct ExternalBusLeaseToken final {
    ExternalBusLeaseToken(
        gpu::SharedFrameBusFrameLease&& source,
        std::shared_ptr<ExternalBusCompletion> source_completion) noexcept
        : lease(std::move(source)), completion(std::move(source_completion)) {}

    ~ExternalBusLeaseToken() {
        gpu::GpuError released;
        if (lease) released = lease.release();
        completion->signal(released);
    }

    gpu::SharedFrameBusFrameLease lease;
    std::shared_ptr<ExternalBusCompletion> completion;
};

void validate_latency_samples(
    std::string_view name,
    const std::vector<double>& samples) {
    if (samples.empty()) {
        fail(std::string(name) + " has no measured samples");
    }
    for (const double sample : samples) {
        if (!std::isfinite(sample)
            || sample < 0.0
            || sample > kMaximumLatencySampleMs) {
            fail(std::string(name) + " contains an invalid latency sample");
        }
    }
    const Summary summary = summarize(samples);
    if (!std::isfinite(summary.p50_ms)
        || !std::isfinite(summary.p95_ms)
        || !std::isfinite(summary.p99_ms)
        || summary.p50_ms > summary.p95_ms
        || summary.p95_ms > summary.p99_ms) {
        fail(std::string(name) + " produced invalid latency percentiles");
    }
}

void validate_completed_run(const RunResult& result) {
    const std::uint32_t parent_process_id = GetCurrentProcessId();
    if (result.consumer_process_id == 0
        || (result.consumer_mode == ConsumerMode::child
            && result.consumer_process_id == parent_process_id)
        || (result.consumer_mode == ConsumerMode::inproc
            && result.consumer_process_id != parent_process_id)) {
        fail("consumer process identity does not match the selected mode");
    }
    if (!std::isfinite(result.elapsed_seconds) || result.elapsed_seconds <= 0.0) {
        fail("measured run has an invalid elapsed duration");
    }
    if (result.source_presentations == 0) {
        fail("benchmark source produced no presentation during the measured window");
    }
    if ((result.capture_backend == CaptureBackend::desktop_duplication
            && result.desktop_present_frames == 0)
        || (result.capture_backend == CaptureBackend::wgc
            && result.desktop_present_frames != 0)) {
        fail("capture backend reported inconsistent real desktop presentation coverage");
    }
    if (result.source_width == 0 || result.source_height == 0) {
        fail("measured run has invalid source dimensions");
    }
    if (result.consumer_frames == 0
        || result.distinct_source_timestamps == 0
        || result.distinct_source_timestamps > result.consumer_frames) {
        fail("measured run has invalid consumer/source frame coverage");
    }
    if (result.first_bus_sequence == 0
        || result.last_bus_sequence < result.first_bus_sequence) {
        fail("measured run has an invalid bus sequence range");
    }
    const std::uint64_t sequence_span =
        result.last_bus_sequence - result.first_bus_sequence + 1u;
    if (result.consumer_frames > sequence_span
        || result.bus_gaps != sequence_span - result.consumer_frames) {
        fail("measured run sequence gaps do not match its frame coverage");
    }
    if (result.packet_submissions != result.consumer_frames
        || result.packets != result.packet_submissions
        || result.packet_latency_ms.size() != result.packets
        || result.packet_bytes == 0
        || result.packet_codec_mismatches != 0) {
        fail("measured run does not have complete encoded-packet coverage");
    }
    if (result.encoder_copied_submissions
            + result.encoder_direct_submissions
            != result.packet_submissions
        || result.encoder_external_submissions
            > result.encoder_direct_submissions
        || result.encoder_external_identity_verified_submissions
            > result.encoder_external_submissions
        || result.encoder_external_video_encoder_bound_submissions
            > result.encoder_external_submissions) {
        fail("measured run has inconsistent encoder submission statistics");
    }
    if (planar_bus_format(result.bus_format)) {
        if (result.topology != Topology::direct
            || result.encoder_copied_submissions != 0
            || result.encoder_direct_submissions != result.consumer_frames
            || result.encoder_external_submissions != result.consumer_frames
            || result.encoder_external_identity_verified_submissions
                != result.encoder_external_submissions
            || result.producer_ingress_copies != 0
            || result.producer_ingress_transforms == 0) {
            fail("planar fast path fell back from external submission");
        }
    } else if (result.encoder_copied_submissions != result.consumer_frames
        || result.encoder_direct_submissions != 0
        || result.encoder_external_submissions != 0
        || result.encoder_external_identity_verified_submissions != 0
        || result.encoder_external_video_encoder_bound_submissions != 0) {
        fail("BGRA baseline encoder submission statistics are inconsistent");
    }
    if (result.capture_age_ms.size() != result.consumer_frames
        || (result.capture_backend == CaptureBackend::wgc
            && result.source_age_ms.size() != result.consumer_frames)
        || (result.capture_backend == CaptureBackend::desktop_duplication
            && !result.source_age_ms.empty())) {
        fail("measured run does not have complete metadata-age coverage");
    }
    if (result.gpu_completion_ms.size() > result.consumer_frames
        || result.gpu_query_dropped
            != result.consumer_frames - result.gpu_completion_ms.size()) {
        fail("measured run does not have complete GPU completion coverage");
    }
    if (result.capture_backend == CaptureBackend::wgc) {
        validate_latency_samples("source -> consumer age", result.source_age_ms);
    }
    validate_latency_samples("capture metadata -> consumer age", result.capture_age_ms);
    validate_latency_samples("transform -> GPU complete", result.gpu_completion_ms);
    validate_latency_samples("submit -> packet", result.packet_latency_ms);
}

struct RunContext final {
    Topology topology = Topology::staged;
    BusFormat bus_format = BusFormat::bgra;
    CaptureBackend capture_backend = CaptureBackend::wgc;
    gpu::SharedFrameBusPublisher publisher;
    // Captures are declared after the publisher so they stop/release their
    // retained producer state before the publisher is destroyed.
    gpu::WgcCapture capture;
    gpu::DesktopDuplicationCapture desktop_duplication;
    gpu::SharedFrameBusRegistration registration;
    gpu::SharedFrameBusConsumer consumer;
    gpu::GpuTransform transform;
    gpu::GpuEncoder encoder;
    gpu::GpuEncoderSupport encoder_support;
    PacketTracker packet_tracker;
    GpuCompletionTracker gpu_completion;
    std::deque<std::shared_ptr<ExternalBusCompletion>> external_completions;
    std::uint64_t next_media_frame = 0;
    std::int64_t frame_duration_100ns = 10'000'000 / kMediaFps;
};

gpu::WgcResult start_active_capture(RunContext& run) noexcept {
    return run.capture_backend == CaptureBackend::desktop_duplication
        ? run.desktop_duplication.start() : run.capture.start();
}

void stop_active_capture(RunContext& run) noexcept {
    if (run.capture_backend == CaptureBackend::desktop_duplication) {
        run.desktop_duplication.stop();
    } else {
        run.capture.stop();
    }
}

gpu::WgcCaptureStats active_capture_stats(const RunContext& run) noexcept {
    return run.capture_backend == CaptureBackend::desktop_duplication
        ? run.desktop_duplication.stats() : run.capture.stats();
}

std::uint64_t active_desktop_present_frames(const RunContext& run) noexcept {
    return run.capture_backend == CaptureBackend::desktop_duplication
        ? run.desktop_duplication.desktop_present_frames() : 0;
}

void require_active_capture_healthy(const RunContext& run) {
    if (run.capture_backend == CaptureBackend::desktop_duplication) {
        if (!run.desktop_duplication.running()
            || run.desktop_duplication.target_closed()) {
            require_wgc(
                run.desktop_duplication.last_error(),
                "Desktop Duplication capture health");
            fail("Desktop Duplication stopped without a reported error");
        }
    } else if (!run.capture.running() || run.capture.target_closed()) {
        fail("WGC capture stopped during the qualification workload");
    }
}

void initialize_consumer_pipeline(
    RunContext& run,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::uint32_t size,
    BusFormat bus_format,
    gpu::VideoCodec codec,
    bool require_l2_encoder_bind = false) {
    run.bus_format = bus_format;
    run.packet_tracker.expected_codec = codec;
    if (bus_format == BusFormat::bgra) {
        run.gpu_completion.initialize(device, context);

        gpu::GpuTransformConfig transform_config;
        transform_config.input_width = size;
        transform_config.input_height = size;
        transform_config.output_width = size;
        transform_config.output_height = size;
        transform_config.output_format = gpu::GpuPixelFormat::nv12;
        transform_config.frame_rate_numerator = kMediaFps;
        transform_config.frame_rate_denominator = 1;
        require_gpu(
            gpu::GpuTransform::create(device, transform_config, run.transform),
            "create BGRA to NV12 transform");
    } else {
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&multithread)))) {
            multithread->SetMultithreadProtected(TRUE);
        }
    }

    gpu::GpuEncoderConfig encoder_config;
    encoder_config.codec = codec;
    encoder_config.width = size;
    encoder_config.height = size;
    encoder_config.frame_rate_numerator = kMediaFps;
    encoder_config.frame_rate_denominator = 1;
    encoder_config.bitrate = size == 320 ? 4'000'000 : 8'000'000;
    encoder_config.gop_size = kMediaFps * 2;
    encoder_config.input_format = bus_format == BusFormat::p010
        ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    encoder_config.low_latency = true;
    encoder_config.input_pool_size = 4;
    encoder_config.event_timeout_ms = 2'000;
    encoder_config.require_video_encoder_input_bind = require_l2_encoder_bind;
    require_encoder(
        gpu::GpuEncoder::probe(device, encoder_config, run.encoder_support),
            std::string("probe ") + codec_name(codec) + " hardware encoder");
    if (!run.encoder_support.supported || !run.encoder_support.d3d11_aware) {
        fail(std::string("no D3D11-aware ") + codec_name(codec)
            + " hardware encoder is available");
    }
    if (planar_bus_format(bus_format)
        && !run.encoder_support.external_planar_input) {
        fail(std::string(codec_name(codec))
            + " hardware encoder does not accept tracked external "
            + evidence_format_name(bus_format) + " textures");
    }
    if (require_l2_encoder_bind
        && (!run.encoder_support.video_encoder_input_bind_supported
            || run.encoder_support.runtime_copy_evidence_capability
                != gpu::GpuCopyEvidenceLevel::
                    l2_external_surface_identity_and_encoder_bind)) {
        fail("hardware encoder cannot qualify VIDEO_ENCODER-bound L2 input");
    }
    require_encoder(
        run.encoder.initialize(
            device,
            encoder_config,
            &PacketTracker::callback,
            &run.packet_tracker),
        std::string("initialize ") + codec_name(codec) + " hardware encoder");
}

gpu::SharedFrameBusConfig bus_config(
    std::uint32_t size,
    BusFormat bus_format,
    bool require_l2_encoder_bind = false) {
    gpu::SharedFrameBusConfig config;
    config.width = size;
    config.height = size;
    config.format = bus_dxgi_format(bus_format);
    config.bind_flags = planar_bus_format(bus_format)
        ? D3D11_BIND_RENDER_TARGET
            | (require_l2_encoder_bind ? D3D11_BIND_VIDEO_ENCODER : 0u)
        : D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    config.slot_count = 4;
    config.max_consumers = 1;
    return config;
}

gpu::WgcMailboxConfig mailbox_config(std::uint32_t size) {
    gpu::WgcMailboxConfig config;
    config.mode = gpu::WgcMailboxMode::centered_region;
    config.width = size;
    config.height = size;
    return config;
}

gpu::SharedFrameBusFrameMetadata staged_metadata(
    const gpu::WgcFrameLease& frame) {
    const gpu::WgcFrameInfo& info = frame.info();
    const gpu::WgcMailboxFrameInfo mailbox = frame.mailbox_info();
    gpu::SharedFrameBusFrameMetadata metadata;
    metadata.valid_fields =
        gpu::shared_frame_bus_metadata_source_timestamp
        | gpu::shared_frame_bus_metadata_qpc
        | gpu::shared_frame_bus_metadata_source_dimensions
        | gpu::shared_frame_bus_metadata_roi
        | gpu::shared_frame_bus_metadata_mailbox_generation
        | gpu::shared_frame_bus_metadata_color_space;
    metadata.source_timestamp_100ns = info.source_timestamp_100ns;
    metadata.timestamp_qpc = info.timestamp_qpc;
    metadata.qpc_frequency = info.qpc_frequency;
    metadata.mailbox_generation = mailbox.generation;
    metadata.source_width = mailbox.source_width;
    metadata.source_height = mailbox.source_height;
    metadata.roi_x = mailbox.x;
    metadata.roi_y = mailbox.y;
    metadata.roi_width = mailbox.width;
    metadata.roi_height = mailbox.height;
    metadata.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    return metadata;
}

void validate_metadata(
    const gpu::SharedFrameBusFrameMetadata& metadata,
    std::uint32_t size,
    BusFormat bus_format,
    CaptureBackend capture_backend) {
    std::uint64_t required = gpu::shared_frame_bus_metadata_qpc
        | gpu::shared_frame_bus_metadata_source_dimensions
        | gpu::shared_frame_bus_metadata_roi
        | gpu::shared_frame_bus_metadata_mailbox_generation
        | gpu::shared_frame_bus_metadata_color_space;
    if (capture_backend == CaptureBackend::wgc) {
        required |= gpu::shared_frame_bus_metadata_source_timestamp;
    }
    if (metadata.structure_size != sizeof(metadata)
        || metadata.metadata_version != gpu::shared_frame_bus_metadata_version
        || (metadata.valid_fields & required) != required
        || (capture_backend == CaptureBackend::wgc
            && metadata.source_timestamp_100ns <= 0)
        || metadata.timestamp_qpc == 0
        || metadata.qpc_frequency == 0
        || metadata.mailbox_generation == 0
        || metadata.source_width < size
        || metadata.source_height < size
        || metadata.roi_width != size
        || metadata.roi_height != size
        || metadata.roi_x + metadata.roi_width > metadata.source_width
        || metadata.roi_y + metadata.roi_height > metadata.source_height
        || metadata.color_space != (planar_bus_format(bus_format)
            ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)) {
        fail("consumer received invalid or incomplete SharedFrameBus metadata");
    }
}

std::uint32_t bounded_timeout_ms(Clock::time_point deadline) noexcept {
    const Clock::time_point now = Clock::now();
    if (now >= deadline) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(remaining + 1, 1, 5));
}

bool feed_staged_bus(RunContext& run, Clock::time_point deadline) {
    if (run.bus_format != BusFormat::bgra) {
        fail("staged topology is only defined for the BGRA A/B baseline");
    }
    gpu::WgcFrameLease frame;
    const gpu::WgcResult acquired = run.capture.acquire_latest(
        bounded_timeout_ms(deadline), frame);
    if (!acquired) {
        if (acquired.status == gpu::WgcStatus::timeout
            || acquired.status == gpu::WgcStatus::no_buffer) {
            return false;
        }
        require_wgc(acquired, "staged WGC acquire");
    }
    const gpu::SharedFrameBusFrameMetadata metadata = staged_metadata(frame);
    const gpu::GpuError published = run.publisher.publish(
        frame.texture(), metadata, 0);
    frame.reset();
    if (!published) {
        if (published.status == gpu::GpuStatus::timeout) return false;
        require_gpu(published, "staged private ROI publish");
    }
    return true;
}

void record_metadata_ages(
    const gpu::SharedFrameBusFrameMetadata& metadata,
    CaptureBackend capture_backend,
    RunResult& result) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (metadata.qpc_frequency == 0
        || static_cast<std::uint64_t>(now.QuadPart) < metadata.timestamp_qpc) {
        return;
    }
    const double capture_age = static_cast<double>(
        static_cast<std::uint64_t>(now.QuadPart) - metadata.timestamp_qpc)
        * 1000.0 / static_cast<double>(metadata.qpc_frequency);
    result.capture_age_ms.push_back(std::max(0.0, capture_age));
    if (capture_backend != CaptureBackend::wgc) return;
    const long double now_100ns = static_cast<long double>(now.QuadPart)
        * 10'000'000.0L / static_cast<long double>(metadata.qpc_frequency);
    const double source_age = static_cast<double>(
        (now_100ns - static_cast<long double>(metadata.source_timestamp_100ns))
        / 10'000.0L);
    // SystemRelativeTime can lead callback QPC by a small presentation-clock
    // skew. Age is lower-bounded at zero, while impossible clock-domain
    // mismatches are still rejected.
    if (source_age >= -1'000.0 && source_age < 60'000.0) {
        result.source_age_ms.push_back(std::max(0.0, source_age));
    }
}

void collect_external_inputs(
    RunContext& run,
    RunResult& result) {
    auto current = run.external_completions.begin();
    while (current != run.external_completions.end()) {
        const auto& completion = *current;
        if (!completion->wait(0)) {
            ++current;
            continue;
        }
        require_gpu(
            completion->result(),
            "release tracked external bus lease");
        if (completion->measured) {
            result.gpu_completion_ms.push_back(
                completion->completion_latency_ms());
        }
        current = run.external_completions.erase(current);
    }
}

bool drain_external_inputs(
    RunContext& run,
    std::uint32_t timeout_ms,
    RunResult& result) {
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        collect_external_inputs(run, result);
        if (run.external_completions.empty()) return true;
        const Clock::time_point now = Clock::now();
        if (now >= deadline) return false;
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now).count();
        (void)run.external_completions.front()->wait(
            static_cast<std::uint32_t>(std::max<std::int64_t>(1, remaining)));
    }
}

bool consume_one(
    RunContext& run,
    std::uint32_t size,
    Clock::time_point deadline,
    bool measured,
    RunResult& result,
    std::unordered_set<std::int64_t>& distinct_timestamps,
    std::uint64_t& previous_sequence) {
    if (planar_bus_format(run.bus_format)) {
        collect_external_inputs(run, result);
    }

    gpu::SharedFrameBusFrameLease lease;
    const gpu::GpuError acquired = run.consumer.acquire_latest(
        bounded_timeout_ms(deadline), lease);
    if (!acquired) {
        if (acquired.status == gpu::GpuStatus::timeout) return false;
        require_gpu(acquired, "consumer acquire_latest");
    }

    const gpu::SharedFrameBusFrameMetadata metadata = lease.metadata();
    validate_metadata(
        metadata, size, run.bus_format, run.capture_backend);
    const DXGI_FORMAT expected_format = bus_dxgi_format(run.bus_format);
    if (lease.info().format != expected_format) {
        fail("consumer acquired a bus texture with the wrong format");
    }
    if (measured) {
        if (result.source_width == 0 && result.source_height == 0) {
            result.source_width = metadata.source_width;
            result.source_height = metadata.source_height;
            result.source_region_x = metadata.roi_x;
            result.source_region_y = metadata.roi_y;
            result.source_region_width = metadata.roi_width;
            result.source_region_height = metadata.roi_height;
            result.mailbox_generation = metadata.mailbox_generation;
        } else if (result.source_width != metadata.source_width
            || result.source_height != metadata.source_height
            || result.source_region_x != metadata.roi_x
            || result.source_region_y != metadata.roi_y
            || result.source_region_width != metadata.roi_width
            || result.source_region_height != metadata.roi_height
            || result.mailbox_generation != metadata.mailbox_generation) {
            fail("capture geometry/epoch changed inside the measured window");
        }
        ++result.consumer_frames;
        if (result.first_bus_sequence == 0) {
            result.first_bus_sequence = lease.info().sequence;
        }
        result.last_bus_sequence = lease.info().sequence;
        distinct_timestamps.insert(run.capture_backend == CaptureBackend::wgc
            ? metadata.source_timestamp_100ns
            : static_cast<std::int64_t>(metadata.timestamp_qpc));
        if (previous_sequence != 0 && lease.info().sequence > previous_sequence + 1) {
            result.bus_gaps += lease.info().sequence - previous_sequence - 1;
        }
        previous_sequence = lease.info().sequence;
        record_metadata_ages(metadata, run.capture_backend, result);
    }

    const std::int64_t timestamp = static_cast<std::int64_t>(
        run.next_media_frame++) * run.frame_duration_100ns;
    run.packet_tracker.submit(timestamp, measured);

    gpu::GpuEncoderResult encoded;
    if (planar_bus_format(run.bus_format)) {
        ID3D11Texture2D* input = lease.texture();
        auto completion = std::make_shared<ExternalBusCompletion>();
        completion->submitted = Clock::now();
        completion->measured = measured;
        auto lifetime = std::make_shared<ExternalBusLeaseToken>(
            std::move(lease), completion);
        encoded = run.encoder.submit_external_texture(
            input,
            std::move(lifetime),
            timestamp,
            run.frame_duration_100ns,
            run.next_media_frame == 1);
        if (encoded) {
            run.external_completions.push_back(std::move(completion));
        }
    } else {
        // This queues the only consumer-side read of the shared bus texture.
        const gpu::GpuError transformed = run.transform.process(lease.texture());
        if (transformed) run.gpu_completion.submit(measured);
        const gpu::GpuError released = run.consumer.release(lease);
        require_gpu(transformed, "bus texture to NV12 transform");
        require_gpu(released, "bus lease release after transform submission");
        encoded = run.encoder.encode_texture(
            run.transform.output_texture(),
            timestamp,
            run.frame_duration_100ns,
            run.next_media_frame == 1);
    }
    if (!encoded) run.packet_tracker.cancel(timestamp);
    require_encoder(encoded, "hardware encode");
    if (run.bus_format == BusFormat::bgra) {
        run.gpu_completion.collect_ready();
    }
    return true;
}

void process_window(
    RunContext& run,
    std::uint32_t size,
    Clock::time_point deadline,
    bool measured,
    RunResult& result,
    std::unordered_set<std::int64_t>& distinct_timestamps,
    std::uint64_t& previous_sequence) {
    while (Clock::now() < deadline) {
        if (run.topology == Topology::staged
            && !feed_staged_bus(run, deadline)) {
            continue;
        }
        (void)consume_one(
            run,
            size,
            deadline,
            measured,
            result,
            distinct_timestamps,
            previous_sequence);
    }
}

void send_child_samples(
    HANDLE output,
    ChildSampleKind kind,
    const std::vector<double>& samples,
    std::uint64_t run_nonce,
    std::uint32_t topology) {
    std::size_t offset = 0;
    while (offset < samples.size()) {
        ChildSampleChunk chunk;
        chunk.kind = kind;
        chunk.run_nonce = run_nonce;
        chunk.topology = topology;
        chunk.first_sample = offset;
        chunk.count = static_cast<std::uint32_t>(std::min<std::size_t>(
            kChildSamplesPerChunk, samples.size() - offset));
        std::copy_n(
            samples.data() + offset,
            chunk.count,
            chunk.samples.data());
        if (!write_exact(output, &chunk, sizeof(chunk))) {
            fail("child result sample pipe write failed");
        }
        offset += chunk.count;
    }
}

ChildResultHeader child_result_error(
    std::uint32_t consumer_index,
    std::uint64_t run_nonce,
    std::uint32_t topology,
    std::string_view message) noexcept {
    ChildResultHeader result;
    result.consumer_index = consumer_index;
    result.run_nonce = run_nonce;
    result.topology = topology;
    result.result_send_qpc = qpc_now();
    copy_message(result.message, message);
    return result;
}

int run_consumer_child(HANDLE input, HANDLE output) noexcept {
    RunContext run;
    std::uint32_t consumer_index = 0;
    std::uint32_t topology = 0;
    std::uint64_t run_nonce = 0;
    bool ready_sent = false;
    bool armed_sent = false;
    bool result_sent = false;
    bool closed_sent = false;
    try {
        ChildSetup setup;
        if (!read_exact(input, &setup, sizeof(setup))) {
            fail("child did not receive setup");
        }
        if (setup.structure_size != sizeof(ChildSetup)
            || setup.wire_version != kChildWireVersion
            || (setup.size != 320 && setup.size != 640)
            || !valid_wire_topology(setup.topology)
            || setup.test_fault > static_cast<std::uint32_t>(
                ChildFault::close_response_after_ready)
            || !valid_wire_bus_format(setup.bus_format)
            || (setup.bus_format == static_cast<std::uint32_t>(BusFormat::nv12)
                && setup.topology != wire_topology(Topology::direct))
            || setup.run_nonce == 0
            || setup.registration.structure_size
                != sizeof(gpu::SharedFrameBusRegistration)
            || setup.registration.protocol_version
                != gpu::shared_frame_bus_protocol_version
            || setup.registration.format != static_cast<std::uint32_t>(
                setup.bus_format == static_cast<std::uint32_t>(BusFormat::nv12)
                    ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM)) {
            fail("child received malformed setup");
        }
        consumer_index = setup.registration.consumer_index;
        topology = setup.topology;
        run_nonce = setup.run_nonce;
        run.topology = static_cast<Topology>(topology);
        run.bus_format = static_cast<BusFormat>(setup.bus_format);
        const ChildFault fault = static_cast<ChildFault>(setup.test_fault);
        if (fault == ChildFault::exit_before_ready) return 70;
        DeviceContext d3d = create_device_for_registration(setup.registration);
        require_gpu(
            gpu::SharedFrameBusConsumer::open(
                d3d.device.Get(), setup.registration, true, run.consumer),
            "open cross-process SharedFrameBus consumer");
        initialize_consumer_pipeline(
            run,
            d3d.device.Get(),
            d3d.context.Get(),
            setup.size,
            run.bus_format,
            gpu::VideoCodec::h264);

        ChildReady ready;
        ready.success = 1;
        ready.consumer_index = consumer_index;
        ready.topology = topology;
        ready.child_process_id = GetCurrentProcessId();
        ready.run_nonce = run_nonce;
        ready.gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::ok);
        ready.hresult = S_OK;
        copy_message(ready.message, "ok");
        if (!write_exact(output, &ready, sizeof(ready))) return 3;
        ready_sent = true;
        if (fault == ChildFault::hang_after_ready) Sleep(INFINITE);
        if (fault == ChildFault::close_response_after_ready) {
            CloseHandle(output);
            output = nullptr;
            Sleep(INFINITE);
        }

        ChildMeasure measure;
        if (!read_exact(input, &measure, sizeof(measure))) {
            fail("child did not receive measure command");
        }
        const std::uint64_t received_qpc = qpc_now();
        const std::uint64_t local_frequency = qpc_frequency();
        if (measure.structure_size != sizeof(ChildMeasure)
            || measure.wire_version != kChildWireVersion
            || measure.command != kChildMeasureCommand
            || measure.topology != topology
            || measure.run_nonce != run_nonce
            || measure.qpc_frequency != local_frequency
            || measure.measure_begin_qpc <= received_qpc
            || measure.measure_end_qpc <= measure.measure_begin_qpc) {
            fail("child received malformed or late measure command");
        }
        ChildArmed armed;
        armed.success = 1;
        armed.consumer_index = consumer_index;
        armed.topology = topology;
        armed.run_nonce = run_nonce;
        armed.gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::ok);
        armed.hresult = S_OK;
        armed.command_received_qpc = received_qpc;
        armed.armed_sent_qpc = qpc_now();
        copy_message(armed.message, "ok");
        if (!write_exact(output, &armed, sizeof(armed))) return 4;
        armed_sent = true;

        RunResult measured_result;
        measured_result.topology = run.topology;
        measured_result.consumer_mode = ConsumerMode::child;
        measured_result.bus_format = run.bus_format;
        measured_result.consumer_process_id = GetCurrentProcessId();
        measured_result.mft_internal_copy_observable =
            run.encoder_support.mft_internal_copy_observable;
        measured_result.encoder_matching_transform_count =
            run.encoder_support.matching_transform_count;
        std::unordered_set<std::int64_t> distinct_timestamps;
        std::uint64_t previous_sequence = 0;
        std::uint64_t actual_begin_qpc = 0;
        gpu::GpuEncoderStats encoder_before{};
        bool encoder_before_recorded = false;
        for (;;) {
            const std::uint64_t now = qpc_now();
            if (now >= measure.measure_end_qpc) break;
            const bool measured = now >= measure.measure_begin_qpc;
            if (measured && actual_begin_qpc == 0) {
                actual_begin_qpc = now;
                encoder_before = run.encoder.stats();
                encoder_before_recorded = true;
            }
            (void)consume_one(
                run,
                setup.size,
                Clock::now() + std::chrono::milliseconds(5),
                measured,
                measured_result,
                distinct_timestamps,
                previous_sequence);
        }
        const std::uint64_t actual_end_qpc = qpc_now();
        if (actual_begin_qpc == 0) actual_begin_qpc = actual_end_qpc;
        if (run.bus_format == BusFormat::bgra) run.gpu_completion.drain();
        require_encoder(run.encoder.drain(), "drain child H.264 encoder");
        const gpu::GpuEncoderStats encoder_after = run.encoder.stats();
        if (planar_bus_format(run.bus_format)) {
            // Some hardware MFTs retain their final drained input until
            // END_STREAMING/transform destruction. close() performs that
            // protocol without weakening the tracked-token lifetime.
            run.encoder.close();
            if (!drain_external_inputs(run, 5'000, measured_result)) {
                fail("tracked external encoder inputs did not release their bus leases");
            }
        }
        if (!encoder_before_recorded) encoder_before = encoder_after;

        measured_result.distinct_source_timestamps = distinct_timestamps.size();
        measured_result.encoder_copied_submissions = delta(
            encoder_after.copied_submissions,
            encoder_before.copied_submissions);
        measured_result.encoder_direct_submissions = delta(
            encoder_after.direct_submissions,
            encoder_before.direct_submissions);
        measured_result.encoder_external_submissions = delta(
            encoder_after.external_submissions,
            encoder_before.external_submissions);
        measured_result.encoder_external_identity_verified_submissions = delta(
            encoder_after.external_identity_verified_submissions,
            encoder_before.external_identity_verified_submissions);
        measured_result.encoder_external_video_encoder_bound_submissions = delta(
            encoder_after.external_video_encoder_bound_submissions,
            encoder_before.external_video_encoder_bound_submissions);
        if (run.bus_format == BusFormat::bgra) {
            measured_result.gpu_query_dropped =
                run.gpu_completion.dropped_samples();
            measured_result.gpu_completion_ms =
                run.gpu_completion.latency_ms();
        }
        {
            std::lock_guard lock(run.packet_tracker.mutex);
            measured_result.packet_submissions =
                run.packet_tracker.measured_submissions;
            measured_result.packets = run.packet_tracker.measured_packets;
            measured_result.packet_bytes = run.packet_tracker.measured_bytes;
            measured_result.packet_codec_mismatches =
                run.packet_tracker.codec_mismatches;
            measured_result.packet_latency_ms =
                run.packet_tracker.packet_latency_ms;
        }
        if (measured_result.packet_codec_mismatches != 0) {
            fail("child encoder returned a codec different from its request");
        }
        if (measured_result.consumer_frames == 0) {
            fail("child measured window consumed no bus frames");
        }

        ChildResultHeader result;
        result.success = 1;
        result.consumer_index = consumer_index;
        result.topology = topology;
        result.run_nonce = run_nonce;
        result.gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::ok);
        result.hresult = S_OK;
        result.source_width = measured_result.source_width;
        result.source_height = measured_result.source_height;
        result.consumer_frames = measured_result.consumer_frames;
        result.distinct_source_timestamps =
            measured_result.distinct_source_timestamps;
        result.bus_gaps = measured_result.bus_gaps;
        result.first_sequence = measured_result.first_bus_sequence;
        result.last_sequence = measured_result.last_bus_sequence;
        result.packet_submissions = measured_result.packet_submissions;
        result.packets = measured_result.packets;
        result.packet_bytes = measured_result.packet_bytes;
        result.encoder_copied_submissions =
            measured_result.encoder_copied_submissions;
        result.encoder_direct_submissions =
            measured_result.encoder_direct_submissions;
        result.encoder_external_submissions =
            measured_result.encoder_external_submissions;
        result.encoder_external_identity_verified_submissions =
            measured_result.encoder_external_identity_verified_submissions;
        result.encoder_external_video_encoder_bound_submissions =
            measured_result.encoder_external_video_encoder_bound_submissions;
        result.gpu_query_dropped = measured_result.gpu_query_dropped;
        result.source_age_samples = measured_result.source_age_ms.size();
        result.capture_age_samples = measured_result.capture_age_ms.size();
        result.gpu_completion_samples = measured_result.gpu_completion_ms.size();
        result.packet_latency_samples = measured_result.packet_latency_ms.size();
        result.scheduled_begin_qpc = measure.measure_begin_qpc;
        result.scheduled_end_qpc = measure.measure_end_qpc;
        result.actual_begin_qpc = actual_begin_qpc;
        result.actual_end_qpc = actual_end_qpc;
        copy_message(result.message, "ok");
        result.result_send_qpc = qpc_now();
        if (!write_exact(output, &result, sizeof(result))) return 5;
        result_sent = true;
        send_child_samples(
            output, ChildSampleKind::source_age, measured_result.source_age_ms,
            run_nonce, topology);
        send_child_samples(
            output, ChildSampleKind::capture_age, measured_result.capture_age_ms,
            run_nonce, topology);
        send_child_samples(
            output,
            ChildSampleKind::gpu_completion,
            measured_result.gpu_completion_ms,
            run_nonce,
            topology);
        send_child_samples(
            output,
            ChildSampleKind::packet_latency,
            measured_result.packet_latency_ms,
            run_nonce,
            topology);

        ChildCommand close;
        if (!read_exact(input, &close, sizeof(close))
            || close.structure_size != sizeof(ChildCommand)
            || close.wire_version != kChildWireVersion
            || close.command != kChildCloseCommand
            || close.topology != topology
            || close.run_nonce != run_nonce) {
            return 6;
        }
        const gpu::GpuError closed = run.consumer.close();
        ChildClosed close_response;
        close_response.consumer_index = consumer_index;
        close_response.topology = topology;
        close_response.run_nonce = run_nonce;
        close_response.success = closed ? 1u : 0u;
        close_response.gpu_status = static_cast<std::uint32_t>(closed.status);
        close_response.hresult = closed.hresult;
        copy_message(
            close_response.message,
            closed ? std::string_view("ok") : std::string_view(closed.what()));
        if (!write_exact(output, &close_response, sizeof(close_response))) return 7;
        closed_sent = true;

        ChildCommand exit;
        if (!read_exact(input, &exit, sizeof(exit))
            || exit.structure_size != sizeof(ChildCommand)
            || exit.wire_version != kChildWireVersion
            || exit.command != kChildExitCommand
            || exit.topology != topology
            || exit.run_nonce != run_nonce) {
            return 8;
        }
        return closed ? 0 : 9;
    } catch (const std::exception& error) {
        // Tear down the MFT before closing the bus consumer so every tracked
        // token gets its authoritative chance to release the lease.
        run.encoder.close();
        if (run.consumer.initialized()) (void)run.consumer.close();
        if (!ready_sent) {
            ChildReady ready;
            ready.consumer_index = consumer_index;
            ready.topology = topology;
            ready.child_process_id = GetCurrentProcessId();
            ready.run_nonce = run_nonce;
            copy_message(ready.message, error.what());
            (void)write_exact(output, &ready, sizeof(ready));
        } else if (!armed_sent) {
            ChildArmed armed;
            armed.consumer_index = consumer_index;
            armed.topology = topology;
            armed.run_nonce = run_nonce;
            armed.command_received_qpc = qpc_now();
            armed.armed_sent_qpc = armed.command_received_qpc;
            copy_message(armed.message, error.what());
            (void)write_exact(output, &armed, sizeof(armed));
        } else if (!result_sent) {
            const ChildResultHeader result = child_result_error(
                consumer_index, run_nonce, topology, error.what());
            (void)write_exact(output, &result, sizeof(result));
        } else if (!closed_sent) {
            ChildClosed closed;
            closed.consumer_index = consumer_index;
            closed.topology = topology;
            closed.run_nonce = run_nonce;
            copy_message(closed.message, error.what());
            (void)write_exact(output, &closed, sizeof(closed));
        } else {
            // The parent has already received terminal protocol state.
        }
        return 2;
    }
}

std::uint32_t remaining_control_timeout_ms(
    Clock::time_point deadline,
    std::string_view phase) {
    const Clock::time_point now = Clock::now();
    if (now >= deadline) {
        fail(std::string(phase) + " exceeded the IPC timeout");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        remaining + 1,
        1,
        std::numeric_limits<std::uint32_t>::max()));
}

void receive_child_samples(
    ChildProcess& child,
    ChildSampleKind expected_kind,
    std::uint64_t expected_count,
    std::uint64_t run_nonce,
    std::uint32_t topology,
    std::uint32_t duration_ms,
    Clock::time_point deadline,
    std::vector<double>& output) {
    const std::uint64_t maximum_count = std::max(
        kMinimumChildLatencySampleLimit,
        static_cast<std::uint64_t>(duration_ms)
            * kMaximumMeasuredFramesPerMillisecond);
    if (expected_count > maximum_count) {
        fail("child advertised an unreasonable latency sample count");
    }
    output.clear();
    output.reserve(static_cast<std::size_t>(expected_count));
    while (output.size() < expected_count) {
        const ChildSampleChunk chunk = child.receive<ChildSampleChunk>(
            remaining_control_timeout_ms(deadline, "child result transfer"),
            "latency sample chunk");
        const std::uint64_t remaining = expected_count - output.size();
        if (chunk.structure_size != sizeof(ChildSampleChunk)
            || chunk.wire_version != kChildWireVersion
            || chunk.kind != expected_kind
            || chunk.run_nonce != run_nonce
            || chunk.topology != topology
            || chunk.reserved != 0
            || chunk.first_sample != output.size()
            || chunk.count == 0
            || chunk.count > kChildSamplesPerChunk
            || chunk.count > remaining) {
            fail("child returned a malformed latency sample chunk");
        }
        for (std::uint32_t index = 0; index < chunk.count; ++index) {
            const double sample = chunk.samples[index];
            if (!std::isfinite(sample)
                || sample < 0.0
                || sample > kMaximumLatencySampleMs) {
                fail("child returned an invalid latency sample");
            }
        }
        output.insert(
            output.end(),
            chunk.samples.begin(),
            chunk.samples.begin() + chunk.count);
    }
}

void produce_until_qpc(
    RunContext& run,
    std::uint64_t deadline_qpc,
    ChildProcess* child) {
    while (qpc_now() < deadline_qpc) {
        if (child != nullptr
            && WaitForSingleObject(child->process(), 0) == WAIT_OBJECT_0) {
            fail("child consumer exited while the parent was producing frames");
        }
        if (run.topology == Topology::staged) {
            (void)feed_staged_bus(
                run, Clock::now() + std::chrono::milliseconds(5));
        } else {
            Sleep(1);
        }
    }
}

RunResult run_topology_inproc(
    Topology topology,
    std::uint32_t pair,
    std::string order,
    const Options& options,
    WindowThread& source,
    DeviceContext& d3d,
    const MonitorTarget* monitor_target) {
    if (planar_bus_format(options.bus_format)
        && topology != Topology::direct) {
        fail("planar benchmark mode supports direct topology only");
    }
    RunContext run;
    run.topology = topology;
    run.bus_format = options.bus_format;
    run.capture_backend = options.capture_backend;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(),
            bus_config(
                options.size,
                options.bus_format,
                options.runtime_evidence_json.has_value()),
            run.publisher),
        "create SharedFrameBus");
    require_gpu(
        run.publisher.register_consumer(
            GetCurrentProcess(), run.registration),
        "register same-process consumer");
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), run.registration, true, run.consumer),
        "open same-process consumer");
    initialize_consumer_pipeline(
        run,
        d3d.device.Get(),
        d3d.context.Get(),
        options.size,
        options.bus_format,
        options.codec,
        options.runtime_evidence_json.has_value());

    gpu::WgcCaptureOptions capture_options;
    capture_options.buffer_count = 3;
    capture_options.include_cursor = false;
    capture_options.require_border = false;
    capture_options.include_secondary_windows = false;
    capture_options.min_update_interval_us = 0;
    if (planar_bus_format(options.bus_format)) {
        capture_options.planar_transform_backend = options.planar_backend;
    }
    const gpu::WgcMailboxConfig mailbox = mailbox_config(options.source_size);
    if (options.capture_backend == CaptureBackend::desktop_duplication) {
        if (topology != Topology::direct || monitor_target == nullptr) {
            fail("Desktop Duplication requires a resolved direct monitor target");
        }
        require_wgc(
            gpu::DesktopDuplicationCapture::create_for_monitor_to_bus(
                monitor_target->monitor,
                run.publisher,
                capture_options,
                mailbox,
                run.desktop_duplication),
            "create direct-to-bus Desktop Duplication capture");
    } else if (topology == Topology::staged) {
        require_wgc(
            gpu::WgcCapture::create_for_window(
                source.window(),
                d3d.device.Get(),
                capture_options,
                mailbox,
                run.capture),
            "create staged private ROI WGC capture");
    } else {
        require_wgc(
            gpu::WgcCapture::create_for_window_to_bus(
                source.window(),
                run.publisher,
                capture_options,
                mailbox,
                run.capture),
            "create direct-to-bus WGC capture");
    }
    require_wgc(start_active_capture(run), "start active capture backend");
    source.require_healthy("capture startup");

    RunResult result;
    result.topology = topology;
    result.consumer_mode = ConsumerMode::inproc;
    result.bus_format = options.bus_format;
    result.capture_backend = options.capture_backend;
    result.codec = options.codec;
    result.planar_backend = options.planar_backend;
    result.consumer_process_id = GetCurrentProcessId();
    result.mft_internal_copy_observable =
        run.encoder_support.mft_internal_copy_observable;
    result.encoder_matching_transform_count =
        run.encoder_support.matching_transform_count;
    result.encoder_mft_identity = run.encoder.mft_identity();
    if (monitor_target != nullptr) {
        result.output_index = monitor_target->output_index;
        result.output_device_name = monitor_target->device_name;
        result.output_desktop = monitor_target->desktop;
        result.output_rotation = monitor_target->rotation;
        result.logical_monitor_width = monitor_target->logical_width;
        result.logical_monitor_height = monitor_target->logical_height;
        result.capture_surface_width = monitor_target->surface_width;
        result.capture_surface_height = monitor_target->surface_height;
    }
    result.pair = pair;
    result.order = std::move(order);
    std::unordered_set<std::int64_t> distinct_timestamps;
    std::uint64_t previous_sequence = 0;

    if (options.warmup_ms != 0) {
        process_window(
            run,
            options.source_size,
            Clock::now() + std::chrono::milliseconds(options.warmup_ms),
            false,
            result,
            distinct_timestamps,
            previous_sequence);
        require_active_capture_healthy(run);
        source.require_healthy("warmup");
    }
    distinct_timestamps.clear();
    previous_sequence = 0;

    const gpu::WgcCaptureStats capture_before = active_capture_stats(run);
    const std::uint64_t desktop_presents_before =
        active_desktop_present_frames(run);
    const gpu::SharedFrameBusStats bus_before = run.publisher.stats();
    const gpu::GpuEncoderStats encoder_before = run.encoder.stats();
    const std::uint64_t source_before = source.presentations();
    const Clock::time_point measured_begin = Clock::now();
    process_window(
        run,
        options.source_size,
        measured_begin + std::chrono::milliseconds(options.duration_ms),
        true,
        result,
        distinct_timestamps,
        previous_sequence);
    require_active_capture_healthy(run);
    source.require_healthy("measured capture");
    const Clock::time_point measured_end = Clock::now();
    const std::uint64_t source_after = source.presentations();
    const gpu::SharedFrameBusStats bus_after = run.publisher.stats();
    const gpu::WgcCaptureStats capture_after = active_capture_stats(run);
    const std::uint64_t desktop_presents_after =
        active_desktop_present_frames(run);

    stop_active_capture(run);
    if (options.bus_format == BusFormat::bgra) run.gpu_completion.drain();
    require_encoder(
        run.encoder.drain(),
        std::string("drain ") + codec_name(options.codec)
            + " hardware encoder");
    const gpu::GpuEncoderStats encoder_after = run.encoder.stats();
    if (planar_bus_format(options.bus_format)) {
        run.encoder.close();
        if (!drain_external_inputs(run, 5'000, result)) {
            fail("tracked external encoder inputs did not release their bus leases");
        }
    }
    const gpu::WgcCaptureStats capture_lifetime = active_capture_stats(run);
    const std::uint64_t desktop_presents_lifetime =
        active_desktop_present_frames(run);
    const gpu::SharedFrameBusStats bus_lifetime = run.publisher.stats();
    if (monitor_target != nullptr) {
        const MonitorTarget final_target = select_monitor_target(
            d3d.device.Get(), monitor_target->output_index);
        if (!same_monitor_target(*monitor_target, final_target)) {
            fail("monitor adapter/output geometry changed during qualification");
        }
    }
    result.elapsed_seconds = seconds(measured_end - measured_begin);
    result.source_presentations = delta(source_after, source_before);
    result.wgc_received = delta(
        capture_after.received_frames, capture_before.received_frames);
    result.wgc_published = delta(
        capture_after.published_frames, capture_before.published_frames);
    result.wgc_private_overwritten = delta(
        capture_after.overwritten_frames, capture_before.overwritten_frames);
    result.wgc_skipped_no_buffer = delta(
        capture_after.skipped_no_buffer, capture_before.skipped_no_buffer);
    result.desktop_present_frames = delta(
        desktop_presents_after,
        desktop_presents_before);
    result.producer_ingress_copies = delta(
        capture_after.ingress_copy_submissions,
        capture_before.ingress_copy_submissions);
    result.producer_ingress_transforms = delta(
        capture_after.ingress_transform_submissions,
        capture_before.ingress_transform_submissions);
    result.bus_published = delta(
        bus_after.published_frames, bus_before.published_frames);
    result.bus_no_slot = delta(bus_after.no_slot, bus_before.no_slot);
    result.derived_control_contention = topology == Topology::direct
        ? delta(result.wgc_skipped_no_buffer, result.bus_no_slot)
        : 0;
    result.distinct_source_timestamps = distinct_timestamps.size();
    result.encoder_copied_submissions = delta(
        encoder_after.copied_submissions,
        encoder_before.copied_submissions);
    result.encoder_direct_submissions = delta(
        encoder_after.direct_submissions,
        encoder_before.direct_submissions);
    result.encoder_external_submissions = delta(
        encoder_after.external_submissions,
        encoder_before.external_submissions);
    result.encoder_external_identity_verified_submissions = delta(
        encoder_after.external_identity_verified_submissions,
        encoder_before.external_identity_verified_submissions);
    result.encoder_external_video_encoder_bound_submissions = delta(
        encoder_after.external_video_encoder_bound_submissions,
        encoder_before.external_video_encoder_bound_submissions);
    result.lifetime_producer_ingress_copies =
        capture_lifetime.ingress_copy_submissions;
    result.lifetime_producer_ingress_transforms =
        capture_lifetime.ingress_transform_submissions;
    result.lifetime_desktop_present_frames =
        desktop_presents_lifetime;
    result.lifetime_bus_published = bus_lifetime.published_frames;
    result.lifetime_bus_copied_publishes = bus_lifetime.copied_publishes;
    result.lifetime_bus_direct_publishes = bus_lifetime.direct_publishes;
    result.lifetime_encoder_copied_submissions =
        encoder_after.copied_submissions;
    result.lifetime_encoder_direct_submissions =
        encoder_after.direct_submissions;
    result.lifetime_encoder_external_submissions =
        encoder_after.external_submissions;
    result.lifetime_encoder_external_identity_verified_submissions =
        encoder_after.external_identity_verified_submissions;
    result.lifetime_encoder_external_video_encoder_bound_submissions =
        encoder_after.external_video_encoder_bound_submissions;
    if (options.bus_format == BusFormat::bgra) {
        result.gpu_query_dropped = run.gpu_completion.dropped_samples();
        result.gpu_completion_ms = run.gpu_completion.latency_ms();
    }
    {
        std::lock_guard lock(run.packet_tracker.mutex);
        result.packet_submissions = run.packet_tracker.measured_submissions;
        result.packets = run.packet_tracker.measured_packets;
        result.packet_bytes = run.packet_tracker.measured_bytes;
        result.packet_codec_mismatches =
            run.packet_tracker.codec_mismatches;
        result.packet_latency_ms = run.packet_tracker.packet_latency_ms;
    }

    require_gpu(run.consumer.close(), "close same-process consumer");
    require_gpu(
        run.publisher.unregister_consumer(run.registration, 2'000),
        "unregister same-process consumer");
    if (result.consumer_frames == 0) {
        fail(std::string(topology_name(topology))
            + " measured window consumed no bus frames");
    }
    validate_completed_run(result);
    return result;
}

RunResult run_topology_child(
    Topology topology,
    std::uint32_t pair,
    std::string order,
    const Options& options,
    WindowThread& source,
    DeviceContext& d3d,
    const MonitorTarget*) {
    if (planar_bus_format(options.bus_format)
        && topology != Topology::direct) {
        fail("planar benchmark mode supports direct topology only");
    }
    RunContext run;
    run.topology = topology;
    run.bus_format = options.bus_format;
    std::optional<ChildProcess> child;
    bool registered = false;
    bool capture_started = false;
    bool child_exited = false;

    auto emergency_cleanup = [&]() {
        std::string failure;
        if (capture_started) {
            run.capture.stop();
            capture_started = false;
        }
        if (child.has_value() && !child_exited) {
            child->terminate(0xfcab0001u);
            child_exited = true;
        }
        if (registered) {
            gpu::GpuError unregistered;
            for (int attempt = 0; attempt != 3; ++attempt) {
                unregistered = run.publisher.unregister_consumer(
                    run.registration, options.ipc_timeout_ms);
                if (unregistered) break;
                Sleep(10);
            }
            const std::uint64_t active = run.publisher.initialized()
                ? run.publisher.stats().active_consumers : 0;
            if (!unregistered && active != 0) {
                failure = "emergency unregister failed: "
                    + gpu_error_text(unregistered);
            } else {
                registered = false;
            }
        }
        if (run.publisher.initialized()) {
            const gpu::SharedFrameBusStats cleanup_stats = run.publisher.stats();
            if (cleanup_stats.active_consumers != 0) {
                if (!failure.empty()) failure += "; ";
                failure += "SharedFrameBus retained an active consumer after cleanup";
            }
            if (cleanup_stats.quarantined_slots != 0) {
                if (!failure.empty()) failure += "; ";
                failure += "SharedFrameBus quarantined a slot during cleanup";
            }
        }
        return failure;
    };

    try {
        require_gpu(
            gpu::SharedFrameBusPublisher::create(
                d3d.device.Get(),
                bus_config(options.size, options.bus_format),
                run.publisher),
            "create child-consumer SharedFrameBus");
        child.emplace(ChildProcess::spawn(L"--consumer-child"));
        require_gpu(
            run.publisher.register_consumer(
                child->process(), run.registration),
            "register child-process consumer");
        registered = true;

        RunResult result;
        result.topology = topology;
        result.consumer_mode = ConsumerMode::child;
        result.bus_format = options.bus_format;
        result.capture_backend = options.capture_backend;
        result.codec = options.codec;
        result.planar_backend = options.planar_backend;
        result.pair = pair;
        result.order = std::move(order);
        result.consumer_process_id = child->process_id();
        const std::uint32_t topology_wire = wire_topology(topology);
        const std::uint64_t run_nonce = make_run_nonce(topology, pair);

        ChildSetup setup;
        setup.size = options.size;
        setup.topology = topology_wire;
        setup.test_fault = static_cast<std::uint32_t>(options.child_fault);
        setup.bus_format = static_cast<std::uint32_t>(options.bus_format);
        setup.run_nonce = run_nonce;
        setup.registration = run.registration;
        const Clock::time_point setup_begin = Clock::now();
        child->send(setup, "child setup");
        const ChildReady ready = child->receive<ChildReady>(
            options.ipc_timeout_ms, "child ready");
        result.setup_ready_ms = milliseconds(Clock::now() - setup_begin);
        if (ready.structure_size != sizeof(ChildReady)
            || ready.wire_version != kChildWireVersion
            || ready.success == 0
            || ready.consumer_index != run.registration.consumer_index
            || ready.topology != topology_wire
            || ready.run_nonce != run_nonce
            || ready.child_process_id != child->process_id()
            || ready.child_process_id == GetCurrentProcessId()
            || ready.gpu_status != static_cast<std::uint32_t>(gpu::GpuStatus::ok)
            || ready.hresult != S_OK) {
            fail("child consumer initialization failed: "
                + std::string(ready.message) + " (status="
                + std::to_string(ready.gpu_status) + ", hr="
                + std::to_string(static_cast<long long>(ready.hresult)) + ')');
        }

        gpu::WgcCaptureOptions capture_options;
        capture_options.buffer_count = 3;
        capture_options.include_cursor = false;
        capture_options.require_border = false;
        capture_options.include_secondary_windows = false;
        capture_options.min_update_interval_us = 0;
        const gpu::WgcMailboxConfig mailbox = mailbox_config(options.source_size);
        if (topology == Topology::staged) {
            require_wgc(
                gpu::WgcCapture::create_for_window(
                    source.window(),
                    d3d.device.Get(),
                    capture_options,
                    mailbox,
                    run.capture),
                "create staged private ROI WGC capture");
        } else {
            require_wgc(
                gpu::WgcCapture::create_for_window_to_bus(
                    source.window(),
                    run.publisher,
                    capture_options,
                    mailbox,
                    run.capture),
                "create direct-to-bus WGC capture");
        }
        require_wgc(run.capture.start(), "start child-consumer WGC capture");
        capture_started = true;
        source.require_healthy("child-consumer capture startup");

        const std::uint64_t frequency = qpc_frequency();
        const std::uint64_t maximum_boundary_skew_ticks = qpc_ticks_for_ms(
            kMaximumBoundarySkewMs, frequency);
        ChildMeasure measure;
        measure.topology = topology_wire;
        measure.run_nonce = run_nonce;
        measure.qpc_frequency = frequency;
        measure.measure_begin_qpc = qpc_now()
            + qpc_ticks_for_ms(
                options.warmup_ms + kChildArmLeadMs, frequency);
        measure.measure_end_qpc = measure.measure_begin_qpc
            + qpc_ticks_for_ms(options.duration_ms, frequency);
        const Clock::time_point arm_begin = Clock::now();
        child->send(measure, "measure command");
        const ChildArmed armed = child->receive<ChildArmed>(
            options.ipc_timeout_ms, "measure armed");
        const std::uint64_t armed_received_qpc = qpc_now();
        result.measure_armed_rtt_ms = milliseconds(Clock::now() - arm_begin);
        if (armed.structure_size != sizeof(ChildArmed)
            || armed.wire_version != kChildWireVersion
            || armed.success == 0
            || armed.consumer_index != run.registration.consumer_index
            || armed.topology != topology_wire
            || armed.run_nonce != run_nonce
            || armed.gpu_status != static_cast<std::uint32_t>(gpu::GpuStatus::ok)
            || armed.hresult != S_OK
            || armed.command_received_qpc == 0
            || armed.armed_sent_qpc < armed.command_received_qpc
            || armed.armed_sent_qpc >= measure.measure_begin_qpc
            || armed_received_qpc < armed.armed_sent_qpc
            || armed_received_qpc >= measure.measure_begin_qpc) {
            fail("child returned a malformed or late armed response: "
                + std::string(armed.message) + " (status="
                + std::to_string(armed.gpu_status) + ", hr="
                + std::to_string(static_cast<long long>(armed.hresult)) + ')');
        }

        produce_until_qpc(run, measure.measure_begin_qpc, &*child);
        source.require_healthy("child-consumer pre-measure window");
        const std::uint64_t parent_actual_begin_qpc = qpc_now();
        const gpu::WgcCaptureStats capture_before = run.capture.stats();
        const gpu::SharedFrameBusStats bus_before = run.publisher.stats();
        const std::uint64_t source_before = source.presentations();

        produce_until_qpc(run, measure.measure_end_qpc, &*child);
        source.require_healthy("child-consumer measured window");
        const std::uint64_t parent_actual_end_qpc = qpc_now();
        const std::uint64_t source_after = source.presentations();
        const gpu::SharedFrameBusStats bus_after = run.publisher.stats();
        const gpu::WgcCaptureStats capture_after = run.capture.stats();
        run.capture.stop();
        capture_started = false;

        const Clock::time_point result_deadline = Clock::now()
            + std::chrono::milliseconds(options.ipc_timeout_ms);
        const ChildResultHeader child_result =
            child->receive<ChildResultHeader>(
                remaining_control_timeout_ms(
                    result_deadline, "child result transfer"),
                "child result header");
        const std::uint64_t result_header_received_qpc = qpc_now();
        if (child_result.structure_size != sizeof(ChildResultHeader)
            || child_result.wire_version != kChildWireVersion
            || child_result.success == 0
            || child_result.consumer_index != run.registration.consumer_index
            || child_result.topology != topology_wire
            || child_result.reserved != 0
            || child_result.run_nonce != run_nonce
            || child_result.gpu_status
                != static_cast<std::uint32_t>(gpu::GpuStatus::ok)
            || child_result.hresult != S_OK
            || child_result.source_width == 0
            || child_result.source_height == 0
            || child_result.consumer_frames == 0
            || child_result.distinct_source_timestamps == 0
            || child_result.distinct_source_timestamps
                > child_result.consumer_frames
            || child_result.first_sequence == 0
            || child_result.last_sequence < child_result.first_sequence
            || child_result.packet_submissions != child_result.consumer_frames
            || child_result.packets != child_result.packet_submissions
            || child_result.packet_bytes == 0
            || child_result.encoder_copied_submissions
                    + child_result.encoder_direct_submissions
                != child_result.packet_submissions
            || child_result.encoder_external_submissions
                > child_result.encoder_direct_submissions
            || child_result.encoder_external_identity_verified_submissions
                > child_result.encoder_external_submissions
            || child_result.encoder_external_video_encoder_bound_submissions
                > child_result.encoder_external_submissions
            || child_result.source_age_samples != child_result.consumer_frames
            || child_result.capture_age_samples != child_result.consumer_frames
            || child_result.gpu_completion_samples > child_result.consumer_frames
            || child_result.gpu_query_dropped
                != child_result.consumer_frames
                    - child_result.gpu_completion_samples
            || child_result.packet_latency_samples != child_result.packets
            || child_result.scheduled_begin_qpc != measure.measure_begin_qpc
            || child_result.scheduled_end_qpc != measure.measure_end_qpc
            || parent_actual_begin_qpc < measure.measure_begin_qpc
            || parent_actual_begin_qpc - measure.measure_begin_qpc
                > maximum_boundary_skew_ticks
            || parent_actual_begin_qpc >= measure.measure_end_qpc
            || parent_actual_end_qpc < measure.measure_end_qpc
            || parent_actual_end_qpc - measure.measure_end_qpc
                > maximum_boundary_skew_ticks
            || child_result.actual_begin_qpc < measure.measure_begin_qpc
            || child_result.actual_begin_qpc - measure.measure_begin_qpc
                > maximum_boundary_skew_ticks
            || child_result.actual_begin_qpc >= measure.measure_end_qpc
            || child_result.actual_end_qpc < measure.measure_end_qpc
            || child_result.actual_end_qpc - measure.measure_end_qpc
                > maximum_boundary_skew_ticks
            || child_result.result_send_qpc < child_result.actual_end_qpc
            || result_header_received_qpc < child_result.result_send_qpc) {
            fail("child consumer run failed: " + std::string(child_result.message)
                + " (status=" + std::to_string(child_result.gpu_status)
                + ", hr="
                + std::to_string(static_cast<long long>(child_result.hresult))
                + ')');
        }
        const std::uint64_t child_sequence_span =
            child_result.last_sequence - child_result.first_sequence + 1u;
        if (child_result.consumer_frames > child_sequence_span
            || child_result.bus_gaps
                != child_sequence_span - child_result.consumer_frames) {
            fail("child result sequence gaps do not match its frame coverage");
        }
        result.result_header_delivery_ms = qpc_milliseconds(
            result_header_received_qpc,
            child_result.result_send_qpc,
            frequency);
        const Clock::time_point chunks_begin = Clock::now();
        receive_child_samples(
            *child,
            ChildSampleKind::source_age,
            child_result.source_age_samples,
            run_nonce,
            topology_wire,
            options.duration_ms,
            result_deadline,
            result.source_age_ms);
        receive_child_samples(
            *child,
            ChildSampleKind::capture_age,
            child_result.capture_age_samples,
            run_nonce,
            topology_wire,
            options.duration_ms,
            result_deadline,
            result.capture_age_ms);
        receive_child_samples(
            *child,
            ChildSampleKind::gpu_completion,
            child_result.gpu_completion_samples,
            run_nonce,
            topology_wire,
            options.duration_ms,
            result_deadline,
            result.gpu_completion_ms);
        receive_child_samples(
            *child,
            ChildSampleKind::packet_latency,
            child_result.packet_latency_samples,
            run_nonce,
            topology_wire,
            options.duration_ms,
            result_deadline,
            result.packet_latency_ms);
        result.result_chunks_transfer_ms =
            milliseconds(Clock::now() - chunks_begin);

        result.elapsed_seconds = qpc_milliseconds(
            measure.measure_end_qpc,
            measure.measure_begin_qpc,
            frequency) / 1'000.0;
        result.parent_start_skew_ms = qpc_skew_milliseconds(
            parent_actual_begin_qpc, measure.measure_begin_qpc, frequency);
        result.parent_end_skew_ms = qpc_skew_milliseconds(
            parent_actual_end_qpc, measure.measure_end_qpc, frequency);
        result.child_start_skew_ms = qpc_skew_milliseconds(
            child_result.actual_begin_qpc, measure.measure_begin_qpc, frequency);
        result.child_end_skew_ms = qpc_skew_milliseconds(
            child_result.actual_end_qpc, measure.measure_end_qpc, frequency);
        result.source_presentations = delta(source_after, source_before);
        result.source_width = child_result.source_width;
        result.source_height = child_result.source_height;
        result.wgc_received = delta(
            capture_after.received_frames, capture_before.received_frames);
        result.wgc_published = delta(
            capture_after.published_frames, capture_before.published_frames);
        result.wgc_private_overwritten = delta(
            capture_after.overwritten_frames, capture_before.overwritten_frames);
        result.wgc_skipped_no_buffer = delta(
            capture_after.skipped_no_buffer, capture_before.skipped_no_buffer);
        result.producer_ingress_copies = delta(
            capture_after.ingress_copy_submissions,
            capture_before.ingress_copy_submissions);
        result.producer_ingress_transforms = delta(
            capture_after.ingress_transform_submissions,
            capture_before.ingress_transform_submissions);
        result.bus_published = delta(
            bus_after.published_frames, bus_before.published_frames);
        result.bus_no_slot = delta(bus_after.no_slot, bus_before.no_slot);
        result.derived_control_contention = topology == Topology::direct
            ? delta(result.wgc_skipped_no_buffer, result.bus_no_slot)
            : 0;
        result.consumer_frames = child_result.consumer_frames;
        result.distinct_source_timestamps =
            child_result.distinct_source_timestamps;
        result.bus_gaps = child_result.bus_gaps;
        result.first_bus_sequence = child_result.first_sequence;
        result.last_bus_sequence = child_result.last_sequence;
        result.packet_submissions = child_result.packet_submissions;
        result.packets = child_result.packets;
        result.packet_bytes = child_result.packet_bytes;
        result.encoder_copied_submissions =
            child_result.encoder_copied_submissions;
        result.encoder_direct_submissions =
            child_result.encoder_direct_submissions;
        result.encoder_external_submissions =
            child_result.encoder_external_submissions;
        result.encoder_external_identity_verified_submissions =
            child_result.encoder_external_identity_verified_submissions;
        result.encoder_external_video_encoder_bound_submissions =
            child_result.encoder_external_video_encoder_bound_submissions;
        result.gpu_query_dropped = child_result.gpu_query_dropped;

        ChildCommand close;
        close.command = kChildCloseCommand;
        close.topology = topology_wire;
        close.run_nonce = run_nonce;
        const Clock::time_point close_begin = Clock::now();
        child->send(close, "child close command");
        const ChildClosed closed = child->receive<ChildClosed>(
            options.ipc_timeout_ms, "child closed");
        result.close_closed_rtt_ms = milliseconds(Clock::now() - close_begin);
        if (closed.structure_size != sizeof(ChildClosed)
            || closed.wire_version != kChildWireVersion
            || closed.success == 0
            || closed.consumer_index != run.registration.consumer_index
            || closed.topology != topology_wire
            || closed.run_nonce != run_nonce
            || closed.reserved != 0
            || closed.gpu_status != static_cast<std::uint32_t>(gpu::GpuStatus::ok)
            || closed.hresult != S_OK) {
            fail("child consumer close failed: " + std::string(closed.message));
        }

        require_gpu(
            run.publisher.unregister_consumer(
                run.registration, options.ipc_timeout_ms),
            "unregister child-process consumer");
        registered = false;
        const gpu::SharedFrameBusStats closed_stats = run.publisher.stats();
        if (closed_stats.active_consumers != 0
            || closed_stats.quarantined_slots != 0
            || closed_stats.dead_consumer_reclamations != 0) {
            fail("normal child cleanup left active, quarantined, or dead-reclaimed bus state");
        }

        ChildCommand exit;
        exit.command = kChildExitCommand;
        exit.topology = topology_wire;
        exit.run_nonce = run_nonce;
        child->send(exit, "child exit command");
        child->wait_for_exit(options.ipc_timeout_ms);
        child_exited = true;
        validate_completed_run(result);
        return result;
    } catch (const std::exception& error) {
        std::string message = error.what();
        const std::string cleanup_failure = emergency_cleanup();
        if (!cleanup_failure.empty()) {
            message += "; cleanup: " + cleanup_failure;
        }
        fail(std::move(message));
    }
}

RunResult run_topology(
    Topology topology,
    std::uint32_t pair,
    std::string order,
    const Options& options,
    WindowThread& source,
    DeviceContext& d3d,
    const MonitorTarget* monitor_target) {
    if (options.consumer_mode == ConsumerMode::child) {
        return run_topology_child(
            topology, pair, std::move(order), options, source, d3d,
            monitor_target);
    }
    return run_topology_inproc(
        topology, pair, std::move(order), options, source, d3d,
        monitor_target);
}

void print_latency(
    std::string_view name,
    const std::vector<double>& samples) {
    const Summary summary = summarize(samples);
    std::cout << "  " << std::left << std::setw(25) << name
              << std::right << std::setw(10) << summary.p50_ms
              << std::setw(10) << summary.p95_ms
              << std::setw(10) << summary.p99_ms
              << "  n=" << samples.size() << '\n';
}

void print_run(const RunResult& result) {
    const double elapsed = result.elapsed_seconds;
    std::cout << (result.bus_format == BusFormat::bgra ? "\nPair " : "\nRun ")
              << result.pair << ' ' << result.order
              << " / " << topology_name(result.topology)
              << " (" << std::fixed << std::setprecision(3)
              << elapsed << " s measured)\n"
              << "  consumer mode/pid:              "
              << consumer_mode_name(result.consumer_mode) << '/'
              << result.consumer_process_id << '\n'
              << "  bus format:                     "
              << bus_format_name(result.bus_format) << '\n'
              << "  source swapchain presentations/s: "
              << rate(result.source_presentations, elapsed) << '\n'
              << "  capture received/s:             "
              << rate(result.wgc_received, elapsed) << '\n'
              << "  capture published/s:            "
              << rate(result.wgc_published, elapsed) << '\n'
              << "  bus published/s:                "
              << rate(result.bus_published, elapsed) << '\n'
              << "  consumer frames/s:              "
              << rate(result.consumer_frames, elapsed) << '\n'
              << "  distinct presentation ids/s:    "
              << rate(result.distinct_source_timestamps, elapsed) << '\n'
              << "  bus gaps:                       " << result.bus_gaps << '\n'
              << "  first/last bus sequence:        "
              << result.first_bus_sequence << '/'
              << result.last_bus_sequence << '\n'
              << "  publisher no_slot:              " << result.bus_no_slot << '\n'
              << "  capture skipped_no_buffer:      "
              << result.wgc_skipped_no_buffer << '\n'
              << "  producer ingress copy/transform: "
              << result.producer_ingress_copies << '/'
              << result.producer_ingress_transforms << '\n'
              << "  derived control contention:     "
              << result.derived_control_contention
              << (result.topology == Topology::direct
                    ? " (skipped_no_buffer - bus no_slot)\n"
                    : " (not applicable to staged producer)\n")
              << "  private-ring overwrites:        "
              << result.wgc_private_overwritten << '\n'
              << "  encoded packets/submissions:    "
              << result.packets << '/' << result.packet_submissions
              << ", bytes=" << result.packet_bytes << '\n'
              << "  encoder copied/direct/external: "
              << result.encoder_copied_submissions << '/'
              << result.encoder_direct_submissions << '/'
              << result.encoder_external_submissions << '\n'
              << "  external identity/encoder-bind: "
              << result.encoder_external_identity_verified_submissions << '/'
              << result.encoder_external_video_encoder_bound_submissions
              << '\n'
              << (planar_bus_format(result.bus_format)
                    ? "  tracked-input samples/dropped:  "
                    : "  GPU query samples/dropped:      ")
              << result.gpu_completion_ms.size() << '/'
              << result.gpu_query_dropped << '\n'
              << "  latency percentiles             p50 ms    p95 ms    p99 ms\n";
    if (result.capture_backend == CaptureBackend::wgc) {
        print_latency("source -> consumer age", result.source_age_ms);
    }
    print_latency(
        result.consumer_mode == ConsumerMode::child
            ? "capture/pub -> child age"
            : "capture -> consumer age",
        result.capture_age_ms);
    print_latency(
        planar_bus_format(result.bus_format)
            ? "external tracked lifetime"
            : "transform -> GPU complete",
        result.gpu_completion_ms);
    print_latency("submit -> packet", result.packet_latency_ms);
    if (result.consumer_mode == ConsumerMode::child) {
        std::cout
            << "  cross-process control/data transfer (not GPU latency)\n"
            << "    setup -> ready:               "
            << result.setup_ready_ms << " ms (includes child GPU/encoder init)\n"
            << "    Measure -> Armed RTT:         "
            << result.measure_armed_rtt_ms << " ms (control pipe)\n"
            << "    ResultHeader delivery/pickup: "
            << result.result_header_delivery_ms << " ms (shared system QPC)\n"
            << "    parent result-chunk drain:     "
            << result.result_chunks_transfer_ms << " ms\n"
            << "    Close -> Closed RTT:          "
            << result.close_closed_rtt_ms << " ms (control pipe)\n"
            << "  absolute-QPC boundary skew (actual - scheduled)\n"
            << "    parent start/end:             "
            << result.parent_start_skew_ms << " / "
            << result.parent_end_skew_ms << " ms\n"
            << "    child start/end:              "
            << result.child_start_skew_ms << " / "
            << result.child_end_skew_ms << " ms\n"
            << "  capture/pub -> child age is the cross-process data-path age; "
               "v3 metadata has no pure fence-ready timestamp.\n";
    }
}

struct Aggregate final {
    Topology topology = Topology::staged;
    ConsumerMode consumer_mode = ConsumerMode::inproc;
    BusFormat bus_format = BusFormat::bgra;
    CaptureBackend capture_backend = CaptureBackend::wgc;
    gpu::VideoCodec codec = gpu::VideoCodec::h264;
    gpu::GpuTransformBackend planar_backend =
        gpu::GpuTransformBackend::automatic;
    double elapsed_seconds = 0.0;
    std::uint64_t wgc_received = 0;
    std::uint64_t wgc_published = 0;
    std::uint64_t bus_published = 0;
    std::uint64_t distinct_source_timestamps = 0;
    std::uint64_t bus_gaps = 0;
    std::uint64_t no_slot = 0;
    std::uint64_t control_contention = 0;
    std::uint64_t producer_ingress_copies = 0;
    std::uint64_t producer_ingress_transforms = 0;
    std::uint64_t packet_submissions = 0;
    std::uint64_t packets = 0;
    std::uint64_t encoder_copied_submissions = 0;
    std::uint64_t encoder_direct_submissions = 0;
    std::uint64_t encoder_external_submissions = 0;
    std::uint64_t encoder_external_identity_verified_submissions = 0;
    std::uint64_t encoder_external_video_encoder_bound_submissions = 0;
    std::uint64_t gpu_query_dropped = 0;
    std::vector<double> source_age_ms;
    std::vector<double> capture_age_ms;
    std::vector<double> gpu_completion_ms;
    std::vector<double> packet_latency_ms;
};

Aggregate aggregate(Topology topology, const std::vector<RunResult>& runs) {
    Aggregate output;
    output.topology = topology;
    bool found_mode = false;
    for (const RunResult& run : runs) {
        if (run.topology != topology) continue;
        if (!found_mode) {
            output.consumer_mode = run.consumer_mode;
            output.bus_format = run.bus_format;
            output.capture_backend = run.capture_backend;
            output.codec = run.codec;
            output.planar_backend = run.planar_backend;
            found_mode = true;
        } else if (output.consumer_mode != run.consumer_mode) {
            fail("aggregate contains mixed consumer modes");
        } else if (output.bus_format != run.bus_format) {
            fail("aggregate contains mixed bus formats");
        } else if (output.capture_backend != run.capture_backend) {
            fail("aggregate contains mixed capture backends");
        } else if (output.codec != run.codec) {
            fail("aggregate contains mixed codecs");
        } else if (output.planar_backend != run.planar_backend) {
            fail("aggregate contains mixed planar backends");
        }
        output.elapsed_seconds += run.elapsed_seconds;
        output.wgc_received += run.wgc_received;
        output.wgc_published += run.wgc_published;
        output.bus_published += run.bus_published;
        output.distinct_source_timestamps += run.distinct_source_timestamps;
        output.bus_gaps += run.bus_gaps;
        output.no_slot += run.bus_no_slot;
        output.control_contention += run.derived_control_contention;
        output.producer_ingress_copies += run.producer_ingress_copies;
        output.producer_ingress_transforms += run.producer_ingress_transforms;
        output.packet_submissions += run.packet_submissions;
        output.packets += run.packets;
        output.encoder_copied_submissions += run.encoder_copied_submissions;
        output.encoder_direct_submissions += run.encoder_direct_submissions;
        output.encoder_external_submissions += run.encoder_external_submissions;
        output.encoder_external_identity_verified_submissions +=
            run.encoder_external_identity_verified_submissions;
        output.encoder_external_video_encoder_bound_submissions +=
            run.encoder_external_video_encoder_bound_submissions;
        output.gpu_query_dropped += run.gpu_query_dropped;
        output.source_age_ms.insert(
            output.source_age_ms.end(), run.source_age_ms.begin(), run.source_age_ms.end());
        output.capture_age_ms.insert(
            output.capture_age_ms.end(), run.capture_age_ms.begin(), run.capture_age_ms.end());
        output.gpu_completion_ms.insert(
            output.gpu_completion_ms.end(),
            run.gpu_completion_ms.begin(),
            run.gpu_completion_ms.end());
        output.packet_latency_ms.insert(
            output.packet_latency_ms.end(), run.packet_latency_ms.begin(), run.packet_latency_ms.end());
    }
    return output;
}

void print_aggregate(const Aggregate& result) {
    std::cout << "\nAggregate " << topology_name(result.topology)
              << '/' << bus_format_name(result.bus_format)
              << " (" << std::fixed << std::setprecision(3)
              << result.elapsed_seconds << " measured seconds)\n"
              << "  capture received/published/s:   "
              << rate(result.wgc_received, result.elapsed_seconds) << " / "
              << rate(result.wgc_published, result.elapsed_seconds) << '\n'
              << "  bus published/s:                "
              << rate(result.bus_published, result.elapsed_seconds) << '\n'
              << "  distinct presentation ids/s:    "
              << rate(result.distinct_source_timestamps, result.elapsed_seconds) << '\n'
              << "  gaps/no_slot/control-contention: "
              << result.bus_gaps << '/' << result.no_slot << '/'
              << result.control_contention << '\n'
              << "  producer ingress copy/transform: "
              << result.producer_ingress_copies << '/'
              << result.producer_ingress_transforms << '\n'
              << "  encoded packets/submissions:    "
              << result.packets << '/' << result.packet_submissions << '\n'
              << "  encoder copied/direct/external: "
              << result.encoder_copied_submissions << '/'
              << result.encoder_direct_submissions << '/'
              << result.encoder_external_submissions << '\n'
              << "  external identity/encoder-bind: "
              << result.encoder_external_identity_verified_submissions << '/'
              << result.encoder_external_video_encoder_bound_submissions
              << '\n'
              << (planar_bus_format(result.bus_format)
                    ? "  tracked-input samples/dropped:  "
                    : "  GPU query samples/dropped:      ")
              << result.gpu_completion_ms.size() << '/'
              << result.gpu_query_dropped << '\n'
              << "  latency percentiles             p50 ms    p95 ms    p99 ms\n";
    if (result.capture_backend == CaptureBackend::wgc) {
        print_latency("source -> consumer age", result.source_age_ms);
    }
    print_latency(
        result.consumer_mode == ConsumerMode::child
            ? "capture/pub -> child age"
            : "capture -> consumer age",
        result.capture_age_ms);
    print_latency(
        planar_bus_format(result.bus_format)
            ? "external tracked lifetime"
            : "transform -> GPU complete",
        result.gpu_completion_ms);
    print_latency("submit -> packet", result.packet_latency_ms);
}

std::string json_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() + 16);
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                output += "\\u00";
                output += hex[character >> 4];
                output += hex[character & 0x0f];
            } else {
                output += static_cast<char>(character);
            }
            break;
        }
    }
    return output;
}

std::string guid_string(const GUID& value) {
    std::ostringstream output;
    output << '{' << std::hex << std::setfill('0')
           << std::setw(8) << value.Data1 << '-'
           << std::setw(4) << value.Data2 << '-'
           << std::setw(4) << value.Data3 << '-'
           << std::setw(2) << static_cast<unsigned>(value.Data4[0])
           << std::setw(2) << static_cast<unsigned>(value.Data4[1]) << '-';
    for (std::size_t index = 2; index < std::size(value.Data4); ++index) {
        output << std::setw(2) << static_cast<unsigned>(value.Data4[index]);
    }
    output << '}';
    return output.str();
}

void write_runtime_evidence_file(
    const std::wstring& path,
    std::string_view contents) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail("opening runtime evidence output failed: "
            + std::to_string(GetLastError()));
    }
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            contents.size() - offset,
            std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(
                file, contents.data() + offset, requested, &written, nullptr)
            || written == 0) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            fail("writing runtime evidence output failed: "
                + std::to_string(error));
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        fail("flushing runtime evidence output failed: "
            + std::to_string(error));
    }
    if (!CloseHandle(file)) {
        fail("closing runtime evidence output failed: "
            + std::to_string(GetLastError()));
    }
}

bool emit_runtime_evidence(
    const Options& options,
    const RunResult& result,
    ID3D11Device* device,
    std::string_view executable_sha256) {
    if (!options.runtime_evidence_json.has_value()) return true;
    const bool exact_workload =
        options.consumer_mode == ConsumerMode::inproc
        && planar_bus_format(options.bus_format)
        && options.pairs == 1
        && result.consumer_mode == ConsumerMode::inproc
        && result.topology == Topology::direct
        && result.bus_format == options.bus_format
        && result.capture_backend == options.capture_backend
        && result.codec == options.codec
        && result.planar_backend == options.planar_backend
        && options.planar_backend_explicit
        && result.planar_backend != gpu::GpuTransformBackend::automatic
        && result.source_presentations > 0
        && result.source_region_width == options.source_size
        && result.source_region_height == options.source_size
        && (result.capture_backend != CaptureBackend::desktop_duplication
            || (result.output_device_name.size() != 0
                && result.logical_monitor_width == result.source_width
                && result.logical_monitor_height == result.source_height
                && result.capture_surface_width != 0
                && result.capture_surface_height != 0
                && result.desktop_present_frames > 0
                && (std::string_view(rotation_name(result.output_rotation))
                        == "identity"
                    || result.planar_backend
                        == gpu::GpuTransformBackend::video_processor)));
    const bool qualifies_l2 = exact_workload
        && result.lifetime_encoder_external_submissions > 0
        && result.lifetime_encoder_direct_submissions
            == result.lifetime_encoder_external_submissions
        && result.lifetime_encoder_external_identity_verified_submissions
            == result.lifetime_encoder_external_submissions
        && result.lifetime_encoder_external_video_encoder_bound_submissions
            == result.lifetime_encoder_external_submissions
        && result.lifetime_encoder_copied_submissions == 0
        && result.lifetime_producer_ingress_copies == 0
        && result.lifetime_producer_ingress_transforms > 0
        && result.lifetime_bus_published > 0
        && result.lifetime_bus_copied_publishes == 0
        && result.lifetime_bus_direct_publishes
            == result.lifetime_bus_published
        && (result.capture_backend != CaptureBackend::desktop_duplication
            || (result.lifetime_desktop_present_frames > 0
                && result.lifetime_desktop_present_frames
                    <= result.lifetime_bus_published))
        && result.packet_codec_mismatches == 0
        && !result.mft_internal_copy_observable
        && !IsEqualGUID(result.encoder_mft_identity.clsid, GUID_NULL)
        && result.encoder_mft_identity.friendly_name[0] != '\0';
    const std::string adapter_luid = adapter_luid_string(device);
    const std::string encoder_mft_clsid = guid_string(
        result.encoder_mft_identity.clsid);
    const std::string encoder_mft_name(
        result.encoder_mft_identity.friendly_name.data());
    const std::string encoder_mft = encoder_mft_name + " " + encoder_mft_clsid;
    const std::string capture_target = result.capture_backend
            == CaptureBackend::desktop_duplication
        ? "monitor" : "window";
    const std::string rotation = result.capture_backend
            == CaptureBackend::desktop_duplication
        ? rotation_name(result.output_rotation) : "not_applicable";
    std::ostringstream output_identity_builder;
    if (result.capture_backend == CaptureBackend::desktop_duplication) {
        output_identity_builder
            << result.output_index << ':' << result.output_device_name << ':'
            << result.output_desktop.left << ',' << result.output_desktop.top
            << ',' << result.output_desktop.right << ','
            << result.output_desktop.bottom << ':'
            << result.logical_monitor_width << 'x'
            << result.logical_monitor_height;
    } else {
        output_identity_builder << "not_applicable";
    }
    const std::string output_identity = output_identity_builder.str();
    const std::uint32_t capture_surface_width = result.capture_backend
            == CaptureBackend::desktop_duplication
        ? result.capture_surface_width : result.source_width;
    const std::uint32_t capture_surface_height = result.capture_backend
            == CaptureBackend::desktop_duplication
        ? result.capture_surface_height : result.source_height;
    std::ostringstream tuple;
    tuple << "v2|" << adapter_luid
          << '|' << capture_backend_name(result.capture_backend)
          << '|' << capture_target
          << '|' << rotation
          << '|' << output_identity
          << '|' << planar_backend_name(result.planar_backend)
          << '|' << evidence_format_name(result.bus_format)
          << "|YCBCR_STUDIO_G22_LEFT_P709|"
          << capture_surface_width << 'x' << capture_surface_height
          << '|' << result.source_region_x << ',' << result.source_region_y
          << ',' << result.source_region_width << ','
          << result.source_region_height
          << '|' << options.size << 'x' << options.size
          << '|' << codec_name(result.codec)
          << '|' << encoder_input_subtype_name(result.bus_format)
          << '|' << encoder_output_subtype_name(result.codec)
          << '|' << encoder_mft_clsid;
    const std::string tuple_sha256 = sha256_text(tuple.str());

    std::ostringstream json;
    json << "{\n"
         << "  \"schemaVersion\": 2,\n"
         << "  \"evidenceLevel\": " << (qualifies_l2 ? 2 : 0) << ",\n"
         << "  \"evidenceName\": \""
         << (qualifies_l2
                ? "l2_external_texture_identity_and_encoder_bind"
                : "none")
         << "\",\n"
         << "  \"processId\": " << GetCurrentProcessId() << ",\n"
         << "  \"executableSha256\": \""
         << json_escape(executable_sha256) << "\",\n"
         << "  \"qualificationRunNonce\": \""
         << options.qualification_run_nonce << "\",\n"
         << "  \"adapterLuid\": \"" << json_escape(adapter_luid)
         << "\",\n"
         << "  \"countersScope\": \"capture_encoder_instance_lifetime_including_warmup\",\n"
         << "  \"tupleSha256\": \"" << tuple_sha256 << "\",\n"
         << "  \"qualificationTuple\": {\n"
         << "    \"adapterLuid\": \"" << json_escape(adapter_luid) << "\",\n"
         << "    \"captureBackend\": \""
         << capture_backend_name(result.capture_backend) << "\",\n"
         << "    \"captureTarget\": \"" << capture_target << "\",\n"
         << "    \"rotation\": \"" << rotation << "\",\n"
         << "    \"outputIdentity\": \""
         << json_escape(output_identity) << "\",\n"
         << "    \"transformBackend\": \""
         << planar_backend_name(result.planar_backend) << "\",\n"
         << "    \"format\": \""
         << evidence_format_name(result.bus_format) << "\",\n"
         << "    \"colorSpace\": \"YCBCR_STUDIO_G22_LEFT_P709\",\n"
         << "    \"hdr10StaticMetadata\": false,\n"
         << "    \"captureSurfaceWidth\": "
         << capture_surface_width << ",\n"
         << "    \"captureSurfaceHeight\": "
         << capture_surface_height << ",\n"
         << "    \"logicalSourceWidth\": " << result.source_width << ",\n"
         << "    \"logicalSourceHeight\": " << result.source_height << ",\n"
         << "    \"sourceX\": " << result.source_region_x << ",\n"
         << "    \"sourceY\": " << result.source_region_y << ",\n"
         << "    \"sourceWidth\": " << result.source_region_width << ",\n"
         << "    \"sourceHeight\": " << result.source_region_height << ",\n"
         << "    \"outputWidth\": " << options.size << ",\n"
         << "    \"outputHeight\": " << options.size << ",\n"
         << "    \"codec\": \"" << codec_name(result.codec) << "\",\n"
         << "    \"encoderInputSubtype\": \""
         << encoder_input_subtype_name(result.bus_format) << "\",\n"
         << "    \"encoderOutputSubtype\": \""
         << encoder_output_subtype_name(result.codec) << "\",\n"
         << "    \"encoderMft\": \""
         << json_escape(encoder_mft) << "\",\n"
         << "    \"encoderMftClsid\": \""
         << json_escape(encoder_mft_clsid) << "\",\n"
         << "    \"encoderMftFriendlyName\": \""
         << json_escape(encoder_mft_name) << "\"\n"
         << "  },\n"
         << "  \"encoderMatchingTransformCount\": "
         << result.encoder_matching_transform_count << ",\n"
         << "  \"externalSubmissions\": "
         << result.lifetime_encoder_external_submissions << ",\n"
         << "  \"externalIdentityVerifiedSubmissions\": "
         << result.lifetime_encoder_external_identity_verified_submissions
         << ",\n"
         << "  \"externalVideoEncoderBoundSubmissions\": "
         << result.lifetime_encoder_external_video_encoder_bound_submissions
         << ",\n"
         << "  \"encoderCopiedSubmissions\": "
         << result.lifetime_encoder_copied_submissions << ",\n"
         << "  \"encoderDirectSubmissions\": "
         << result.lifetime_encoder_direct_submissions << ",\n"
         << "  \"producerIngressCopySubmissions\": "
         << result.lifetime_producer_ingress_copies << ",\n"
         << "  \"producerIngressTransformSubmissions\": "
         << result.lifetime_producer_ingress_transforms << ",\n"
         << "  \"desktopPresentFrames\": "
         << result.lifetime_desktop_present_frames << ",\n"
         << "  \"busPublishedFrames\": "
         << result.lifetime_bus_published << ",\n"
         << "  \"busCopiedPublishes\": "
         << result.lifetime_bus_copied_publishes << ",\n"
         << "  \"busDirectPublishes\": "
         << result.lifetime_bus_direct_publishes << ",\n"
         << "  \"packetCodecMismatches\": "
         << result.packet_codec_mismatches << ",\n"
         << "  \"fluxcapExplicitCopyFree\": "
         << (result.lifetime_producer_ingress_copies == 0
                && result.lifetime_bus_copied_publishes == 0
                && result.lifetime_encoder_copied_submissions == 0
                ? "true" : "false")
         << ",\n"
         << "  \"measuredConsumerFrames\": " << result.consumer_frames << ",\n"
         << "  \"measuredSourcePresentations\": "
         << result.source_presentations << ",\n"
         << "  \"measuredDesktopPresentFrames\": "
         << result.desktop_present_frames << ",\n"
         << "  \"measuredDurationMs\": " << options.duration_ms << ",\n"
         << "  \"warmupMs\": " << options.warmup_ms << ",\n"
         << "  \"mftInternalCopyObservable\": "
         << (result.mft_internal_copy_observable ? "true" : "false")
         << ",\n"
         << "  \"driverPrivateSurfaceCopyObservable\": false,\n"
         << "  \"hardwareDmaCopyObservable\": false\n"
         << "}\n";
    write_runtime_evidence_file(*options.runtime_evidence_json, json.str());
    return qualifies_l2;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--consumer-child") {
            if (argc != 4) return 64;
            HANDLE input = parse_inherited_handle(argv[2]);
            HANDLE output = parse_inherited_handle(argv[3]);
            if (input == nullptr || output == nullptr) return 65;
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            return run_consumer_child(input, output);
        }
        Options options = parse_options(argc, argv);
        if (options.help) {
            std::cout
                << "Usage: fluxcap_gpu_wgc_bus_bench [--size 320|640] "
                   "[--source-size 320|640] "
                   "[--duration-ms N] [--warmup-ms N] [--pairs N] "
                   "[--consumer-mode inproc|child] [--bus-format bgra|nv12|p010] "
                   "[--codec h264|hevc|av1] [--capture-backend wgc|desktop-duplication] "
                    "[--planar-backend automatic|deterministic-planar|video-processor] "
                    "[--ipc-timeout-ms N] [--adapter-index N] [--output-index N] "
                    "[--runtime-evidence-json PATH] [--list-adapters]\n"
                   "BGRA uses counterbalanced staged/direct AB/BA blocks. "
                   "NV12/P010 run the direct planar fast path only. Runtime "
                   "evidence requires one inproc planar run, an explicit "
                   "deterministic-planar or video-processor backend, and "
                   "VIDEO_ENCODER bind.\n";
            return 0;
        }
        // DXGI_OUTPUT_DESC desktop coordinates are DPI-virtualized unless the
        // process is per-monitor aware. Apply the same coordinate space to
        // --list-adapters and to the actual qualification workload so an
        // output selected from the listing has an identical recorded identity.
        if (!SetProcessDpiAwarenessContext(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
            && !AreDpiAwarenessContextsEqual(
                GetThreadDpiAwarenessContext(),
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            fail("per-monitor-v2 DPI awareness is required for stable output identity");
        }
        if (options.list_adapters) {
            print_adapters();
            return 0;
        }
        std::optional<std::string> runtime_executable_sha256;
        if (options.runtime_evidence_json.has_value()) {
            if (options.qualification_run_nonce.empty()) {
                options.qualification_run_nonce = make_qualification_nonce();
            }
            // Remove any prior claim before starting this traced workload. If
            // the process later fails, the empty file cannot be mistaken for
            // evidence from this PID.
            truncate_runtime_evidence_file(*options.runtime_evidence_json);
            if (options.consumer_mode != ConsumerMode::inproc
                || !planar_bus_format(options.bus_format)
                 || options.pairs != 1
                 || options.child_fault != ChildFault::none
                 || !options.planar_backend_explicit
                 || options.planar_backend
                     == gpu::GpuTransformBackend::automatic) {
                fail("--runtime-evidence-json requires exactly "
                    "one inproc NV12/P010 run, pairs=1, and an explicit planar backend");
            }
            runtime_executable_sha256 = sha256_file(running_executable_path());
        }
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        ComPtr<IDXGIAdapter1> requested_adapter;
        if (options.adapter_index.has_value()) {
            requested_adapter = adapter_at_index(*options.adapter_index);
        }
        DeviceContext d3d = create_device(requested_adapter.Get());
        std::optional<MonitorTarget> monitor_target;
        if (options.capture_backend == CaptureBackend::desktop_duplication) {
            monitor_target = select_monitor_target(
                d3d.device.Get(), *options.output_index);
        }
        ComPtr<IDXGIAdapter1> source_adapter = device_adapter(d3d.device.Get());
        WindowThread source(
            source_adapter.Get(),
            monitor_target.has_value() ? &*monitor_target : nullptr);

        const char* capture_label = options.capture_backend
                == CaptureBackend::desktop_duplication
            ? "Desktop Duplication" : "WGC";
        std::cout << "FluxCap real " << capture_label
                  << (options.bus_format == BusFormat::bgra
                        ? " SharedFrameBus AB/BA benchmark\n"
                        : " planar SharedFrameBus fast-path benchmark\n")
                  << "scope: "
                  << (options.consumer_mode == ConsumerMode::child
                        ? "CROSS-PROCESS" : "SAME-PROCESS")
                  << " consumer; real " << capture_label
                  << " presentations; no synthetic source\n"
                  << (planar_bus_format(options.bus_format)
                        ? "consumer path: planar bus texture -> tracked external hardware encoder input\n"
                        : "consumer path: BGRA bus texture -> GPU NV12 transform -> release -> hardware encoder\n")
                  << "consumer-side bus CopyResource: none\n"
                  << "consumer mode: "
                  << consumer_mode_name(options.consumer_mode) << '\n'
                  << "bus format: " << bus_format_name(options.bus_format) << '\n'
                  << "codec: " << codec_name(options.codec) << '\n'
                  << "capture backend: "
                  << capture_backend_name(options.capture_backend) << '\n'
                  << "planar backend: "
                  << planar_backend_name(options.planar_backend) << '\n'
                  << "DXGI adapter index: "
                  << (options.adapter_index.has_value()
                        ? std::to_string(*options.adapter_index)
                        : std::string("default")) << '\n'
                  << "adapter: " << adapter_name(d3d.device.Get()) << '\n'
                  << "source ROI -> output: "
                  << options.source_size << 'x' << options.source_size
                  << " -> " << options.size << 'x' << options.size
                  << ", measured window: " << options.duration_ms << " ms"
                   << ", warmup: " << options.warmup_ms << " ms"
                   << (options.bus_format == BusFormat::bgra
                        ? ", AB/BA blocks: " : ", fast-path runs: ")
                   << options.pairs
                   << ", IPC timeout: " << options.ipc_timeout_ms << " ms\n"
                  << (planar_bus_format(options.bus_format)
                        ? "Input completion: each IMFTrackedSample lifetime token owns its bus lease; callbacks may retire multiple in-flight slots.\n"
                        : "GPU completion: asynchronous D3D11 event-query ring placed after the transform read and submitted by bus release.\n");
        if (options.consumer_mode == ConsumerMode::child) {
            std::cout
                << "child process/device/transform/encoder initialization is outside "
                   "the measured window; no per-frame pipe IPC is used.\n"
                << "parent and child share an absolute system-QPC begin/end barrier.\n";
        }

        std::vector<RunResult> runs;
        if (options.bus_format == BusFormat::bgra) {
            runs.reserve(static_cast<std::size_t>(options.pairs) * 4u);
            for (std::uint32_t pair = 1; pair <= options.pairs; ++pair) {
                constexpr std::array<Topology, 4> order{
                    Topology::staged,
                    Topology::direct,
                    Topology::direct,
                    Topology::staged};
                constexpr std::array<const char*, 4> labels{
                    "AB/A",
                    "AB/B",
                    "BA/B",
                    "BA/A"};
                for (std::size_t index = 0; index < order.size(); ++index) {
                    RunResult result = run_topology(
                        order[index],
                        pair,
                        labels[index],
                        options,
                        source,
                        d3d,
                        monitor_target.has_value() ? &*monitor_target : nullptr);
                    print_run(result);
                    runs.push_back(std::move(result));
                }
            }
        } else {
            runs.reserve(options.pairs);
            for (std::uint32_t run_index = 1;
                 run_index <= options.pairs;
                 ++run_index) {
                RunResult result = run_topology(
                    Topology::direct,
                    run_index,
                    std::string(evidence_format_name(options.bus_format))
                        + " fast path",
                    options,
                    source,
                    d3d,
                    monitor_target.has_value() ? &*monitor_target : nullptr);
                print_run(result);
                runs.push_back(std::move(result));
            }
        }

        const Aggregate direct = aggregate(Topology::direct, runs);
        if (options.bus_format == BusFormat::bgra) {
            const Aggregate staged = aggregate(Topology::staged, runs);
            print_aggregate(staged);
            print_aggregate(direct);
            const double published_ratio =
                rate(direct.bus_published, direct.elapsed_seconds)
                / std::max(0.001,
                    rate(staged.bus_published, staged.elapsed_seconds));
            std::cout << "\nDirect/staged aggregate bus-publish rate ratio: "
                      << std::fixed << std::setprecision(3)
                      << published_ratio << "x\n";
        } else {
            print_aggregate(direct);
            std::cout << "\nPlanar fast-path invariant: one producer planar transform; "
                         "zero consumer copies; external tracked encoder input.\n";
        }
        if (options.runtime_evidence_json.has_value()) {
            if (runs.size() != 1 || !runtime_executable_sha256.has_value()) {
                fail("runtime evidence workload was not exactly one completed run");
            }
            const bool emitted_l2 = emit_runtime_evidence(
                options,
                runs.front(),
                d3d.device.Get(),
                *runtime_executable_sha256);
            if (!emitted_l2) {
                fail("runtime evidence requirements were not met; wrote evidenceLevel 0");
            }
            std::cout << "Runtime L2 evidence: "
                      << "external identity and VIDEO_ENCODER bind verified for "
                      << runs.front().encoder_external_submissions
                      << " measured submissions.\n";
        }
        std::cout << "Interpretation: unique capture presentation rate remains distinct from "
                     "synthetic GPU operation throughput.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU bus benchmark failed: " << error.what() << '\n';
        std::cerr << "Usage: fluxcap_gpu_wgc_bus_bench [--size 320|640] "
                     "[--source-size 320|640] "
                     "[--duration-ms N] [--warmup-ms N] [--pairs N] "
                      "[--consumer-mode inproc|child] [--bus-format bgra|nv12|p010] "
                     "[--codec h264|hevc|av1] [--capture-backend wgc|desktop-duplication] "
                     "[--planar-backend automatic|deterministic-planar|video-processor] "
                     "[--ipc-timeout-ms N] [--adapter-index N] [--output-index N] "
                     "[--runtime-evidence-json PATH] [--list-adapters]\n";
        return 1;
    }
}
