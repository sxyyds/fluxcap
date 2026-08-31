#include <fluxcap/gpu.hpp>

#include "../test_support/win32_child_process.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
namespace gpu = fluxcap::gpu;
using fluxcap::test_support::ChildProcess;
using fluxcap::test_support::parse_inherited_handle;
using fluxcap::test_support::read_exact;
using fluxcap::test_support::write_exact;

constexpr std::uint32_t kWireVersion = 1;
constexpr std::uint32_t kSlotCount = gpu::shared_frame_bus_max_slots;
constexpr std::uint32_t kAcquireSliceMs = 100;
constexpr std::uint32_t kValidationPeriod = 256;
constexpr std::uint32_t kDefaultFrames = 10'000;
constexpr std::uint32_t kDefaultWarmup = 1'000;
constexpr std::uint32_t kDefaultTimeoutMs = 120'000;
constexpr std::uint32_t kMaximumFrames = 10'000'000;
constexpr std::uint32_t kMaximumTimeoutMs = 3'600'000;
constexpr std::uint32_t kRoiSourceWidth = 2560;
constexpr std::uint32_t kRoiSourceHeight = 1600;
constexpr std::uint32_t kDefaultCompareRounds = 4;
constexpr std::uint32_t kMaximumCompareRounds = 32;
constexpr std::uint32_t kStartCommand = 0x5354'4152u;
constexpr std::uint32_t kCloseCommand = 0x434c'4f53u;
constexpr std::uint32_t kExitCommand = 0x4558'4954u;

enum class BenchmarkMode {
    broadcast,
    roi_publish_compare,
};

enum class RoiPublishTopology {
    staged,
    direct,
};

struct Options final {
    std::uint32_t size = 320;
    std::uint32_t consumers = 2;
    std::uint32_t frames = kDefaultFrames;
    std::uint32_t warmup = kDefaultWarmup;
    std::uint32_t timeout_ms = kDefaultTimeoutMs;
    std::uint32_t rounds = kDefaultCompareRounds;
    BenchmarkMode mode = BenchmarkMode::broadcast;
    bool show_help = false;
};

struct ChildSetup final {
    std::uint32_t structure_size = sizeof(ChildSetup);
    std::uint32_t wire_version = kWireVersion;
    std::uint32_t size = 0;
    std::uint32_t timeout_ms = 0;
    std::uint64_t warmup_sequence = 0;
    std::uint64_t target_sequence = 0;
    gpu::SharedFrameBusRegistration registration{};
};

struct ChildCommand final {
    std::uint32_t structure_size = sizeof(ChildCommand);
    std::uint32_t command = 0;
};

struct ChildReady final {
    std::uint32_t structure_size = sizeof(ChildReady);
    std::uint32_t wire_version = kWireVersion;
    std::uint32_t success = 0;
    std::uint32_t consumer_index = 0;
    std::uint32_t gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::system_error);
    HRESULT hresult = E_FAIL;
    char message[384]{};
};

struct ChildArmed final {
    std::uint32_t structure_size = sizeof(ChildArmed);
    std::uint32_t wire_version = kWireVersion;
    std::uint32_t success = 0;
    std::uint32_t consumer_index = 0;
};

struct ChildClosed final {
    std::uint32_t structure_size = sizeof(ChildClosed);
    std::uint32_t wire_version = kWireVersion;
    std::uint32_t success = 0;
    std::uint32_t consumer_index = 0;
    std::uint32_t gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::system_error);
    HRESULT hresult = E_FAIL;
    char message[384]{};
};

struct ChildResult final {
    std::uint32_t structure_size = sizeof(ChildResult);
    std::uint32_t wire_version = kWireVersion;
    std::uint32_t success = 0;
    std::uint32_t consumer_index = 0;
    std::uint32_t gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::system_error);
    HRESULT hresult = E_FAIL;
    std::uint64_t total_acquisitions = 0;
    std::uint64_t private_copy_commands = 0;
    std::uint64_t distinct_sequences = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t validations = 0;
    std::uint64_t validation_checksum = 0;
    std::uint64_t run_nanoseconds = 0;
    char message[384]{};
};

static_assert(std::is_trivially_copyable_v<ChildSetup>);
static_assert(std::is_trivially_copyable_v<ChildCommand>);
static_assert(std::is_trivially_copyable_v<ChildReady>);
static_assert(std::is_trivially_copyable_v<ChildArmed>);
static_assert(std::is_trivially_copyable_v<ChildClosed>);
static_assert(std::is_trivially_copyable_v<ChildResult>);
static_assert(std::is_trivially_copyable_v<gpu::SharedFrameBusRegistration>);

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

std::string gpu_error_text(const gpu::GpuError& error) {
    return std::string(error.what()) + " (status="
        + std::to_string(static_cast<unsigned>(error.status)) + ", hr="
        + std::to_string(static_cast<long long>(error.hresult)) + ')';
}

void require(bool condition, std::string message) {
    if (!condition) fail(std::move(message));
}

void require_gpu(const gpu::GpuError& result, std::string_view operation) {
    if (!result) {
        fail(std::string(operation) + " failed: " + gpu_error_text(result));
    }
}

void copy_message(char (&destination)[384], std::string_view message) noexcept {
    const std::size_t count = std::min(message.size(), sizeof(destination) - 1);
    std::memcpy(destination, message.data(), count);
    destination[count] = '\0';
}

std::uint32_t parse_number(std::wstring_view text, const char* option) {
    if (text.empty()) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }
    std::uint64_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            throw std::invalid_argument(std::string(option) + " must be an integer");
        }
        value = value * 10u + static_cast<std::uint64_t>(character - L'0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument(std::string(option) + " is too large");
        }
    }
    return static_cast<std::uint32_t>(value);
}

std::wstring_view option_value(
    int& index,
    int argc,
    wchar_t* argv[],
    std::wstring_view argument,
    std::wstring_view name) {
    if (argument == name) {
        if (++index >= argc) {
            throw std::invalid_argument("benchmark option requires a value");
        }
        return argv[index];
    }
    const std::wstring prefix = std::wstring(name) + L"=";
    if (argument.starts_with(prefix)) return argument.substr(prefix.size());
    return {};
}

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.show_help = true;
        } else if (argument == L"--mode" || argument.starts_with(L"--mode=")) {
            const std::wstring_view value = option_value(
                index, argc, argv, argument, L"--mode");
            if (value == L"broadcast") {
                options.mode = BenchmarkMode::broadcast;
            } else if (value == L"roi-publish-compare") {
                options.mode = BenchmarkMode::roi_publish_compare;
            } else {
                throw std::invalid_argument(
                    "--mode must be broadcast or roi-publish-compare");
            }
        } else if (argument == L"--size" || argument.starts_with(L"--size=")) {
            options.size = parse_number(
                option_value(index, argc, argv, argument, L"--size"), "--size");
        } else if (
            argument == L"--consumers"
            || argument.starts_with(L"--consumers=")) {
            options.consumers = parse_number(
                option_value(index, argc, argv, argument, L"--consumers"),
                "--consumers");
        } else if (
            argument == L"--frames" || argument.starts_with(L"--frames=")) {
            options.frames = parse_number(
                option_value(index, argc, argv, argument, L"--frames"),
                "--frames");
        } else if (
            argument == L"--warmup" || argument.starts_with(L"--warmup=")) {
            options.warmup = parse_number(
                option_value(index, argc, argv, argument, L"--warmup"),
                "--warmup");
        } else if (
            argument == L"--timeout-ms"
            || argument.starts_with(L"--timeout-ms=")) {
            options.timeout_ms = parse_number(
                option_value(index, argc, argv, argument, L"--timeout-ms"),
                "--timeout-ms");
        } else if (
            argument == L"--rounds" || argument.starts_with(L"--rounds=")) {
            options.rounds = parse_number(
                option_value(index, argc, argv, argument, L"--rounds"),
                "--rounds");
        } else {
            throw std::invalid_argument("unknown benchmark option");
        }
    }

    if (options.size != 320 && options.size != 640) {
        throw std::invalid_argument("--size must be 320 or 640");
    }
    if (options.consumers != 1
        && options.consumers != 2
        && options.consumers != 4) {
        throw std::invalid_argument("--consumers must be 1, 2, or 4");
    }
    if (options.frames == 0 || options.frames > kMaximumFrames) {
        throw std::invalid_argument("--frames must be in [1, 10000000]");
    }
    if (options.warmup > kMaximumFrames) {
        throw std::invalid_argument("--warmup must be at most 10000000");
    }
    if (options.timeout_ms < 1'000 || options.timeout_ms > kMaximumTimeoutMs) {
        throw std::invalid_argument("--timeout-ms must be in [1000, 3600000]");
    }
    if (options.rounds < 2 || options.rounds > kMaximumCompareRounds) {
        throw std::invalid_argument("--rounds must be in [2, 32]");
    }
    const std::uint64_t total = static_cast<std::uint64_t>(options.warmup)
        + options.frames;
    if (total >= (std::uint64_t{1} << 30u)) {
        throw std::invalid_argument("warmup plus frames exceeds marker capacity");
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
    DeviceContext result;
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
        &result.device,
        &created_level,
        &result.context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            adapter,
            driver,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &result.device,
            &created_level,
            &result.context);
    }
    if (FAILED(hr)) {
        fail("D3D11CreateDevice failed: " + std::to_string(hr));
    }
    return result;
}

DeviceContext create_device_for_registration(
    const gpu::SharedFrameBusRegistration& registration) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        fail("CreateDXGIFactory1 failed: " + std::to_string(hr));
    }
    ComPtr<IDXGIAdapter1> selected;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) fail("EnumAdapters1 failed: " + std::to_string(hr));
        DXGI_ADAPTER_DESC1 description{};
        hr = candidate->GetDesc1(&description);
        if (FAILED(hr)) {
            fail("IDXGIAdapter1::GetDesc1 failed: " + std::to_string(hr));
        }
        if (description.AdapterLuid.LowPart == registration.adapter_luid_low
            && description.AdapterLuid.HighPart == registration.adapter_luid_high) {
            selected = std::move(candidate);
            break;
        }
    }
    if (selected == nullptr) {
        fail("registration adapter was not found in the consumer process");
    }
    return create_device(selected.Get());
}

std::string adapter_name(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr)) return "unknown";
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr) || FAILED(adapter->GetDesc(&description))) return "unknown";
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, description.Description, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return "unknown";
    std::string result(static_cast<std::size_t>(required), '\0');
    (void)WideCharToMultiByte(
        CP_UTF8,
        0,
        description.Description,
        -1,
        result.data(),
        required,
        nullptr,
        nullptr);
    result.pop_back();
    return result;
}

std::uint32_t coordinate_pixel(std::uint32_t linear_index) noexcept {
    return 0x8000'0000u | linear_index;
}

std::uint32_t frame_marker(std::uint64_t sequence) noexcept {
    return 0x4000'0000u | static_cast<std::uint32_t>(sequence);
}

D3D11_TEXTURE2D_DESC default_texture_description(std::uint32_t size) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = size;
    description.Height = size;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return description;
}

ComPtr<ID3D11Texture2D> create_source_texture(
    ID3D11Device* device,
    std::uint32_t size) {
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(size) * size);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        pixels[index] = coordinate_pixel(static_cast<std::uint32_t>(index));
    }
    pixels[0] = frame_marker(0);
    const D3D11_TEXTURE2D_DESC description = default_texture_description(size);
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = size * sizeof(std::uint32_t);
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, &initial, &texture);
    if (FAILED(hr)) {
        fail("source texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

struct RoiRegion final {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    D3D11_BOX box{};
};

RoiRegion make_roi_region(std::uint32_t size) noexcept {
    const std::uint32_t x = (kRoiSourceWidth - size) / 2u;
    const std::uint32_t y = (kRoiSourceHeight - size) / 2u;
    return {x, y, {x, y, 0, x + size, y + size, 1}};
}

ComPtr<ID3D11Texture2D> create_roi_source_texture(
    ID3D11Device* device,
    std::uint32_t size,
    const RoiRegion& roi) {
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(kRoiSourceWidth) * kRoiSourceHeight,
        0xdead'beefu);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::uint32_t linear = y * size + x;
            pixels[static_cast<std::size_t>(roi.y + y) * kRoiSourceWidth
                + roi.x + x] = coordinate_pixel(linear);
        }
    }
    pixels[static_cast<std::size_t>(roi.y) * kRoiSourceWidth + roi.x] =
        frame_marker(0);

    D3D11_TEXTURE2D_DESC description{};
    description.Width = kRoiSourceWidth;
    description.Height = kRoiSourceHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = kRoiSourceWidth * sizeof(std::uint32_t);

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, &initial, &texture);
    if (FAILED(hr)) {
        fail("ROI source texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

ComPtr<ID3D11Texture2D> create_private_texture(
    ID3D11Device* device,
    std::uint32_t size) {
    const D3D11_TEXTURE2D_DESC description = default_texture_description(size);
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, nullptr, &texture);
    if (FAILED(hr)) {
        fail("private texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

ComPtr<ID3D11Texture2D> create_staging_texture(
    ID3D11Device* device,
    std::uint32_t size) {
    D3D11_TEXTURE2D_DESC description = default_texture_description(size);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, nullptr, &texture);
    if (FAILED(hr)) {
        fail("staging texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

ComPtr<ID3D11Query> create_event_query(ID3D11Device* device) {
    D3D11_QUERY_DESC description{};
    description.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> query;
    const HRESULT hr = device->CreateQuery(&description, &query);
    if (FAILED(hr)) {
        fail("D3D11 event query creation failed: " + std::to_string(hr));
    }
    return query;
}

void wait_for_event(
    ID3D11DeviceContext* context,
    ID3D11Query* query,
    Clock::time_point deadline) {
    context->Flush();
    for (;;) {
        BOOL complete = FALSE;
        const HRESULT hr = context->GetData(
            query, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && complete != FALSE) return;
        if (FAILED(hr)) {
            fail("D3D11 event query read failed: " + std::to_string(hr));
        }
        if (Clock::now() >= deadline) {
            fail("D3D11 event query timed out");
        }
        SwitchToThread();
    }
}

void update_source_marker(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    std::uint64_t sequence) noexcept {
    const std::uint32_t marker = frame_marker(sequence);
    constexpr D3D11_BOX box{0, 0, 0, 1, 1, 1};
    context->UpdateSubresource(
        source, 0, &box, &marker, sizeof(marker), sizeof(marker));
}

void update_roi_source_marker(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    const RoiRegion& roi,
    std::uint64_t sequence) noexcept {
    const std::uint32_t marker = frame_marker(sequence);
    const D3D11_BOX box{roi.x, roi.y, 0, roi.x + 1, roi.y + 1, 1};
    context->UpdateSubresource(
        source, 0, &box, &marker, sizeof(marker), sizeof(marker));
}

std::uint64_t validate_private_texture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    ID3D11Texture2D* staging,
    std::uint32_t size,
    std::uint64_t sequence) {
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fail("private texture validation Map failed: " + std::to_string(hr));
    }

    bool mismatch = false;
    std::uint32_t mismatch_x = 0;
    std::uint32_t mismatch_y = 0;
    std::uint32_t actual = 0;
    std::uint32_t expected = 0;
    std::uint64_t checksum = 1469598103934665603ull;
    for (std::uint32_t y = 0; y < size && !mismatch; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch);
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::uint32_t linear = y * size + x;
            expected = linear == 0
                ? frame_marker(sequence) : coordinate_pixel(linear);
            actual = row[x];
            checksum ^= actual;
            checksum *= 1099511628211ull;
            if (actual != expected) {
                mismatch = true;
                mismatch_x = x;
                mismatch_y = y;
                break;
            }
        }
    }
    context->Unmap(staging, 0);
    if (mismatch) {
        fail("private texture mismatch at (" + std::to_string(mismatch_x)
            + ',' + std::to_string(mismatch_y) + ") for sequence "
            + std::to_string(sequence) + ": expected="
            + std::to_string(expected) + ", actual=" + std::to_string(actual));
    }
    return checksum;
}

gpu::SharedFrameBusConfig make_bus_config(const Options& options) noexcept {
    gpu::SharedFrameBusConfig config;
    config.width = options.size;
    config.height = options.size;
    config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
    config.slot_count = kSlotCount;
    config.max_consumers = options.consumers;
    return config;
}

ChildReady ready_error(std::string_view message) noexcept {
    ChildReady result;
    copy_message(result.message, message);
    return result;
}

ChildResult result_error(
    std::uint32_t consumer_index,
    std::string_view message) noexcept {
    ChildResult result;
    result.consumer_index = consumer_index;
    copy_message(result.message, message);
    return result;
}

ChildResult consume_frames(
    const ChildSetup& setup,
    DeviceContext& d3d,
    gpu::SharedFrameBusConsumer& consumer,
    ID3D11Texture2D* private_texture,
    ID3D11Texture2D* staging_texture) {
    ChildResult result;
    result.consumer_index = setup.registration.consumer_index;
    result.gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::ok);
    result.hresult = S_OK;
    const Clock::time_point begin = Clock::now();
    const Clock::time_point deadline = begin
        + std::chrono::milliseconds(setup.timeout_ms);
    std::uint64_t previous_observed = 0;
    std::uint64_t previous_measured = setup.warmup_sequence;

    while (result.last_sequence < setup.target_sequence) {
        const Clock::time_point now = Clock::now();
        if (now >= deadline) {
            fail("consumer timed out before observing target sequence "
                + std::to_string(setup.target_sequence));
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        const std::uint32_t acquire_timeout = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(remaining, 1, kAcquireSliceMs));

        gpu::SharedFrameBusFrameLease lease;
        const gpu::GpuError acquired = consumer.acquire_latest(
            acquire_timeout, lease);
        if (!acquired) {
            if (acquired.status == gpu::GpuStatus::timeout) continue;
            fail("acquire_latest failed: " + gpu_error_text(acquired));
        }
        const gpu::SharedFrameBusFrameInfo info = lease.info();
        require(
            info.width == setup.size
                && info.height == setup.size
                && info.format == DXGI_FORMAT_B8G8R8A8_UNORM,
            "acquired frame metadata does not match the benchmark contract");
        require(info.sequence > previous_observed,
            "consumer acquired a non-increasing sequence");
        require(info.sequence <= setup.target_sequence,
            "consumer acquired a sequence beyond the benchmark target");
        previous_observed = info.sequence;
        ++result.total_acquisitions;

        d3d.context->CopyResource(private_texture, lease.texture());
        ++result.private_copy_commands;
        const gpu::GpuError released = consumer.release(lease);
        if (!released) {
            fail("release failed: " + gpu_error_text(released));
        }

        if (info.sequence <= setup.warmup_sequence) continue;
        if (result.distinct_sequences == 0) result.first_sequence = info.sequence;
        result.sequence_gaps += info.sequence - previous_measured - 1;
        previous_measured = info.sequence;
        result.last_sequence = info.sequence;
        ++result.distinct_sequences;

        const bool validate = info.sequence == setup.target_sequence
            || result.distinct_sequences % kValidationPeriod == 0;
        if (validate) {
            const std::uint64_t checksum = validate_private_texture(
                d3d.context.Get(),
                private_texture,
                staging_texture,
                setup.size,
                info.sequence);
            result.validation_checksum ^= checksum
                + 0x9e3779b97f4a7c15ull
                + (result.validation_checksum << 6u)
                + (result.validation_checksum >> 2u);
            ++result.validations;
        }
    }

    result.run_nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - begin).count());
    const std::uint64_t expected = setup.target_sequence
        - setup.warmup_sequence;
    require(result.distinct_sequences + result.sequence_gaps == expected,
        "consumer distinct/gap accounting does not cover the measured range");
    require(result.last_sequence == setup.target_sequence,
        "consumer did not finish on the target sequence");
    require(result.validations != 0,
        "consumer did not perform a full-frame validation");
    const HRESULT removed = d3d.device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        fail("consumer device was removed: " + std::to_string(removed));
    }
    result.success = 1;
    copy_message(result.message, "ok");
    return result;
}

int run_consumer_child(HANDLE input, HANDLE output) noexcept {
    gpu::SharedFrameBusConsumer consumer;
    std::uint32_t consumer_index = 0;
    bool ready_sent = false;
    try {
        ChildSetup setup;
        if (!read_exact(input, &setup, sizeof(setup))) {
            fail("consumer did not receive its setup");
        }
        if (setup.structure_size != sizeof(ChildSetup)
            || setup.wire_version != kWireVersion
            || (setup.size != 320 && setup.size != 640)
            || setup.target_sequence <= setup.warmup_sequence
            || setup.timeout_ms < 1'000) {
            fail("consumer received malformed setup");
        }
        consumer_index = setup.registration.consumer_index;
        DeviceContext d3d = create_device_for_registration(setup.registration);
        require_gpu(
            gpu::SharedFrameBusConsumer::open(
                d3d.device.Get(), setup.registration, true, consumer),
            "open shared frame bus consumer");
        auto private_texture = create_private_texture(d3d.device.Get(), setup.size);
        auto staging_texture = create_staging_texture(d3d.device.Get(), setup.size);

        ChildReady ready;
        ready.success = 1;
        ready.consumer_index = consumer_index;
        ready.gpu_status = static_cast<std::uint32_t>(gpu::GpuStatus::ok);
        ready.hresult = S_OK;
        copy_message(ready.message, "ok");
        if (!write_exact(output, &ready, sizeof(ready))) return 3;
        ready_sent = true;

        ChildCommand start;
        if (!read_exact(input, &start, sizeof(start))
            || start.structure_size != sizeof(ChildCommand)
            || start.command != kStartCommand) {
            fail("consumer received malformed start command");
        }
        ChildArmed armed;
        armed.success = 1;
        armed.consumer_index = consumer_index;
        if (!write_exact(output, &armed, sizeof(armed))) return 4;

        ChildResult result;
        try {
            result = consume_frames(
                setup,
                d3d,
                consumer,
                private_texture.Get(),
                staging_texture.Get());
        } catch (const std::exception& error) {
            result = result_error(consumer_index, error.what());
        }
        if (!write_exact(output, &result, sizeof(result))) return 5;

        ChildCommand close;
        if (!read_exact(input, &close, sizeof(close))
            || close.structure_size != sizeof(ChildCommand)
            || close.command != kCloseCommand) {
            return 6;
        }
        const gpu::GpuError closed = consumer.close();
        ChildClosed close_response;
        close_response.consumer_index = consumer_index;
        close_response.success = closed ? 1u : 0u;
        close_response.gpu_status = static_cast<std::uint32_t>(closed.status);
        close_response.hresult = closed.hresult;
        copy_message(
            close_response.message,
            closed ? std::string_view("ok") : std::string_view(closed.what()));
        if (!write_exact(output, &close_response, sizeof(close_response))) return 7;

        ChildCommand exit;
        if (!read_exact(input, &exit, sizeof(exit))
            || exit.structure_size != sizeof(ChildCommand)
            || exit.command != kExitCommand) {
            return 8;
        }
        return 0;
    } catch (const std::exception& error) {
        if (consumer.initialized()) (void)consumer.close();
        if (!ready_sent) {
            ChildReady ready = ready_error(error.what());
            ready.consumer_index = consumer_index;
            (void)write_exact(output, &ready, sizeof(ready));
        } else {
            const ChildResult result = result_error(consumer_index, error.what());
            (void)write_exact(output, &result, sizeof(result));
        }
        return 2;
    }
}

struct PublishMeasurement final {
    gpu::SharedFrameBusStats before{};
    gpu::SharedFrameBusStats after{};
    Clock::time_point begin{};
    Clock::time_point end{};
    double publish_api_seconds = 0.0;
};

void publish_successes(
    gpu::SharedFrameBusPublisher& publisher,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    std::uint64_t count,
    Clock::time_point deadline,
    double* publish_api_seconds) {
    const std::uint64_t target = publisher.sequence() + count;
    while (publisher.sequence() < target) {
        if (Clock::now() >= deadline) {
            fail("publisher timed out before sequence " + std::to_string(target));
        }
        const std::uint64_t next = publisher.sequence() + 1;
        update_source_marker(context, source, next);
        for (;;) {
            const Clock::time_point call_begin = Clock::now();
            const gpu::GpuError published = publisher.publish(source, 0);
            const Clock::time_point call_end = Clock::now();
            if (publish_api_seconds != nullptr) {
                *publish_api_seconds += std::chrono::duration<double>(
                    call_end - call_begin).count();
            }
            if (published) break;
            if (published.status != gpu::GpuStatus::timeout) {
                fail("copy publish failed: " + gpu_error_text(published));
            }
            if (Clock::now() >= deadline) {
                fail("publisher ran out of reclaimable slots until timeout");
            }
            SwitchToThread();
        }
    }
}

const char* topology_name(RoiPublishTopology topology) noexcept {
    return topology == RoiPublishTopology::staged ? "staged" : "direct";
}

const char* topology_description(RoiPublishTopology topology) noexcept {
    return topology == RoiPublishTopology::staged
        ? "source ROI -> private ROI texture -> SharedFrameBus slot"
        : "source ROI -> SharedFrameBusWriteLease slot -> commit";
}

void publish_roi_successes(
    gpu::SharedFrameBusPublisher& publisher,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    ID3D11Texture2D* private_roi,
    const RoiRegion& roi,
    RoiPublishTopology topology,
    std::uint64_t count,
    Clock::time_point deadline,
    double* topology_api_seconds) {
    const std::uint64_t target = publisher.sequence() + count;
    while (publisher.sequence() < target) {
        if (Clock::now() >= deadline) {
            fail(std::string(topology_name(topology))
                + " ROI publisher timed out before sequence "
                + std::to_string(target));
        }

        const Clock::time_point frame_begin = Clock::now();
        const std::uint64_t next = publisher.sequence() + 1;
        update_roi_source_marker(context, source, roi, next);
        if (topology == RoiPublishTopology::staged) {
            context->CopySubresourceRegion(
                private_roi, 0, 0, 0, 0, source, 0, &roi.box);
        }

        for (;;) {
            gpu::GpuError published;
            if (topology == RoiPublishTopology::staged) {
                published = publisher.publish(private_roi, 0);
            } else {
                gpu::SharedFrameBusWriteLease lease;
                published = publisher.begin_publish(0, lease);
                if (published) {
                    context->CopySubresourceRegion(
                        lease.texture(), 0, 0, 0, 0, source, 0, &roi.box);
                    published = publisher.commit(std::move(lease));
                }
            }

            if (published) break;
            if (published.status != gpu::GpuStatus::timeout) {
                fail(std::string(topology_name(topology))
                    + " ROI publish failed: " + gpu_error_text(published));
            }
            if (Clock::now() >= deadline) {
                fail(std::string(topology_name(topology))
                    + " ROI publisher ran out of reclaimable slots");
            }
            SwitchToThread();
        }

        if (topology_api_seconds != nullptr) {
            *topology_api_seconds += std::chrono::duration<double>(
                Clock::now() - frame_begin).count();
        }
    }
}

struct RoiTopologyRun final {
    RoiPublishTopology topology = RoiPublishTopology::staged;
    std::uint64_t attempts = 0;
    std::uint64_t published = 0;
    std::uint64_t copied = 0;
    std::uint64_t direct = 0;
    std::uint64_t no_slot = 0;
    double cpu_submit_seconds = 0.0;
    double publisher_gpu_seconds = 0.0;
    double consumer_complete_seconds = 0.0;
    double topology_api_seconds = 0.0;
    std::vector<ChildResult> consumers;
    std::vector<DWORD> consumer_process_ids;
};

std::uint64_t delta(std::uint64_t after, std::uint64_t before) {
    require(after >= before, "publisher statistic moved backwards");
    return after - before;
}

double seconds_between(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double>(end - begin).count();
}

double rate(std::uint64_t count, double seconds) noexcept {
    return seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
}

double mebibytes(std::uint64_t bytes) noexcept {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void require_ready(const ChildReady& ready, const ChildProcess& child) {
    if (ready.structure_size != sizeof(ChildReady)
        || ready.wire_version != kWireVersion
        || ready.success == 0) {
        fail("consumer pid=" + std::to_string(child.process_id())
            + " failed to open: " + ready.message + " (status="
            + std::to_string(ready.gpu_status) + ", hr="
            + std::to_string(static_cast<long long>(ready.hresult)) + ')');
    }
}

RoiTopologyRun run_roi_topology(
    const Options& options,
    DeviceContext& d3d,
    ID3D11Texture2D* source,
    ID3D11Texture2D* private_roi,
    const RoiRegion& roi,
    RoiPublishTopology topology) {
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), make_bus_config(options), publisher),
        "create ROI comparison bus");

    std::vector<ChildProcess> children;
    children.reserve(options.consumers);
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        children.push_back(ChildProcess::spawn(L"--consumer-child"));
    }
    std::vector<gpu::SharedFrameBusRegistration> registrations(
        options.consumers);
    const std::uint64_t target_sequence = static_cast<std::uint64_t>(
        options.warmup) + options.frames;
    const std::uint32_t child_run_timeout = options.timeout_ms * 2u;
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        require_gpu(
            publisher.register_consumer(
                children[index].process(), registrations[index]),
            "register ROI comparison consumer");
        ChildSetup setup;
        setup.size = options.size;
        setup.timeout_ms = child_run_timeout;
        setup.warmup_sequence = options.warmup;
        setup.target_sequence = target_sequence;
        setup.registration = registrations[index];
        children[index].send(setup, "ROI comparison setup");
    }
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        const ChildReady ready = children[index].receive<ChildReady>(
            options.timeout_ms, "ROI comparison open response");
        require_ready(ready, children[index]);
        require(ready.consumer_index == registrations[index].consumer_index,
            "ROI comparison consumer opened the wrong registration");
    }

    auto completion_query = create_event_query(d3d.device.Get());
    const Clock::time_point warmup_deadline = Clock::now()
        + std::chrono::milliseconds(options.timeout_ms);
    publish_roi_successes(
        publisher,
        d3d.context.Get(),
        source,
        private_roi,
        roi,
        topology,
        options.warmup,
        warmup_deadline,
        nullptr);
    d3d.context->End(completion_query.Get());
    wait_for_event(d3d.context.Get(), completion_query.Get(), warmup_deadline);

    RoiTopologyRun result;
    result.topology = topology;
    const gpu::SharedFrameBusStats before = publisher.stats();
    const Clock::time_point begin = Clock::now();
    const Clock::time_point publish_deadline = begin
        + std::chrono::milliseconds(options.timeout_ms);
    publish_roi_successes(
        publisher,
        d3d.context.Get(),
        source,
        private_roi,
        roi,
        topology,
        options.frames,
        publish_deadline,
        &result.topology_api_seconds);
    d3d.context->End(completion_query.Get());
    const Clock::time_point submitted = Clock::now();
    wait_for_event(d3d.context.Get(), completion_query.Get(), publish_deadline);
    const Clock::time_point gpu_complete = Clock::now();
    const gpu::SharedFrameBusStats after = publisher.stats();

    result.cpu_submit_seconds = seconds_between(begin, submitted);
    result.publisher_gpu_seconds = seconds_between(begin, gpu_complete);
    result.attempts = delta(after.publish_attempts, before.publish_attempts);
    result.published = delta(after.published_frames, before.published_frames);
    result.copied = delta(after.copied_publishes, before.copied_publishes);
    result.direct = delta(after.direct_publishes, before.direct_publishes);
    result.no_slot = delta(after.no_slot, before.no_slot);
    require(result.published == options.frames,
        std::string(topology_name(topology))
            + " did not publish every requested measured frame");
    require(result.attempts == result.published + result.no_slot,
        std::string(topology_name(topology))
            + " publish attempt accounting is inconsistent");
    require(result.no_slot == 0,
        std::string(topology_name(topology))
            + " encountered unexpected backpressure in the producer-only window");
    require(
        topology == RoiPublishTopology::staged
            ? result.copied == options.frames && result.direct == 0
            : result.direct == options.frames && result.copied == 0,
        std::string(topology_name(topology))
            + " used the wrong SharedFrameBus publication API");

    ChildCommand start;
    start.command = kStartCommand;
    for (ChildProcess& child : children) {
        child.send(start, "ROI comparison validation start command");
    }
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        const ChildArmed armed = children[index].receive<ChildArmed>(
            options.timeout_ms, "ROI comparison validation armed response");
        require(
            armed.structure_size == sizeof(ChildArmed)
                && armed.wire_version == kWireVersion
                && armed.success != 0
                && armed.consumer_index == registrations[index].consumer_index,
            "ROI comparison consumer returned a malformed armed response");
    }

    result.consumers.resize(options.consumers);
    result.consumer_process_ids.resize(options.consumers);
    const std::uint32_t response_timeout = child_run_timeout + 5'000u;
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        result.consumer_process_ids[index] = children[index].process_id();
        result.consumers[index] = children[index].receive<ChildResult>(
            response_timeout, "ROI comparison result");
        const ChildResult& child_result = result.consumers[index];
        require(
            child_result.structure_size == sizeof(ChildResult)
                && child_result.wire_version == kWireVersion
                && child_result.consumer_index
                    == registrations[index].consumer_index
                && child_result.success != 0,
            "ROI comparison consumer failed validation");
    }
    result.consumer_complete_seconds = seconds_between(begin, Clock::now());

    std::string cleanup_failure;
    ChildCommand close;
    close.command = kCloseCommand;
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        try {
            children[index].send(close, "ROI comparison close command");
            const ChildClosed closed = children[index].receive<ChildClosed>(
                5'000, "ROI comparison close response");
            if (closed.structure_size != sizeof(ChildClosed)
                || closed.wire_version != kWireVersion
                || closed.consumer_index != registrations[index].consumer_index
                || closed.success == 0) {
                if (cleanup_failure.empty()) {
                    cleanup_failure = "ROI comparison consumer close failed: "
                        + std::string(closed.message);
                }
            }
        } catch (const std::exception& error) {
            if (cleanup_failure.empty()) cleanup_failure = error.what();
        }
        const gpu::GpuError unregistered = publisher.unregister_consumer(
            registrations[index], options.timeout_ms);
        if (!unregistered && cleanup_failure.empty()) {
            cleanup_failure = "ROI comparison unregister failed: "
                + gpu_error_text(unregistered);
        }
    }
    ChildCommand exit;
    exit.command = kExitCommand;
    for (ChildProcess& child : children) {
        try {
            child.send(exit, "ROI comparison exit command");
            child.wait_for_exit(5'000);
        } catch (const std::exception& error) {
            if (cleanup_failure.empty()) cleanup_failure = error.what();
        }
    }
    if (!cleanup_failure.empty()) fail(std::move(cleanup_failure));
    return result;
}

double median(std::vector<double> values) {
    require(!values.empty(), "cannot take the median of an empty sample set");
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2u;
    return (values.size() & 1u) != 0u
        ? values[middle]
        : (values[middle - 1u] + values[middle]) / 2.0;
}

double median_metric(
    const std::vector<RoiTopologyRun>& runs,
    double RoiTopologyRun::*member) {
    std::vector<double> values;
    values.reserve(runs.size());
    for (const RoiTopologyRun& run : runs) values.push_back(run.*member);
    return median(std::move(values));
}

double median_paired_ratio(
    const std::vector<RoiTopologyRun>& staged,
    const std::vector<RoiTopologyRun>& direct,
    double RoiTopologyRun::*member) {
    require(staged.size() == direct.size() && !staged.empty(),
        "paired ROI comparison samples are inconsistent");
    std::vector<double> ratios;
    ratios.reserve(staged.size());
    for (std::size_t index = 0; index < staged.size(); ++index) {
        const double denominator = direct[index].*member;
        require(denominator > 0.0, "direct ROI comparison interval is zero");
        ratios.push_back((staged[index].*member) / denominator);
    }
    return median(std::move(ratios));
}

std::uint64_t sum_counter(
    const std::vector<RoiTopologyRun>& runs,
    std::uint64_t RoiTopologyRun::*member) noexcept {
    std::uint64_t total = 0;
    for (const RoiTopologyRun& run : runs) total += run.*member;
    return total;
}

void print_roi_topology_summary(
    std::string_view label,
    const std::vector<RoiTopologyRun>& runs,
    std::uint32_t frames) {
    const double cpu = median_metric(runs, &RoiTopologyRun::cpu_submit_seconds);
    const double gpu = median_metric(runs, &RoiTopologyRun::publisher_gpu_seconds);
    const double consumer = median_metric(
        runs, &RoiTopologyRun::consumer_complete_seconds);
    const double api = median_metric(runs, &RoiTopologyRun::topology_api_seconds);
    std::cout
        << label << " - " << topology_description(runs.front().topology) << '\n'
        << "  successful publishes/round: " << frames << '\n'
        << "  CPU measured submit median: " << cpu * 1'000.0 << " ms ("
        << rate(frames, cpu) << " publishes/s)\n"
        << "  producer event-query complete median: " << gpu * 1'000.0
        << " ms (" << rate(frames, gpu) << " publishes/s)\n"
        << "  measured-start to final consumer validation median: "
        << consumer * 1'000.0 << " ms\n"
        << "  cumulative measured topology calls median: " << api * 1'000.0
        << " ms\n"
        << "  attempts / no-slot across rounds: "
        << sum_counter(runs, &RoiTopologyRun::attempts) << " / "
        << sum_counter(runs, &RoiTopologyRun::no_slot) << '\n';
}

void run_roi_publish_compare(const Options& options) {
    DeviceContext d3d = create_device();
    const RoiRegion roi = make_roi_region(options.size);
    auto source = create_roi_source_texture(d3d.device.Get(), options.size, roi);
    auto private_roi = create_private_texture(d3d.device.Get(), options.size);

    std::vector<RoiTopologyRun> staged;
    std::vector<RoiTopologyRun> direct;
    staged.reserve(options.rounds);
    direct.reserve(options.rounds);
    for (std::uint32_t round = 0; round < options.rounds; ++round) {
        const bool staged_first = (round & 1u) == 0u;
        if (staged_first) {
            staged.push_back(run_roi_topology(
                options, d3d, source.Get(), private_roi.Get(), roi,
                RoiPublishTopology::staged));
            direct.push_back(run_roi_topology(
                options, d3d, source.Get(), private_roi.Get(), roi,
                RoiPublishTopology::direct));
        } else {
            direct.push_back(run_roi_topology(
                options, d3d, source.Get(), private_roi.Get(), roi,
                RoiPublishTopology::direct));
            staged.push_back(run_roi_topology(
                options, d3d, source.Get(), private_roi.Get(), roi,
                RoiPublishTopology::staged));
        }
    }

    const double cpu_ratio = median_paired_ratio(
        staged, direct, &RoiTopologyRun::cpu_submit_seconds);
    const double gpu_ratio = median_paired_ratio(
        staged, direct, &RoiTopologyRun::publisher_gpu_seconds);
    const double consumer_ratio = median_paired_ratio(
        staged, direct, &RoiTopologyRun::consumer_complete_seconds);
    const std::uint64_t roi_bytes = static_cast<std::uint64_t>(options.size)
        * options.size * sizeof(std::uint32_t);

    std::cout << std::fixed << std::setprecision(3)
        << "FluxCap SharedFrameBus staged/direct ROI publish comparison\n"
        << "scope: synthetic resident 2560x1600 BGRA8 source on one producer "
           "D3D11 device; NOT WGC/desktop unique-frame FPS and NOT display "
           "refresh rate\n"
        << "fairness: each topology uses a fresh bus epoch with the same "
           "slot count, registered consumer count, ROI, warmup, and fence "
           "protocol; "
           "consumers start after the producer event query, then use the same "
           "ready/done protocol for final cross-process pixel validation; "
           "round order alternates AB/BA\n"
        << "timed publisher window: producer-only and zero-backpressure by "
           "construction; process startup and consumer scheduling are excluded\n"
        << "adapter: " << adapter_name(d3d.device.Get()) << '\n'
        << "ROI: " << options.size << 'x' << options.size
        << ", consumers=" << options.consumers
        << ", slots=" << kSlotCount
        << ", warmup=" << options.warmup
        << ", measured publishes/round=" << options.frames
        << ", rounds=" << options.rounds << "\n\n";
    print_roi_topology_summary("Staged", staged, options.frames);
    std::cout << '\n';
    print_roi_topology_summary("Direct", direct, options.frames);
    std::cout
        << "\nDirect improvement over staged (median paired round ratio)\n"
        << "  CPU submit: " << cpu_ratio << "x\n"
        << "  producer event-query completion: " << gpu_ratio << "x\n"
        << "  measured-start to final consumer validation wall interval: "
        << consumer_ratio << "x\n"
        << "  producer copy commands/publish: 2 -> 1\n"
        << "  API copy payload/publish: " << roi_bytes * 2u << " -> "
        << roi_bytes << " bytes (2.000x reduction)\n"
        << "  note: paired-ratio medians need not equal the quotient of the "
           "two independently reported median rates\n"
        << "Correctness: PASS (real child consumers, ready/done fences, final "
           "full-frame coordinate and sequence-marker validation in every run)\n"
        << "Interpretation: rates are successful synthetic bus publishes, not "
           "new WGC presentations.\n";
}

void run_parent(const Options& options) {
    DeviceContext d3d = create_device();
    auto source = create_source_texture(d3d.device.Get(), options.size);
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), make_bus_config(options), publisher),
        "create shared frame bus publisher");

    std::vector<ChildProcess> children;
    children.reserve(options.consumers);
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        children.push_back(ChildProcess::spawn(L"--consumer-child"));
    }
    std::vector<gpu::SharedFrameBusRegistration> registrations(
        options.consumers);
    const std::uint64_t target_sequence = static_cast<std::uint64_t>(
        options.warmup) + options.frames;
    const std::uint32_t child_run_timeout = options.timeout_ms * 2u;
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        require_gpu(
            publisher.register_consumer(
                children[index].process(), registrations[index]),
            "register child consumer");
        ChildSetup setup;
        setup.size = options.size;
        setup.timeout_ms = child_run_timeout;
        setup.warmup_sequence = options.warmup;
        setup.target_sequence = target_sequence;
        setup.registration = registrations[index];
        children[index].send(setup, "setup");
    }
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        const ChildReady ready = children[index].receive<ChildReady>(
            options.timeout_ms, "open response");
        require_ready(ready, children[index]);
        require(ready.consumer_index == registrations[index].consumer_index,
            "consumer opened the wrong registration index");
    }

    ChildCommand start;
    start.command = kStartCommand;
    for (ChildProcess& child : children) child.send(start, "start command");
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        const ChildArmed armed = children[index].receive<ChildArmed>(
            options.timeout_ms, "armed response");
        require(
            armed.structure_size == sizeof(ChildArmed)
                && armed.wire_version == kWireVersion
                && armed.success != 0
                && armed.consumer_index == registrations[index].consumer_index,
            "consumer returned a malformed armed response");
    }

    d3d.context->Flush();
    const Clock::time_point warmup_deadline = Clock::now()
        + std::chrono::milliseconds(options.timeout_ms);
    publish_successes(
        publisher,
        d3d.context.Get(),
        source.Get(),
        options.warmup,
        warmup_deadline,
        nullptr);

    PublishMeasurement measurement;
    measurement.before = publisher.stats();
    measurement.begin = Clock::now();
    const Clock::time_point publish_deadline = measurement.begin
        + std::chrono::milliseconds(options.timeout_ms);
    publish_successes(
        publisher,
        d3d.context.Get(),
        source.Get(),
        options.frames,
        publish_deadline,
        &measurement.publish_api_seconds);
    measurement.end = Clock::now();
    measurement.after = publisher.stats();

    std::vector<ChildResult> results(options.consumers);
    const std::uint32_t response_timeout = child_run_timeout + 5'000u;
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        results[index] = children[index].receive<ChildResult>(
            response_timeout, "benchmark result");
        require(
            results[index].structure_size == sizeof(ChildResult)
                && results[index].wire_version == kWireVersion
                && results[index].consumer_index
                    == registrations[index].consumer_index,
            "consumer returned a malformed benchmark result");
    }
    const Clock::time_point consumers_complete = Clock::now();

    std::string cleanup_failure;
    ChildCommand close;
    close.command = kCloseCommand;
    for (std::uint32_t index = 0; index < options.consumers; ++index) {
        try {
            children[index].send(close, "close command");
            const ChildClosed closed = children[index].receive<ChildClosed>(
                5'000, "close response");
            if (closed.structure_size != sizeof(ChildClosed)
                || closed.wire_version != kWireVersion
                || closed.consumer_index != registrations[index].consumer_index
                || closed.success == 0) {
                if (cleanup_failure.empty()) {
                    cleanup_failure = "consumer close failed: "
                        + std::string(closed.message) + " (status="
                        + std::to_string(closed.gpu_status) + ", hr="
                        + std::to_string(static_cast<long long>(closed.hresult))
                        + ')';
                }
            }
        } catch (const std::exception& error) {
            if (cleanup_failure.empty()) cleanup_failure = error.what();
        }
        const gpu::GpuError unregistered = publisher.unregister_consumer(
            registrations[index], options.timeout_ms);
        if (!unregistered && cleanup_failure.empty()) {
            cleanup_failure = "unregister consumer failed: "
                + gpu_error_text(unregistered);
        }
    }
    ChildCommand exit;
    exit.command = kExitCommand;
    for (ChildProcess& child : children) {
        try {
            child.send(exit, "exit command");
        } catch (const std::exception& error) {
            if (cleanup_failure.empty()) cleanup_failure = error.what();
        }
    }
    for (ChildProcess& child : children) {
        try {
            child.wait_for_exit(5'000);
        } catch (const std::exception& error) {
            if (cleanup_failure.empty()) cleanup_failure = error.what();
        }
    }
    if (!cleanup_failure.empty()) fail(std::move(cleanup_failure));
    for (const ChildResult& result : results) {
        if (result.success == 0) {
            fail("consumer index=" + std::to_string(result.consumer_index)
                + " benchmark failed: " + result.message + " (status="
                + std::to_string(result.gpu_status) + ", hr="
                + std::to_string(static_cast<long long>(result.hresult)) + ')');
        }
    }

    const std::uint64_t attempts = delta(
        measurement.after.publish_attempts,
        measurement.before.publish_attempts);
    const std::uint64_t published = delta(
        measurement.after.published_frames,
        measurement.before.published_frames);
    const std::uint64_t copied = delta(
        measurement.after.copied_publishes,
        measurement.before.copied_publishes);
    const std::uint64_t no_slot = delta(
        measurement.after.no_slot,
        measurement.before.no_slot);
    require(published == options.frames && copied == options.frames,
        "publisher did not copy-publish every requested measured frame");
    require(attempts == published + no_slot,
        "publish attempt accounting is inconsistent");

    const double publish_seconds = seconds_between(
        measurement.begin, measurement.end);
    const double completion_seconds = seconds_between(
        measurement.begin, consumers_complete);
    const std::uint64_t frame_bytes = static_cast<std::uint64_t>(options.size)
        * options.size * sizeof(std::uint32_t);
    const std::uint64_t bus_payload = frame_bytes * published;
    const std::uint64_t fanout_payload = bus_payload * options.consumers;

    std::cout << std::fixed << std::setprecision(3);
    std::cout
        << "FluxCap SharedFrameBus cross-process benchmark\n"
        << "scope: synthetic GPU broadcast; NOT WGC/desktop unique-frame FPS, "
           "NOT display refresh rate, and NOT physical memory bandwidth\n"
        << "timing: CPU-observed submission/API-return intervals; final consumer "
           "validation is the GPU completion boundary\n"
        << "pacing: none (publish retries immediately only when every slot is busy)\n"
        << "adapter: " << adapter_name(d3d.device.Get()) << '\n'
        << "texture: " << options.size << 'x' << options.size
        << " BGRA8, consumers=" << options.consumers
        << ", slots=" << kSlotCount
        << ", warmup=" << options.warmup
        << ", measured frames=" << options.frames << '\n'
        << "oracle: unique coordinate value per pixel plus a globally unique "
           "1x1 sequence marker; sampled outputs and every final output are "
           "fully validated\n\n";

    std::cout
        << "publisher measured window:\n"
        << "  publish attempts: " << attempts << '\n'
        << "  published frames: " << published << '\n'
        << "  copied publishes: " << copied << '\n'
        << "  no-slot retries: " << no_slot << '\n'
        << "  publish window: " << publish_seconds * 1000.0 << " ms\n"
        << "  copied publish API return rate: "
        << rate(copied, publish_seconds) << " frames/s\n"
        << "  all publish() calls per window second: "
        << rate(attempts, publish_seconds) << " calls/s\n"
        << "  cumulative time inside publish(): "
        << measurement.publish_api_seconds * 1000.0 << " ms\n"
        << "  measured-start to all consumers complete: "
        << completion_seconds * 1000.0 << " ms\n\n";

    for (const ChildResult& result : results) {
        const double run_seconds = static_cast<double>(result.run_nanoseconds)
            / 1'000'000'000.0;
        const double coverage = options.frames == 0 ? 0.0
            : 100.0 * static_cast<double>(result.distinct_sequences)
                / options.frames;
        std::cout
            << "consumer[" << result.consumer_index << "] pid="
            << children[result.consumer_index].process_id() << ":\n"
            << "  distinct measured sequences: " << result.distinct_sequences
            << " / " << options.frames << " (" << coverage << "%)\n"
            << "  sequence gaps: " << result.sequence_gaps << '\n'
            << "  first/last measured sequence: " << result.first_sequence
            << " / " << result.last_sequence << '\n'
            << "  total acquisitions/private CopyResource commands: "
            << result.total_acquisitions << " / "
            << result.private_copy_commands << '\n'
            << "  full-frame validations: " << result.validations << '\n'
            << "  validation checksum: " << result.validation_checksum << '\n'
            << "  child run interval (start barrier through final frame): "
            << run_seconds * 1000.0 << " ms\n"
            << "  distinct-sequence processing rate: "
            << rate(result.distinct_sequences, run_seconds) << " sequences/s\n";
    }

    std::cout
        << "\ncopy-topology API payload accounting (not a timed bandwidth claim):\n"
        << "  SharedFrameBus: 1 publisher CopyResource/frame = "
        << mebibytes(bus_payload) << " MiB submitted payload\n"
        << "  per-consumer fan-out topology: " << options.consumers
        << " CopyResource/frame = " << mebibytes(fanout_payload)
        << " MiB submitted payload\n"
        << "  command/payload ratio: " << options.consumers
        << ":1; consumer-private output copies are common downstream work\n";
}

void print_usage() {
    std::cout
        << "Usage: fluxcap_gpu_shared_bus_bench [options]\n"
        << "  --mode broadcast|roi-publish-compare\n"
        << "                         Benchmark mode (default broadcast)\n"
        << "  --size 320|640       Shared texture width and height (default 320)\n"
        << "  --consumers 1|2|4    Real consumer process count (default 2)\n"
        << "  --frames N           Successful measured publishes (default 10000)\n"
        << "  --warmup N           Successful unmeasured publishes (default 1000)\n"
        << "  --rounds N           AB/BA pairs for ROI comparison (default 4)\n"
        << "  --timeout-ms N        Per-phase safety timeout (default 120000)\n"
        << "  --help                Show this text\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--consumer-child") {
        HANDLE input = parse_inherited_handle(argv[2]);
        HANDLE output = parse_inherited_handle(argv[3]);
        if (input == nullptr || output == nullptr) return 64;
        const int result = run_consumer_child(input, output);
        CloseHandle(input);
        CloseHandle(output);
        return result;
    }

    try {
        const Options options = parse_options(argc, argv);
        if (options.show_help) {
            print_usage();
            return 0;
        }
        if (options.mode == BenchmarkMode::roi_publish_compare) {
            run_roi_publish_compare(options);
        } else {
            run_parent(options);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] SharedFrameBus benchmark: " << error.what() << '\n';
        print_usage();
        return 1;
    }
}
