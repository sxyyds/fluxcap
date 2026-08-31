#include <fluxcap/gpu.hpp>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
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

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 48;
constexpr std::uint32_t kChildTimeoutMs = 8'000;
constexpr std::uint32_t kOperationTimeoutMs = 3'000;

enum class WireOperation : std::uint32_t {
    open_consumer = 1,
    acquire_validate,
    validate_held,
    release,
    close,
    exit_process,
    producer_ready,
    producer_exit,
    death_scenario
};

enum class ProducerExitMode : std::uint32_t {
    graceful = 1,
    abrupt
};

struct WireCommand final {
    std::uint32_t structure_size = sizeof(WireCommand);
    WireOperation operation = WireOperation::exit_process;
    std::uint32_t timeout_ms = 0;
    std::uint32_t frame_id = 0;
};

struct WireResponse final {
    std::uint32_t structure_size = sizeof(WireResponse);
    WireOperation operation = WireOperation::exit_process;
    std::uint32_t success = 0;
    gpu::GpuStatus status = gpu::GpuStatus::system_error;
    HRESULT hresult = E_FAIL;
    std::uint32_t slot_index = 0;
    std::uint32_t reserved = 0;
    std::uint64_t sequence = 0;
    gpu::SharedFrameBusFrameMetadata metadata{};
    gpu::SharedFrameBusFrameSideData side_data{};
    char message[384]{};
};

struct ProducerHandshake final {
    std::uint32_t structure_size = sizeof(ProducerHandshake);
    WireResponse response{};
    gpu::SharedFrameBusRegistration registration{};
};

static_assert(std::is_trivially_copyable_v<WireCommand>);
static_assert(std::is_trivially_copyable_v<WireResponse>);
static_assert(std::is_trivially_copyable_v<ProducerHandshake>);
static_assert(std::is_trivially_copyable_v<gpu::SharedFrameBusRegistration>);
static_assert(std::is_trivially_copyable_v<gpu::SharedFrameBusFrameMetadata>);
static_assert(std::is_trivially_copyable_v<gpu::SharedFrameBusFrameSideData>);
static_assert(std::is_trivially_copyable_v<gpu::SharedFrameBusMoveResult>);
static_assert(gpu::shared_frame_bus_protocol_version == 5);
static_assert(sizeof(gpu::SharedFrameBusRegistration) == 144);
static_assert(offsetof(gpu::SharedFrameBusRegistration, protocol_version) == 4);
static_assert(offsetof(gpu::SharedFrameBusRegistration, consumer_token) == 40);
static_assert(offsetof(gpu::SharedFrameBusRegistration, control_mapping_handle) == 48);
static_assert(offsetof(gpu::SharedFrameBusRegistration, texture_handles) == 80);
static_assert(sizeof(gpu::SharedFrameBusFrameMetadata) == 96);
static_assert(sizeof(gpu::SharedFrameBusMoveResult) == 448);

constexpr std::size_t kBusControlPageV1Size = 384;
constexpr std::size_t kBusControlPageV2Size = 1408;

struct alignas(64) BusSlotSideDataV3Fixture final {
    volatile LONG64 sequence;
    gpu::SharedFrameBusFrameSideData payload;
};

struct alignas(64) BusCursorShapeSlotV3Fixture final {
    volatile LONG64 commit_sequence;
    std::uint64_t shape_sequence;
    std::uint32_t kind;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t hotspot_x;
    std::uint32_t hotspot_y;
    std::uint32_t stride_bytes;
    std::uint32_t data_size;
    std::uint32_t reserved;
    std::array<std::uint8_t,
        gpu::shared_frame_bus_max_cursor_shape_bytes> data;
};

struct alignas(64) BusControlPageV3Fixture final {
    std::array<std::uint8_t, kBusControlPageV2Size> v2;
    std::array<BusSlotSideDataV3Fixture,
        gpu::shared_frame_bus_max_slots> side_data;
    std::array<BusCursorShapeSlotV3Fixture, 2> cursor_shapes;
};

struct alignas(64) BusMoveResultSlotV4Fixture final {
    volatile LONG64 commit_sequence;
    gpu::SharedFrameBusMoveResult payload;
    std::array<std::uint8_t, 56> reserved;
};

struct alignas(64) BusControlPageV4Fixture final {
    BusControlPageV3Fixture v3;
    std::array<BusMoveResultSlotV4Fixture,
        gpu::shared_frame_bus_move_result_slots> move_results;
};

static_assert(sizeof(BusSlotSideDataV3Fixture) % 64 == 0);
static_assert(sizeof(BusCursorShapeSlotV3Fixture) % 64 == 0);
static_assert(offsetof(BusControlPageV3Fixture, side_data)
    == kBusControlPageV2Size);
static_assert(sizeof(BusMoveResultSlotV4Fixture) % 64 == 0);
static_assert(offsetof(BusControlPageV4Fixture, move_results)
    == sizeof(BusControlPageV3Fixture));

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

void require_status(
    const gpu::GpuError& result,
    gpu::GpuStatus expected,
    std::string_view operation) {
    if (result || result.status != expected) {
        fail(std::string(operation) + " returned " + gpu_error_text(result)
            + ", expected status="
            + std::to_string(static_cast<unsigned>(expected)));
    }
}

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
        return std::exchange(value_, nullptr);
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

class ScopedMappedView final {
public:
    explicit ScopedMappedView(void* value = nullptr) noexcept : value_(value) {}
    ~ScopedMappedView() {
        if (value_ != nullptr) UnmapViewOfFile(value_);
    }
    ScopedMappedView(const ScopedMappedView&) = delete;
    ScopedMappedView& operator=(const ScopedMappedView&) = delete;
    [[nodiscard]] void* get() const noexcept { return value_; }
private:
    void* value_ = nullptr;
};

bool read_exact(HANDLE pipe, void* output, std::size_t size) noexcept {
    auto* cursor = static_cast<std::uint8_t*>(output);
    while (size != 0) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            size, std::numeric_limits<DWORD>::max()));
        if (!ReadFile(pipe, cursor, chunk, &read, nullptr) || read == 0) {
            return false;
        }
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
        if (!WriteFile(pipe, cursor, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

HANDLE parse_handle(const wchar_t* text) noexcept {
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (end == text || *end != L'\0'
        || value > std::numeric_limits<std::uintptr_t>::max()) {
        return nullptr;
    }
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
}

std::uint32_t parse_u32(const wchar_t* text, const char* name) {
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0'
        || value > std::numeric_limits<std::uint32_t>::max()) {
        fail(std::string("invalid ") + name);
    }
    return static_cast<std::uint32_t>(value);
}

void copy_message(char (&destination)[384], std::string_view message) noexcept {
    const std::size_t count = std::min(message.size(), sizeof(destination) - 1);
    std::memcpy(destination, message.data(), count);
    destination[count] = '\0';
}

std::string narrow_ascii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) {
            fail("child mode contains a non-ASCII character");
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

WireResponse success_response(WireOperation operation) noexcept {
    WireResponse response;
    response.operation = operation;
    response.success = 1;
    response.status = gpu::GpuStatus::ok;
    response.hresult = S_OK;
    copy_message(response.message, "ok");
    return response;
}

WireResponse error_response(
    WireOperation operation,
    const gpu::GpuError& error) noexcept {
    WireResponse response;
    response.operation = operation;
    response.status = error.status;
    response.hresult = error.hresult;
    copy_message(response.message, error.what());
    return response;
}

WireResponse exception_response(
    WireOperation operation,
    std::string_view message) noexcept {
    WireResponse response;
    response.operation = operation;
    response.status = gpu::GpuStatus::system_error;
    response.hresult = E_FAIL;
    copy_message(response.message, message);
    return response;
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
        if (FAILED(hr)) {
            fail("EnumAdapters1 failed: " + std::to_string(hr));
        }
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
        fail("registration adapter was not found in the child process");
    }
    return create_device(selected.Get());
}

std::uint32_t pattern_pixel(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t frame_id) noexcept {
    return 0xff00'0000u
        | ((frame_id & 0xffu) << 16u)
        | ((y & 0xffu) << 8u)
        | (x & 0xffu);
}

std::vector<std::uint32_t> make_pattern(std::uint32_t frame_id) {
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            pixels[static_cast<std::size_t>(y) * kWidth + x] =
                pattern_pixel(x, y, frame_id);
        }
    }
    return pixels;
}

gpu::SharedFrameBusFrameMetadata frame_metadata(
    std::uint32_t frame_id) noexcept {
    gpu::SharedFrameBusFrameMetadata metadata;
    metadata.valid_fields =
        gpu::shared_frame_bus_metadata_source_timestamp
        | gpu::shared_frame_bus_metadata_qpc
        | gpu::shared_frame_bus_metadata_source_dimensions
        | gpu::shared_frame_bus_metadata_roi
        | gpu::shared_frame_bus_metadata_mailbox_generation
        | gpu::shared_frame_bus_metadata_color_space;
    metadata.source_timestamp_100ns = static_cast<std::int64_t>(frame_id) * 100;
    metadata.timestamp_qpc = static_cast<std::uint64_t>(frame_id) + 10;
    metadata.qpc_frequency = 10'000'000;
    metadata.mailbox_generation = static_cast<std::uint64_t>(frame_id) + 1;
    metadata.source_width = kWidth;
    metadata.source_height = kHeight;
    metadata.roi_width = kWidth;
    metadata.roi_height = kHeight;
    metadata.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    return metadata;
}

bool same_metadata(
    const gpu::SharedFrameBusFrameMetadata& left,
    const gpu::SharedFrameBusFrameMetadata& right) noexcept {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

gpu::SharedFrameBusFrameSideData frame_side_data(
    std::uint32_t frame_id,
    std::uint64_t base_sequence = 0,
    bool include_move = false) noexcept {
    gpu::SharedFrameBusFrameSideData side_data;
    side_data.epoch = static_cast<std::uint64_t>(frame_id) + 100;
    side_data.epoch_nonce = static_cast<std::uint64_t>(frame_id) + 10'000;
    side_data.damage.base_sequence = base_sequence;
    side_data.damage.dirty_count = 2;
    side_data.damage.move_count = include_move ? 1u : 0u;
    side_data.damage.flags = gpu::wgc_damage_valid
        | (include_move ? gpu::wgc_damage_inferred_move : 0u);
    side_data.damage.dirty_rects[0] = {1, 2, 8, 7};
    side_data.damage.dirty_rects[1] = {16, 12, 12, 9};
    if (include_move) {
        side_data.damage.move_rects[0] = {
            4, 5, {20, 18, 6, 5}};
    }
    side_data.cursor.sample_qpc =
        static_cast<std::uint64_t>(frame_id) + 500;
    side_data.cursor.shape_sequence =
        static_cast<std::uint64_t>(frame_id) + 1'000;
    side_data.cursor.screen_x = 100;
    side_data.cursor.screen_y = 200;
    side_data.cursor.frame_x = 12;
    side_data.cursor.frame_y = 14;
    side_data.cursor.width = 8;
    side_data.cursor.height = 6;
    side_data.cursor.hotspot_x = 2;
    side_data.cursor.hotspot_y = 1;
    side_data.cursor.flags = gpu::wgc_cursor_visible
        | gpu::wgc_cursor_position_valid;
    return side_data;
}

gpu::SharedFrameBusFrameSideData native_move_side_data(
    std::uint32_t frame_id,
    std::uint64_t base_sequence) noexcept {
    auto side_data = frame_side_data(frame_id, base_sequence);
    side_data.damage.move_count = 1;
    side_data.damage.flags |= gpu::wgc_damage_native
        | gpu::wgc_damage_native_move_available;
    side_data.damage.move_rects[0] = {4, 5, {20, 18, 6, 5}};
    return side_data;
}

bool same_rect(
    const gpu::WgcRect& left,
    const gpu::WgcRect& right) noexcept {
    return left.x == right.x
        && left.y == right.y
        && left.width == right.width
        && left.height == right.height;
}

bool same_move_rect(
    const gpu::WgcMoveRect& left,
    const gpu::WgcMoveRect& right) noexcept {
    return left.source_x == right.source_x
        && left.source_y == right.source_y
        && same_rect(left.destination, right.destination);
}

gpu::SharedFrameBusMoveResult inferred_move_result(
    std::uint64_t sequence,
    std::uint64_t epoch = 7,
    std::uint64_t epoch_nonce = 11) noexcept {
    gpu::SharedFrameBusMoveResult result;
    result.epoch = epoch;
    result.epoch_nonce = epoch_nonce;
    result.sequence = sequence;
    result.base_sequence = sequence - 1;
    result.move_count = 1;
    result.flags = gpu::shared_frame_bus_move_result_valid
        | gpu::shared_frame_bus_move_result_inferred;
    result.move_rects[0] = {4, 5, {20, 18, 6, 5}};
    return result;
}

bool same_move_result(
    const gpu::SharedFrameBusMoveResult& left,
    const gpu::SharedFrameBusMoveResult& right) noexcept {
    if (left.structure_size != right.structure_size
        || left.result_version != right.result_version
        || left.epoch != right.epoch
        || left.epoch_nonce != right.epoch_nonce
        || left.sequence != right.sequence
        || left.base_sequence != right.base_sequence
        || left.move_count != right.move_count
        || left.flags != right.flags
        || left.reserved != right.reserved) {
        return false;
    }
    for (std::size_t index = 0; index < left.move_rects.size(); ++index) {
        if (!same_move_rect(left.move_rects[index], right.move_rects[index])) {
            return false;
        }
    }
    return true;
}

bool same_side_data(
    const gpu::SharedFrameBusFrameSideData& left,
    const gpu::SharedFrameBusFrameSideData& right) noexcept {
    if (left.structure_size != right.structure_size
        || left.side_data_version != right.side_data_version
        || left.epoch != right.epoch
        || left.epoch_nonce != right.epoch_nonce
        || left.damage.base_sequence != right.damage.base_sequence
        || left.damage.dirty_count != right.damage.dirty_count
        || left.damage.move_count != right.damage.move_count
        || left.damage.flags != right.damage.flags
        || left.damage.reserved != right.damage.reserved
        || left.cursor.sample_qpc != right.cursor.sample_qpc
        || left.cursor.shape_sequence != right.cursor.shape_sequence
        || left.cursor.screen_x != right.cursor.screen_x
        || left.cursor.screen_y != right.cursor.screen_y
        || left.cursor.frame_x != right.cursor.frame_x
        || left.cursor.frame_y != right.cursor.frame_y
        || left.cursor.width != right.cursor.width
        || left.cursor.height != right.cursor.height
        || left.cursor.hotspot_x != right.cursor.hotspot_x
        || left.cursor.hotspot_y != right.cursor.hotspot_y
        || left.cursor.flags != right.cursor.flags
        || left.cursor.reserved != right.cursor.reserved
        || left.reserved != right.reserved) {
        return false;
    }
    for (std::size_t index = 0; index < left.damage.dirty_rects.size(); ++index) {
        if (!same_rect(
                left.damage.dirty_rects[index],
                right.damage.dirty_rects[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.damage.move_rects.size(); ++index) {
        if (!same_move_rect(
                left.damage.move_rects[index],
                right.damage.move_rects[index])) {
            return false;
        }
    }
    return true;
}

bool empty_side_data(
    const gpu::SharedFrameBusFrameSideData& side_data) noexcept {
    return same_side_data(side_data, gpu::SharedFrameBusFrameSideData{});
}

bool same_cursor_shape(
    const gpu::WgcCursorShape& left,
    const gpu::WgcCursorShape& right) noexcept {
    return left.sequence == right.sequence
        && left.kind == right.kind
        && left.width == right.width
        && left.height == right.height
        && left.hotspot_x == right.hotspot_x
        && left.hotspot_y == right.hotspot_y
        && left.stride_bytes == right.stride_bytes
        && left.data == right.data;
}

ComPtr<ID3D11Texture2D> create_source_texture(ID3D11Device* device) {
    const auto pixels = make_pattern(0);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kWidth;
    description.Height = kHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = kWidth * sizeof(std::uint32_t);
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(
        &description, &initial, &texture);
    if (FAILED(hr)) {
        fail("source texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

void write_pattern(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    std::uint32_t frame_id) {
    require(context != nullptr && texture != nullptr, "write_pattern received null input");
    const auto pixels = make_pattern(frame_id);
    context->UpdateSubresource(
        texture,
        0,
        nullptr,
        pixels.data(),
        kWidth * sizeof(std::uint32_t),
        0);
}

void validate_pattern(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    std::uint32_t frame_id) {
    require(device != nullptr && context != nullptr && texture != nullptr,
        "validate_pattern received null input");
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    require(
        description.Width == kWidth
            && description.Height == kHeight
            && description.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
        "shared texture metadata does not match the test pattern");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(hr)) {
        fail("staging texture creation failed: " + std::to_string(hr));
    }
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fail("staging texture Map failed: " + std::to_string(hr));
    }
    bool mismatch = false;
    std::uint32_t mismatch_x = 0;
    std::uint32_t mismatch_y = 0;
    std::uint32_t actual = 0;
    std::uint32_t expected = 0;
    for (std::uint32_t y = 0; y < kHeight && !mismatch; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch);
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            expected = pattern_pixel(x, y, frame_id);
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
        fail("pattern mismatch at (" + std::to_string(mismatch_x) + ','
            + std::to_string(mismatch_y) + "): expected="
            + std::to_string(expected) + ", actual=" + std::to_string(actual));
    }
}

gpu::SharedFrameBusConfig bus_config(
    std::uint32_t slots,
    std::uint32_t consumers) noexcept {
    gpu::SharedFrameBusConfig config;
    config.width = kWidth;
    config.height = kHeight;
    config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
    config.slot_count = slots;
    config.max_consumers = consumers;
    return config;
}

class ChildProcess final {
public:
    ChildProcess() noexcept = default;
    ~ChildProcess() { cleanup(); }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept
        : process_(std::move(other.process_)),
          thread_(std::move(other.thread_)),
          input_(std::move(other.input_)),
          output_(std::move(other.output_)),
          process_id_(std::exchange(other.process_id_, 0)),
          label_(std::move(other.label_)) {}
    ChildProcess& operator=(ChildProcess&& other) noexcept {
        if (this == &other) return *this;
        cleanup();
        process_ = std::move(other.process_);
        thread_ = std::move(other.thread_);
        input_ = std::move(other.input_);
        output_ = std::move(other.output_);
        process_id_ = std::exchange(other.process_id_, 0);
        label_ = std::move(other.label_);
        return *this;
    }

    static ChildProcess spawn(
        std::wstring_view mode,
        const std::vector<std::wstring>& extra_arguments = {}) {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE child_input_raw = nullptr;
        HANDLE parent_output_raw = nullptr;
        if (!CreatePipe(
                &child_input_raw, &parent_output_raw, &security, 0)) {
            fail("CreatePipe for child input failed: "
                + std::to_string(GetLastError()));
        }
        ScopedHandle child_input(child_input_raw);
        ScopedHandle parent_output(parent_output_raw);

        HANDLE parent_input_raw = nullptr;
        HANDLE child_output_raw = nullptr;
        if (!CreatePipe(
                &parent_input_raw, &child_output_raw, &security, 0)) {
            fail("CreatePipe for child output failed: "
                + std::to_string(GetLastError()));
        }
        ScopedHandle parent_input(parent_input_raw);
        ScopedHandle child_output(child_output_raw);
        if (!SetHandleInformation(parent_output.get(), HANDLE_FLAG_INHERIT, 0)
            || !SetHandleInformation(parent_input.get(), HANDLE_FLAG_INHERIT, 0)) {
            fail("SetHandleInformation failed: " + std::to_string(GetLastError()));
        }

        std::vector<wchar_t> executable(32'768);
        const DWORD length = GetModuleFileNameW(
            nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) {
            fail("GetModuleFileNameW failed: " + std::to_string(GetLastError()));
        }
        std::wstring command_line = L"\""
            + std::wstring(executable.data(), length) + L"\" "
            + std::wstring(mode) + L" "
            + std::to_wstring(
                reinterpret_cast<std::uintptr_t>(child_input.get())) + L" "
            + std::to_wstring(
                reinterpret_cast<std::uintptr_t>(child_output.get()));
        for (const std::wstring& argument : extra_arguments) {
            command_line += L" \"" + argument + L"\"";
        }

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
            fail("CreateProcessW for " + narrow_ascii(mode)
                + " failed: " + std::to_string(GetLastError()));
        }

        ChildProcess result;
        result.process_.reset(process.hProcess);
        result.thread_.reset(process.hThread);
        result.input_ = std::move(parent_input);
        result.output_ = std::move(parent_output);
        result.process_id_ = process.dwProcessId;
        result.label_ = narrow_ascii(mode);
        child_input.reset();
        child_output.reset();
        return result;
    }

    [[nodiscard]] HANDLE process() const noexcept { return process_.get(); }
    [[nodiscard]] DWORD process_id() const noexcept { return process_id_; }

    template <class T>
    void send(const T& value, std::string_view context) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!write_exact(output_.get(), &value, sizeof(value))) {
            fail(label_ + " failed to receive " + std::string(context)
                + ": pipe error=" + std::to_string(GetLastError()));
        }
    }

    template <class T>
    T receive(std::uint32_t timeout_ms, std::string_view context) {
        static_assert(std::is_trivially_copyable_v<T>);
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(
                    input_.get(), nullptr, 0, nullptr, &available, nullptr)) {
                fail(child_failure(context, "response pipe closed"));
            }
            if (available >= sizeof(T)) {
                T value{};
                if (!read_exact(input_.get(), &value, sizeof(value))) {
                    fail(child_failure(context, "response read failed"));
                }
                return value;
            }
            const DWORD process_wait = WaitForSingleObject(process_.get(), 0);
            if (process_wait == WAIT_OBJECT_0) {
                fail(child_failure(context, "process exited before its response"));
            }
            if (process_wait == WAIT_FAILED) {
                fail(child_failure(context, "process wait failed"));
            }
            if (GetTickCount64() >= deadline) {
                fail(child_failure(context, "timed out after "
                    + std::to_string(timeout_ms) + " ms"));
            }
            Sleep(1);
        }
    }

    void wait(std::uint32_t timeout_ms, std::string_view context) {
        const DWORD waited = WaitForSingleObject(process_.get(), timeout_ms);
        if (waited == WAIT_TIMEOUT) {
            fail(child_failure(context, "did not exit within "
                + std::to_string(timeout_ms) + " ms"));
        }
        if (waited != WAIT_OBJECT_0) {
            fail(child_failure(context, "process wait failed"));
        }
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_.get(), &exit_code) || exit_code != 0) {
            fail(child_failure(context, "unexpected exit code "
                + std::to_string(exit_code)));
        }
    }

    void terminate(UINT exit_code) noexcept {
        if (process_.get() == nullptr) return;
        if (WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process_.get(), exit_code);
            (void)WaitForSingleObject(process_.get(), 2'000);
        }
    }

private:
    std::string child_failure(
        std::string_view context,
        std::string detail) const {
        DWORD exit_code = STILL_ACTIVE;
        if (process_.get() != nullptr) {
            (void)GetExitCodeProcess(process_.get(), &exit_code);
        }
        return label_ + " (pid=" + std::to_string(process_id_)
            + ") " + std::string(context) + ": " + std::move(detail)
            + ", exit=" + std::to_string(exit_code)
            + ", win32=" + std::to_string(GetLastError());
    }

    void cleanup() noexcept {
        output_.reset();
        if (process_.get() != nullptr
            && WaitForSingleObject(process_.get(), 250) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process_.get(), 0xfcb5u);
            (void)WaitForSingleObject(process_.get(), 2'000);
        }
        input_.reset();
        thread_.reset();
        process_.reset();
        process_id_ = 0;
    }

    ScopedHandle process_;
    ScopedHandle thread_;
    ScopedHandle input_;
    ScopedHandle output_;
    DWORD process_id_ = 0;
    std::string label_;
};

void require_wire_success(
    const WireResponse& response,
    WireOperation expected,
    std::string_view context) {
    if (response.structure_size != sizeof(WireResponse)
        || response.operation != expected
        || response.success == 0
        || response.status != gpu::GpuStatus::ok) {
        fail(std::string(context) + " failed in child: " + response.message
            + " (status="
            + std::to_string(static_cast<unsigned>(response.status))
            + ", hr=" + std::to_string(static_cast<long long>(response.hresult))
            + ')');
    }
}

WireResponse command_child(
    ChildProcess& child,
    WireOperation operation,
    std::uint32_t frame_id = 0,
    std::uint32_t timeout_ms = kOperationTimeoutMs) {
    WireCommand command;
    command.operation = operation;
    command.timeout_ms = timeout_ms;
    command.frame_id = frame_id;
    child.send(command, "command");
    WireResponse response = child.receive<WireResponse>(
        kChildTimeoutMs, "command response");
    require(response.structure_size == sizeof(WireResponse),
        "child returned a malformed response");
    require(response.operation == operation,
        "child response operation did not match its command");
    return response;
}

gpu::SharedFrameBusRegistration attach_consumer(
    gpu::SharedFrameBusPublisher& publisher,
    ChildProcess& child) {
    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(child.process(), registration),
        "register cross-process consumer");
    child.send(registration, "consumer registration");
    const WireResponse opened = child.receive<WireResponse>(
        kChildTimeoutMs, "consumer open response");
    require_wire_success(
        opened, WireOperation::open_consumer, "open cross-process consumer");
    require(opened.slot_index == registration.consumer_index,
        "child opened a different consumer index");
    return registration;
}

void stop_child(ChildProcess& child) {
    const WireResponse exited = command_child(
        child, WireOperation::exit_process, 0, 0);
    require_wire_success(exited, WireOperation::exit_process, "exit child");
    child.wait(kOperationTimeoutMs, "exit child");
}

void close_registered_consumer(
    gpu::SharedFrameBusPublisher& publisher,
    const gpu::SharedFrameBusRegistration& registration,
    ChildProcess& child) {
    const WireResponse closed = command_child(
        child, WireOperation::close, 0, 0);
    require_wire_success(closed, WireOperation::close, "close consumer");
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister closed consumer");
    stop_child(child);
}

std::uint64_t acquire_and_validate(
    ChildProcess& child,
    std::uint32_t frame_id,
    const gpu::SharedFrameBusFrameMetadata* expected_metadata = nullptr,
    const gpu::SharedFrameBusFrameSideData* expected_side_data = nullptr) {
    const WireResponse acquired = command_child(
        child, WireOperation::acquire_validate, frame_id);
    require_wire_success(
        acquired, WireOperation::acquire_validate, "acquire and validate frame");
    require(acquired.sequence != 0, "child acquired sequence zero");
    if (expected_metadata != nullptr) {
        require(same_metadata(acquired.metadata, *expected_metadata),
            "child acquired metadata that does not match its texture marker");
    }
    if (expected_side_data != nullptr) {
        require(same_side_data(acquired.side_data, *expected_side_data),
            "child acquired side data that does not match its texture marker");
    }
    return acquired.sequence;
}

void release_child_lease(ChildProcess& child) {
    const WireResponse released = command_child(
        child, WireOperation::release, 0, 0);
    require_wire_success(released, WireOperation::release, "release child frame");
}

int run_consumer_child(HANDLE input, HANDLE output) noexcept {
    gpu::SharedFrameBusFrameLease held;
    try {
        gpu::SharedFrameBusRegistration registration;
        if (!read_exact(input, &registration, sizeof(registration))) {
            fail("consumer child did not receive a registration");
        }
        DeviceContext d3d = create_device_for_registration(registration);
        gpu::SharedFrameBusConsumer consumer;
        const gpu::GpuError opened = gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), registration, true, consumer);
        WireResponse open_response = opened
            ? success_response(WireOperation::open_consumer)
            : error_response(WireOperation::open_consumer, opened);
        open_response.slot_index = registration.consumer_index;
        if (!write_exact(output, &open_response, sizeof(open_response))) return 3;
        if (!opened) return 4;

        for (;;) {
            WireCommand command;
            if (!read_exact(input, &command, sizeof(command))) return 0;
            if (command.structure_size != sizeof(WireCommand)) {
                const WireResponse malformed = exception_response(
                    command.operation, "malformed child command");
                (void)write_exact(output, &malformed, sizeof(malformed));
                return 5;
            }

            WireResponse response;
            try {
                switch (command.operation) {
                case WireOperation::acquire_validate: {
                    gpu::SharedFrameBusFrameLease next;
                    const gpu::GpuError acquired = consumer.acquire_latest(
                        command.timeout_ms, next);
                    if (!acquired) {
                        response = error_response(command.operation, acquired);
                        break;
                    }
                    require(next.info().width == kWidth
                            && next.info().height == kHeight
                            && next.info().format
                                == DXGI_FORMAT_B8G8R8A8_UNORM,
                        "acquired frame metadata is inconsistent");
                    validate_pattern(
                        d3d.device.Get(), d3d.context.Get(), next.texture(),
                        command.frame_id);
                    held = std::move(next);
                    response = success_response(command.operation);
                    response.slot_index = held.info().slot_index;
                    response.sequence = held.info().sequence;
                    response.metadata = held.metadata();
                    response.side_data = held.side_data();
                    break;
                }
                case WireOperation::validate_held:
                    require(static_cast<bool>(held), "child has no held frame");
                    validate_pattern(
                        d3d.device.Get(), d3d.context.Get(), held.texture(),
                        command.frame_id);
                    response = success_response(command.operation);
                    response.slot_index = held.info().slot_index;
                    response.sequence = held.info().sequence;
                    response.metadata = held.metadata();
                    response.side_data = held.side_data();
                    break;
                case WireOperation::release: {
                    const gpu::GpuError released = consumer.release(held);
                    response = released
                        ? success_response(command.operation)
                        : error_response(command.operation, released);
                    break;
                }
                case WireOperation::close: {
                    const gpu::GpuError closed = consumer.close();
                    response = closed
                        ? success_response(command.operation)
                        : error_response(command.operation, closed);
                    break;
                }
                case WireOperation::exit_process:
                    response = success_response(command.operation);
                    if (!write_exact(output, &response, sizeof(response))) return 6;
                    return 0;
                default:
                    response = exception_response(
                        command.operation, "unsupported consumer command");
                    break;
                }
            } catch (const std::exception& error) {
                response = exception_response(command.operation, error.what());
            }
            if (!write_exact(output, &response, sizeof(response))) return 7;
        }
    } catch (const std::exception& error) {
        const WireResponse response = exception_response(
            WireOperation::open_consumer, error.what());
        (void)write_exact(output, &response, sizeof(response));
        return 2;
    }
}

int run_producer_child(
    HANDLE input,
    HANDLE output,
    DWORD target_process_id) noexcept {
    ProducerHandshake handshake;
    handshake.response.operation = WireOperation::producer_ready;
    try {
        DeviceContext d3d = create_device();
        gpu::SharedFrameBusPublisher publisher;
        require_gpu(
            gpu::SharedFrameBusPublisher::create(
                d3d.device.Get(), bus_config(3, 1), publisher),
            "create producer-child bus");
        ScopedHandle target(OpenProcess(
            PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE,
            target_process_id));
        if (target.get() == nullptr) {
            fail("producer child OpenProcess failed: "
                + std::to_string(GetLastError()));
        }
        require_gpu(
            publisher.register_consumer(target.get(), handshake.registration),
            "producer child register target");
        handshake.response = success_response(WireOperation::producer_ready);
        if (!write_exact(output, &handshake, sizeof(handshake))) return 3;

        for (;;) {
            WireCommand command;
            if (!read_exact(input, &command, sizeof(command))) return 0;
            WireResponse response;
            if (command.structure_size != sizeof(command)
                || command.operation != WireOperation::producer_exit) {
                response = exception_response(
                    command.operation, "unsupported producer command");
            } else {
                response = success_response(WireOperation::producer_exit);
                if (!write_exact(output, &response, sizeof(response))) return 4;
                return 0;
            }
            if (!write_exact(output, &response, sizeof(response))) return 5;
        }
    } catch (const std::exception& error) {
        handshake.response = exception_response(
            WireOperation::producer_ready, error.what());
        (void)write_exact(output, &handshake, sizeof(handshake));
        return 2;
    }
}

int run_death_scenario(
    HANDLE output,
    std::uint32_t acquire_timeout_ms,
    ProducerExitMode mode) noexcept {
    try {
        ChildProcess producer = ChildProcess::spawn(
            L"--producer-child", {std::to_wstring(GetCurrentProcessId())});
        const ProducerHandshake handshake = producer.receive<ProducerHandshake>(
            kChildTimeoutMs, "producer handshake");
        require(handshake.structure_size == sizeof(ProducerHandshake),
            "producer returned malformed handshake");
        require_wire_success(
            handshake.response,
            WireOperation::producer_ready,
            "start producer child");

        DeviceContext d3d = create_device_for_registration(
            handshake.registration);
        gpu::SharedFrameBusConsumer consumer;
        require_gpu(
            gpu::SharedFrameBusConsumer::open(
                d3d.device.Get(), handshake.registration, true, consumer),
            "open producer-death consumer");

        gpu::SharedFrameBusFrameLease lease;
        gpu::GpuError acquired;
        Clock::time_point begin{};
        Clock::time_point end{};
        std::atomic<bool> acquire_entered{false};
        std::thread waiter([&] {
            begin = Clock::now();
            acquire_entered.store(true, std::memory_order_release);
            acquired = consumer.acquire_latest(acquire_timeout_ms, lease);
            end = Clock::now();
        });
        while (!acquire_entered.load(std::memory_order_acquire)) {
            SwitchToThread();
        }
        Sleep(50);

        std::exception_ptr exit_failure;
        try {
            if (mode == ProducerExitMode::graceful) {
                const WireResponse response = command_child(
                    producer, WireOperation::producer_exit, 0, 0);
                require_wire_success(
                    response, WireOperation::producer_exit,
                    "graceful producer exit");
                producer.wait(kOperationTimeoutMs, "graceful producer exit");
            } else {
                producer.terminate(77);
                require(
                    WaitForSingleObject(producer.process(), kOperationTimeoutMs)
                        == WAIT_OBJECT_0,
                    "abrupt producer child did not terminate");
            }
        } catch (...) {
            exit_failure = std::current_exception();
            producer.terminate(78);
        }
        waiter.join();
        if (exit_failure != nullptr) std::rethrow_exception(exit_failure);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - begin);
        require(!acquired, "consumer acquired a frame after producer exit");
        require(acquired.status != gpu::GpuStatus::timeout,
            "producer exit was reported as an ordinary frame timeout");
        require(elapsed.count() < 3'000,
            "consumer took " + std::to_string(elapsed.count())
                + " ms to observe producer exit");
        require(!lease, "failed producer-exit acquire returned a lease");
        require_gpu(consumer.close(), "close producer-death consumer");

        WireResponse response = success_response(WireOperation::death_scenario);
        response.reserved = static_cast<std::uint32_t>(acquired.status);
        response.hresult = acquired.hresult;
        response.sequence = static_cast<std::uint64_t>(elapsed.count());
        copy_message(response.message, acquired.what());
        return write_exact(output, &response, sizeof(response)) ? 0 : 3;
    } catch (const std::exception& error) {
        const WireResponse response = exception_response(
            WireOperation::death_scenario, error.what());
        (void)write_exact(output, &response, sizeof(response));
        return 2;
    }
}

void test_copy_direct_and_latest_sequence() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(4, 1), publisher),
        "create copy/direct bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    ChildProcess child = ChildProcess::spawn(L"--consumer-child");
    const auto registration = attach_consumer(publisher, child);

    write_pattern(d3d.context.Get(), source.Get(), 1);
    const auto copy_metadata = frame_metadata(1);
    require_gpu(
        publisher.publish(source.Get(), copy_metadata, kOperationTimeoutMs),
        "copy publish with metadata");
    require(acquire_and_validate(child, 1, &copy_metadata) == 1,
        "first copy publish did not produce sequence 1");
    release_child_lease(child);

    gpu::SharedFrameBusWriteLease direct;
    require_gpu(
        publisher.begin_publish(kOperationTimeoutMs, direct),
        "begin direct publish");
    require(static_cast<bool>(direct), "direct publish returned an empty lease");
    require(direct.slot_index() < publisher.config().slot_count,
        "direct publish returned an invalid slot index");
    write_pattern(d3d.context.Get(), direct.texture(), 2);
    const auto direct_metadata = frame_metadata(2);
    require_gpu(
        publisher.commit(std::move(direct), direct_metadata),
        "commit direct publish with metadata");
    require(!direct, "commit did not consume the direct write lease");
    require(acquire_and_validate(child, 2) == 2,
        "direct publish did not produce sequence 2");
    release_child_lease(child);

    std::uint64_t previous = 2;
    for (std::uint32_t frame_id = 3; frame_id <= 40; ++frame_id) {
        write_pattern(d3d.context.Get(), source.Get(), frame_id);
        const auto metadata = frame_metadata(frame_id);
        if (frame_id == 3) {
            require_gpu(
                publisher.publish(source.Get(), kOperationTimeoutMs),
                "legacy publish after metadata reuse");
        } else {
            require_gpu(
                publisher.publish(source.Get(), metadata, kOperationTimeoutMs),
                "continuous metadata copy publish");
        }
        const std::uint64_t sequence = acquire_and_validate(
            child, frame_id, frame_id == 3 ? nullptr : &metadata);
        require(sequence == previous + 1,
            "consumer sequence was not continuous");
        previous = sequence;
        if (frame_id == 3) {
            const WireResponse held = command_child(
                child, WireOperation::validate_held, frame_id);
            require_wire_success(
                held, WireOperation::validate_held, "validate legacy metadata clear");
            require(held.metadata.valid_fields == 0,
                "legacy publish inherited metadata from a reused slot");
        }
        release_child_lease(child);
    }

    for (std::uint32_t frame_id = 41; frame_id <= 80; ++frame_id) {
        write_pattern(d3d.context.Get(), source.Get(), frame_id);
        const auto metadata = frame_metadata(frame_id);
        require_gpu(
            publisher.publish(source.Get(), metadata, kOperationTimeoutMs),
            "latest-wins burst publish");
    }
    const auto newest_metadata = frame_metadata(80);
    require(acquire_and_validate(child, 80, &newest_metadata) == 80,
        "latest acquire did not select the newest burst sequence");
    release_child_lease(child);

    const gpu::SharedFrameBusStats stats = publisher.stats();
    require(publisher.sequence() == 80
            && stats.publish_attempts == 80
            && stats.published_frames == 80
            && stats.copied_publishes == 79
            && stats.direct_publishes == 1,
        "copy/direct publish statistics are inconsistent");
    close_registered_consumer(publisher, registration, child);
}

void test_v5_side_data_round_trip_validation_and_legacy_clear() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(2, 1), publisher),
        "create protocol v5 side-data bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    ChildProcess child = ChildProcess::spawn(L"--consumer-child");
    const auto registration = attach_consumer(publisher, child);
    require(registration.protocol_version == 5,
        "side-data consumer did not register protocol v5");

    const auto copy_metadata = frame_metadata(201);
    const auto copy_side_data = frame_side_data(201);
    write_pattern(d3d.context.Get(), source.Get(), 201);
    require_gpu(
        publisher.publish(
            source.Get(), copy_metadata, copy_side_data, kOperationTimeoutMs),
        "copy publish protocol v3 side data");
    require(acquire_and_validate(
            child, 201, &copy_metadata, &copy_side_data) == 1,
        "copy-published side data did not round-trip cross-process");
    release_child_lease(child);

    gpu::SharedFrameBusWriteLease direct;
    require_gpu(
        publisher.begin_publish(kOperationTimeoutMs, direct),
        "begin direct protocol v3 side-data publish");
    write_pattern(d3d.context.Get(), direct.texture(), 202);
    const auto direct_metadata = frame_metadata(202);
    const auto direct_side_data = frame_side_data(202, 1, true);
    require_gpu(
        publisher.commit(
            std::move(direct), direct_metadata, direct_side_data),
        "commit direct protocol v3 side data");
    require(acquire_and_validate(
            child, 202, &direct_metadata, &direct_side_data) == 2,
        "direct-published side data did not round-trip cross-process");
    release_child_lease(child);

    const auto native_metadata = frame_metadata(203);
    const auto native_side_data = native_move_side_data(203, 2);
    write_pattern(d3d.context.Get(), source.Get(), 203);
    require_gpu(
        publisher.publish(
            source.Get(), native_metadata, native_side_data,
            kOperationTimeoutMs),
        "publish protocol v5 native-move side data");
    require(acquire_and_validate(
            child, 203, &native_metadata, &native_side_data) == 3,
        "native-move side data did not round-trip cross-process");
    release_child_lease(child);

    const auto legacy_metadata = frame_metadata(204);
    const gpu::SharedFrameBusFrameSideData no_side_data;
    write_pattern(d3d.context.Get(), source.Get(), 204);
    require_gpu(
        publisher.publish(
            source.Get(), legacy_metadata, kOperationTimeoutMs),
        "publish without side data into a reused v3 slot");
    require(acquire_and_validate(
            child, 204, &legacy_metadata, &no_side_data) == 4,
        "publish without side data inherited a reused slot sidecar");
    release_child_lease(child);

    const auto valid = frame_side_data(205, 4, true);
    std::vector<gpu::SharedFrameBusFrameSideData> invalid;
    auto candidate = valid;
    --candidate.structure_size;
    invalid.push_back(candidate);
    candidate = valid;
    ++candidate.side_data_version;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.reserved[0] = 1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.epoch_nonce = 0;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.epoch = 0;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.reserved = 1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.dirty_count = gpu::wgc_max_dirty_rects + 1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.move_count = gpu::wgc_max_move_rects + 1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.flags |= 1u << 31;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.flags |= gpu::wgc_damage_native
        | gpu::wgc_damage_native_move_available;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.flags &= ~gpu::wgc_damage_inferred_move;
    candidate.damage.flags |= gpu::wgc_damage_native_move_available;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.flags &= ~gpu::wgc_damage_inferred_move;
    candidate.damage.flags |= gpu::wgc_damage_native
        | gpu::wgc_damage_native_move_available
        | gpu::wgc_damage_native_move_unavailable;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.flags &= ~gpu::wgc_damage_inferred_move;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.move_count = 0;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.base_sequence = 0;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.base_sequence = 5;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.dirty_rects[0].x = -1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.dirty_rects[0].width = 0;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.dirty_rects[0] = {
        static_cast<std::int32_t>(kWidth - 1), 0, 2, 1};
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.move_rects[0].source_x = -1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.move_rects[0].source_x =
        static_cast<std::int32_t>(kWidth - 2);
    candidate.damage.move_rects[0].destination.width = 4;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.damage.move_rects[0].destination = {
        static_cast<std::int32_t>(kWidth), 0, 1, 1};
    invalid.push_back(candidate);
    candidate = valid;
    candidate.cursor.flags |= 1u << 31;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.cursor.reserved = 1;
    invalid.push_back(candidate);

    const auto stats_before_invalid = publisher.stats();
    for (const auto& side_data : invalid) {
        require_status(
            publisher.publish(
                source.Get(), frame_metadata(205), side_data, 0),
            gpu::GpuStatus::invalid_argument,
            "reject malformed protocol v3 side data");
    }
    require(publisher.sequence() == 4
            && publisher.stats().published_frames
                == stats_before_invalid.published_frames,
        "malformed side data changed the published sequence or frame count");

    gpu::SharedFrameBusWriteLease invalid_direct;
    require_gpu(
        publisher.begin_publish(0, invalid_direct),
        "begin malformed direct side-data publish");
    write_pattern(d3d.context.Get(), invalid_direct.texture(), 205);
    auto invalid_direct_base = valid;
    invalid_direct_base.damage.base_sequence = invalid_direct.sequence();
    require_status(
        publisher.commit(
            std::move(invalid_direct), frame_metadata(205),
            invalid_direct_base),
        gpu::GpuStatus::invalid_argument,
        "reject invalid direct side-data base sequence");
    require(!invalid_direct && publisher.sequence() == 4,
        "malformed direct side-data commit retained its lease or advanced sequence");
    gpu::SharedFrameBusWriteLease reclaimed;
    require_gpu(publisher.begin_publish(0, reclaimed),
        "reclaim slot after malformed direct side data");
    reclaimed.reset();

    close_registered_consumer(publisher, registration, child);
}

void test_metadata_validation_legacy_clear_and_v1_v2_open() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(2, 1), publisher),
        "create metadata protocol bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    const auto valid = frame_metadata(90);

    std::vector<gpu::SharedFrameBusFrameMetadata> invalid;
    auto candidate = valid;
    --candidate.structure_size;
    invalid.push_back(candidate);
    candidate = valid;
    ++candidate.metadata_version;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.valid_fields |= 1ull << 63;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.reserved_0 = 1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.reserved[1] = 1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.timestamp_qpc = 0;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.source_width = 0;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.roi_x = 1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.mailbox_generation = 0;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.color_space = DXGI_COLOR_SPACE_CUSTOM;
    invalid.push_back(candidate);
    candidate = valid;
    candidate.color_space = DXGI_COLOR_SPACE_RESERVED;
    invalid.push_back(candidate);

    for (const auto& metadata : invalid) {
        require_status(
            publisher.publish(source.Get(), metadata, 0),
            gpu::GpuStatus::invalid_argument,
            "reject invalid copy-publish metadata");
    }
    auto stats = publisher.stats();
    require(publisher.sequence() == 0
            && stats.publish_attempts == 0
            && stats.published_frames == 0,
        "invalid copy metadata changed sequence or statistics");

    gpu::SharedFrameBusWriteLease invalid_direct;
    require_gpu(
        publisher.begin_publish(0, invalid_direct),
        "begin invalid metadata direct publish");
    write_pattern(d3d.context.Get(), invalid_direct.texture(), 90);
    require_status(
        publisher.commit(std::move(invalid_direct), invalid.front()),
        gpu::GpuStatus::invalid_argument,
        "reject invalid direct-commit metadata");
    require(!invalid_direct, "invalid direct commit did not consume its lease");
    stats = publisher.stats();
    require(publisher.sequence() == 0
            && stats.publish_attempts == 1
            && stats.published_frames == 0,
        "invalid direct metadata advanced publication state");
    gpu::SharedFrameBusWriteLease reclaimed;
    require_gpu(publisher.begin_publish(0, reclaimed),
        "claim slot after invalid direct metadata");
    reclaimed.reset();

    write_pattern(d3d.context.Get(), source.Get(), 90);
    require_gpu(
        publisher.publish(source.Get(), valid, kOperationTimeoutMs),
        "publish validated metadata");
    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), registration),
        "register metadata protocol consumer");
    require(registration.protocol_version == gpu::shared_frame_bus_protocol_version,
        "publisher did not register the current bus protocol");
    gpu::SharedFrameBusConsumer consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), registration, false, consumer),
        "open metadata protocol consumer");
    gpu::SharedFrameBusFrameLease lease;
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire validated metadata");
    require(same_metadata(lease.metadata(), valid),
        "validated metadata did not round-trip through a lease");
    require_gpu(consumer.release(lease), "release validated metadata frame");
    require(lease.metadata().valid_fields == 0,
        "released frame lease retained metadata");

    auto scaled_source_metadata = frame_metadata(90);
    scaled_source_metadata.source_width = kWidth * 2;
    scaled_source_metadata.source_height = kHeight * 2;
    scaled_source_metadata.roi_x = 3;
    scaled_source_metadata.roi_y = 5;
    scaled_source_metadata.roi_width = kWidth + 7;
    scaled_source_metadata.roi_height = kHeight + 9;
    require_gpu(
        publisher.publish(
            source.Get(), scaled_source_metadata, kOperationTimeoutMs),
        "publish protocol v5 scaled-source ROI metadata");
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire protocol v5 scaled-source ROI metadata");
    require(same_metadata(lease.metadata(), scaled_source_metadata),
        "protocol v5 scaled-source ROI metadata did not round-trip");
    require_gpu(consumer.release(lease),
        "release protocol v5 scaled-source ROI metadata");

    const auto second_metadata = frame_metadata(91);
    write_pattern(d3d.context.Get(), source.Get(), 91);
    require_gpu(
        publisher.publish(source.Get(), second_metadata, kOperationTimeoutMs),
        "fill second slot with metadata");
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire second metadata frame");
    require(same_metadata(lease.metadata(), second_metadata),
        "second metadata frame did not round-trip through a lease");
    require_gpu(consumer.release(lease), "release second metadata frame");

    write_pattern(d3d.context.Get(), source.Get(), 92);
    require_gpu(publisher.publish(source.Get(), kOperationTimeoutMs),
        "legacy copy publish into a metadata-populated slot");
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire legacy copy publish after metadata slot reuse");
    require(lease.metadata().valid_fields == 0,
        "legacy copy publish inherited stale slot metadata");
    require_gpu(consumer.release(lease), "release legacy copy frame");

    gpu::SharedFrameBusWriteLease legacy_direct;
    require_gpu(publisher.begin_publish(kOperationTimeoutMs, legacy_direct),
        "begin legacy direct publish into a metadata-populated slot");
    write_pattern(d3d.context.Get(), legacy_direct.texture(), 93);
    require_gpu(publisher.commit(std::move(legacy_direct)),
        "commit legacy direct publish into a metadata-populated slot");
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire legacy direct publish after metadata slot reuse");
    require(lease.metadata().valid_fields == 0,
        "legacy direct publish inherited stale slot metadata");
    require_gpu(consumer.release(lease), "release legacy direct frame");
    require_gpu(consumer.close(), "close metadata protocol consumer");
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister metadata protocol consumer");

    struct ControlHeader final {
        std::uint32_t magic;
        std::uint32_t protocol_version;
        std::uint32_t structure_size;
    };
    const auto compatibility_metadata = frame_metadata(94);
    const auto compatibility_side_data = frame_side_data(94, 4, true);
    write_pattern(d3d.context.Get(), source.Get(), 94);
    require_gpu(
        publisher.publish(
            source.Get(), compatibility_metadata,
            compatibility_side_data, kOperationTimeoutMs),
        "publish v1/v2 compatibility fixture frame");

    gpu::SharedFrameBusRegistration v3_registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), v3_registration),
        "register v2 compatibility fixture consumer");
    ScopedMappedView v2_mapped(MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(
            v3_registration.control_mapping_handle)),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(BusControlPageV4Fixture)));
    require(v2_mapped.get() != nullptr,
        "map v2 compatibility control fixture");
    auto* header = static_cast<ControlHeader*>(v2_mapped.get());
    require(header->protocol_version == gpu::shared_frame_bus_protocol_version
            && header->structure_size == sizeof(BusControlPageV4Fixture),
        "publisher did not create the expected protocol v5 control page");

    gpu::SharedFrameBusConsumer rejected;
    auto rejected_registration = v3_registration;
    rejected_registration.protocol_version = 2;
    require_status(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), rejected_registration, false, rejected),
        gpu::GpuStatus::invalid_argument,
        "reject registration/page protocol mismatch");
    rejected_registration = v3_registration;
    rejected_registration.protocol_version = 99;
    require_status(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), rejected_registration, false, rejected),
        gpu::GpuStatus::invalid_argument,
        "reject unknown shared frame bus protocol");
    header->structure_size = static_cast<std::uint32_t>(kBusControlPageV2Size);
    require_status(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), v3_registration, false, rejected),
        gpu::GpuStatus::invalid_argument,
        "reject protocol v3 page with v2 size");

    header->protocol_version = 2;
    header->structure_size = static_cast<std::uint32_t>(
        sizeof(BusControlPageV4Fixture));
    rejected_registration = v3_registration;
    rejected_registration.protocol_version = 2;
    require_status(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), rejected_registration, false, rejected),
        gpu::GpuStatus::invalid_argument,
        "reject protocol v2 page with v3 size");
    header->structure_size = static_cast<std::uint32_t>(kBusControlPageV2Size);

    gpu::SharedFrameBusConsumer v2_consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), rejected_registration, true, v2_consumer),
        "open protocol v2 compatibility fixture");
    gpu::SharedFrameBusFrameLease v2_lease;
    require_gpu(v2_consumer.acquire_latest(kOperationTimeoutMs, v2_lease),
        "acquire protocol v2 fixture frame");
    require(same_metadata(v2_lease.metadata(), compatibility_metadata),
        "protocol v2 consumer lost its frame metadata");
    require(empty_side_data(v2_lease.side_data()),
        "protocol v2 consumer exposed protocol v3 side data");
    require_gpu(v2_consumer.release(v2_lease), "release protocol v2 frame");
    require_gpu(v2_consumer.close(), "close protocol v2 consumer");
    header->protocol_version = gpu::shared_frame_bus_protocol_version;
    header->structure_size = static_cast<std::uint32_t>(
        sizeof(BusControlPageV4Fixture));
    require_gpu(
        publisher.unregister_consumer(v3_registration, kOperationTimeoutMs),
        "unregister protocol v2 fixture consumer");

    gpu::SharedFrameBusRegistration v1_registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), v1_registration),
        "register v1 compatibility fixture consumer");
    ScopedMappedView v1_mapped(MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(
            v1_registration.control_mapping_handle)),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(BusControlPageV4Fixture)));
    require(v1_mapped.get() != nullptr,
        "map v1 compatibility control fixture");
    header = static_cast<ControlHeader*>(v1_mapped.get());
    header->protocol_version = 1;
    header->structure_size = static_cast<std::uint32_t>(kBusControlPageV2Size);
    rejected_registration = v1_registration;
    rejected_registration.protocol_version = 1;
    require_status(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), rejected_registration, false, rejected),
        gpu::GpuStatus::invalid_argument,
        "reject protocol v1 page with v2 size");
    header->structure_size = static_cast<std::uint32_t>(kBusControlPageV1Size);

    gpu::SharedFrameBusConsumer v1_consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), rejected_registration, true, v1_consumer),
        "open protocol v1 compatibility fixture");
    gpu::SharedFrameBusFrameLease v1_lease;
    require_gpu(v1_consumer.acquire_latest(kOperationTimeoutMs, v1_lease),
        "acquire protocol v1 fixture frame");
    require(v1_lease.metadata().valid_fields == 0,
        "protocol v1 consumer exposed synthetic metadata");
    require(empty_side_data(v1_lease.side_data()),
        "protocol v1 consumer exposed protocol v3 side data");
    require_gpu(v1_consumer.release(v1_lease), "release protocol v1 frame");
    require_gpu(v1_consumer.close(), "close protocol v1 consumer");
    header->protocol_version = gpu::shared_frame_bus_protocol_version;
    header->structure_size = static_cast<std::uint32_t>(
        sizeof(BusControlPageV4Fixture));
    require_gpu(
        publisher.unregister_consumer(v1_registration, kOperationTimeoutMs),
        "unregister protocol v1 fixture consumer");
}

void test_bus_color_contract_validation() {
    DeviceContext d3d = create_device();

    auto invalid_config = bus_config(2, 1);
    invalid_config.color_space = DXGI_COLOR_SPACE_RESERVED;
    gpu::SharedFrameBusPublisher invalid_publisher;
    require_status(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), invalid_config, invalid_publisher),
        gpu::GpuStatus::invalid_argument,
        "reject reserved bus color space");
    require(!invalid_publisher.initialized(),
        "rejected bus color space initialized a publisher");

    auto derived_config = bus_config(2, 1);
    derived_config.color_space = DXGI_COLOR_SPACE_CUSTOM;
    gpu::SharedFrameBusPublisher derived_publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), derived_config, derived_publisher),
        "create format-derived color bus");
    require(derived_publisher.config().color_space == DXGI_COLOR_SPACE_CUSTOM,
        "format-derived bus did not preserve its color selection");

    auto contracted_config = bus_config(2, 1);
    contracted_config.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), contracted_config, publisher),
        "create explicit-color bus");
    require(publisher.config().color_space == contracted_config.color_space,
        "explicit bus color contract was not retained");

    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    write_pattern(d3d.context.Get(), source.Get(), 91);
    const auto matching = frame_metadata(91);
    require_gpu(
        publisher.publish(source.Get(), matching, kOperationTimeoutMs),
        "publish metadata matching the bus color contract");

    auto mismatched = matching;
    mismatched.color_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    require_status(
        publisher.publish(source.Get(), mismatched, 0),
        gpu::GpuStatus::invalid_argument,
        "reject metadata that conflicts with the bus color contract");

    auto missing = matching;
    missing.valid_fields &= ~gpu::shared_frame_bus_metadata_color_space;
    missing.color_space = DXGI_COLOR_SPACE_CUSTOM;
    require_gpu(
        publisher.publish(source.Get(), missing, kOperationTimeoutMs),
        "publish metadata without an optional color field");

    gpu::SharedFrameBusWriteLease direct;
    require_gpu(
        publisher.begin_publish(kOperationTimeoutMs, direct),
        "begin direct publish with conflicting color metadata");
    write_pattern(d3d.context.Get(), direct.texture(), 92);
    require_status(
        publisher.commit(std::move(direct), mismatched),
        gpu::GpuStatus::invalid_argument,
        "reject direct metadata that conflicts with the bus color contract");
    require(!direct,
        "rejected direct color metadata retained its write lease");

    const auto stats = publisher.stats();
    require(publisher.sequence() == 2
            && stats.publish_attempts == 3
            && stats.published_frames == 2,
        "rejected color metadata changed bus publication state");

    gpu::SharedFrameBusWriteLease reclaimed;
    require_gpu(
        publisher.begin_publish(kOperationTimeoutMs, reclaimed),
        "reclaim slot after rejected direct color metadata");
    reclaimed.reset();
}

void test_v4_sequence_keyed_move_results_and_corruption_isolation() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(3, 1), publisher),
        "create protocol v4 move-result bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());

    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), registration),
        "register protocol v4 move-result consumer");
    gpu::SharedFrameBusConsumer consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), registration, false, consumer),
        "open protocol v4 move-result consumer");

    gpu::SharedFrameBusWriteLease baseline;
    require_gpu(publisher.begin_publish(kOperationTimeoutMs, baseline),
        "begin move-result baseline frame");
    require(baseline.sequence() == 1,
        "write lease did not expose its reserved frame sequence");
    write_pattern(d3d.context.Get(), baseline.texture(), 1);
    require_gpu(publisher.commit(std::move(baseline)),
        "commit move-result baseline frame");
    write_pattern(d3d.context.Get(), source.Get(), 2);
    require_gpu(publisher.publish(source.Get(), kOperationTimeoutMs),
        "publish move-result target frame");

    gpu::SharedFrameBusMoveResult output;
    output.sequence = 999;
    require_status(
        consumer.try_get_move_result(7, 11, 2, output),
        gpu::GpuStatus::timeout,
        "poll move result before asynchronous publication");
    require(output.sequence == 0,
        "pending move-result query did not clear its output");

    const auto valid = inferred_move_result(2);
    std::array<gpu::SharedFrameBusMoveResult, 4> invalid{};
    invalid.fill(valid);
    --invalid[0].structure_size;
    invalid[1].base_sequence = invalid[1].sequence;
    invalid[2].flags = gpu::shared_frame_bus_move_result_valid;
    invalid[3].move_rects[0].source_x = -1;
    for (const auto& candidate : invalid) {
        require_status(
            publisher.publish_move_result(candidate),
            gpu::GpuStatus::invalid_argument,
            "reject malformed asynchronous move result");
    }
    auto unpublished = inferred_move_result(3);
    require_status(
        publisher.publish_move_result(unpublished),
        gpu::GpuStatus::invalid_argument,
        "reject move result for an unpublished frame");
    require(publisher.stats().move_results_published == 0,
        "rejected move result changed publication statistics");

    require_gpu(publisher.publish_move_result(valid),
        "publish sequence-keyed move result");
    require_status(
        consumer.try_get_move_result(8, 11, 2, output),
        gpu::GpuStatus::timeout,
        "reject move-result epoch mismatch");
    require_status(
        consumer.try_get_move_result(7, 12, 2, output),
        gpu::GpuStatus::timeout,
        "reject move-result nonce mismatch");
    require_gpu(consumer.try_get_move_result(7, 11, 2, output),
        "query exact sequence-keyed move result");
    require(same_move_result(output, valid),
        "sequence-keyed move result did not round-trip");
    require_status(
        publisher.publish_move_result(valid),
        gpu::GpuStatus::invalid_argument,
        "reject duplicate move-result sequence");

    for (std::uint64_t sequence = 3; sequence <= 18; ++sequence) {
        write_pattern(
            d3d.context.Get(), source.Get(),
            static_cast<std::uint32_t>(sequence));
        require_gpu(publisher.publish(source.Get(), kOperationTimeoutMs),
            "publish move-result ring frame");
        require(publisher.sequence() == sequence,
            "frame and move-result sequences diverged");
        require_gpu(publisher.publish_move_result(
            inferred_move_result(sequence)),
            "publish move-result ring entry");
    }
    require_status(
        consumer.try_get_move_result(7, 11, 2, output),
        gpu::GpuStatus::timeout,
        "query overwritten move-result entry");
    const auto retained = inferred_move_result(3);
    require_gpu(consumer.try_get_move_result(7, 11, 3, output),
        "query oldest retained move-result entry");
    require(same_move_result(output, retained),
        "move-result ring retained the wrong oldest entry");
    const auto stats = publisher.stats();
    require(stats.move_results_published == 17
            && stats.move_result_overwrites == 1,
        "move-result ring overwrite statistics are inconsistent");

    ScopedMappedView mapped(MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(
            registration.control_mapping_handle)),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(BusControlPageV4Fixture)));
    require(mapped.get() != nullptr,
        "map protocol v4 move-result corruption fixture");
    auto* page = static_cast<BusControlPageV4Fixture*>(mapped.get());
    BusMoveResultSlotV4Fixture* newest = nullptr;
    for (auto& slot : page->move_results) {
        const auto stamp = static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&slot.commit_sequence, 0, 0));
        if (stamp != 0 && (stamp & 1u) == 0
            && slot.payload.sequence == 18) {
            newest = &slot;
            break;
        }
    }
    require(newest != nullptr,
        "newest move-result cache entry was not committed");
    newest->reserved[0] = 1;
    require_status(
        consumer.try_get_move_result(7, 11, 18, output),
        gpu::GpuStatus::system_error,
        "fail closed on corrupt move-result cache entry");
    require(consumer.initialized(),
        "move-result corruption incorrectly deactivated the consumer");

    gpu::SharedFrameBusFrameLease lease;
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire texture after isolated move-result corruption");
    require(lease.info().sequence == 18,
        "move-result corruption changed the texture publication");
    require_gpu(consumer.release(lease),
        "release texture after move-result corruption");
    require(publisher.stats().quarantined_slots == 0,
        "move-result corruption quarantined a healthy texture slot");
    require_gpu(consumer.close(), "close move-result consumer");
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister move-result consumer");
}

void test_protocol_v3_prefix_remains_readable() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(2, 1), publisher),
        "create v3 compatibility bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    const auto metadata = frame_metadata(301);
    const auto side_data = frame_side_data(301);
    write_pattern(d3d.context.Get(), source.Get(), 301);
    require_gpu(publisher.publish(
        source.Get(), metadata, side_data, kOperationTimeoutMs),
        "publish v3 compatibility frame");

    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), registration),
        "register v3 compatibility consumer");
    ScopedMappedView mapped(MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(
            registration.control_mapping_handle)),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(BusControlPageV4Fixture)));
    require(mapped.get() != nullptr, "map v3 compatibility control page");
    struct ControlHeader final {
        std::uint32_t magic;
        std::uint32_t protocol_version;
        std::uint32_t structure_size;
    };
    auto* header = static_cast<ControlHeader*>(mapped.get());
    header->protocol_version = 3;
    header->structure_size = static_cast<std::uint32_t>(
        sizeof(BusControlPageV3Fixture));

    auto v3_registration = registration;
    v3_registration.protocol_version = 3;
    gpu::SharedFrameBusConsumer consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), v3_registration, true, consumer),
        "open protocol v3 prefix consumer");
    gpu::SharedFrameBusFrameLease lease;
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire protocol v3 prefix frame");
    require(same_metadata(lease.metadata(), metadata)
            && same_side_data(lease.side_data(), side_data),
        "protocol v3 prefix lost metadata or side data");
    require_gpu(consumer.release(lease), "release protocol v3 prefix frame");
    gpu::SharedFrameBusMoveResult move;
    require_status(
        consumer.try_get_move_result(1, 1, 1, move),
        gpu::GpuStatus::unsupported,
        "query v4 move result through protocol v3 prefix");
    require_gpu(consumer.close(), "close protocol v3 prefix consumer");

    header->protocol_version = gpu::shared_frame_bus_protocol_version;
    header->structure_size = static_cast<std::uint32_t>(
        sizeof(BusControlPageV4Fixture));
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister protocol v3 prefix consumer");
}

void test_metadata_commit_stamp_corruption_quarantines_slot() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(3, 1), publisher),
        "create metadata corruption bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    const auto metadata = frame_metadata(100);
    write_pattern(d3d.context.Get(), source.Get(), 100);
    require_gpu(publisher.publish(source.Get(), metadata, kOperationTimeoutMs),
        "publish metadata corruption fixture");

    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), registration),
        "register metadata corruption consumer");
    constexpr std::size_t v1_prefix_size = 384;
    constexpr std::size_t v2_page_size = 1408;
    ScopedMappedView mapped(MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(
            registration.control_mapping_handle)),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, v2_page_size));
    require(mapped.get() != nullptr, "map metadata corruption fixture");
    auto* stamp = reinterpret_cast<volatile LONG64*>(
        static_cast<std::uint8_t*>(mapped.get()) + v1_prefix_size);
    require(static_cast<std::uint64_t>(InterlockedCompareExchange64(stamp, 0, 0))
            == publisher.sequence(),
        "metadata commit stamp did not match the published slot sequence");
    (void)InterlockedExchange64(stamp, 0);

    gpu::SharedFrameBusConsumer consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), registration, true, consumer),
        "open metadata corruption consumer");
    gpu::SharedFrameBusFrameLease lease;
    require_status(
        consumer.acquire_latest(kOperationTimeoutMs, lease),
        gpu::GpuStatus::system_error,
        "reject mismatched metadata commit stamp");
    require(!lease && !consumer.initialized(),
        "metadata corruption returned a lease or left the consumer active");
    require_gpu(consumer.close(), "close metadata corruption consumer");
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister metadata corruption consumer");
    require(publisher.stats().quarantined_slots == 1,
        "metadata corruption did not quarantine its slot");
}

void test_side_data_commit_stamp_corruption_quarantines_slot() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(3, 1), publisher),
        "create side-data corruption bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    const auto metadata = frame_metadata(110);
    const auto side_data = frame_side_data(110);
    write_pattern(d3d.context.Get(), source.Get(), 110);
    require_gpu(
        publisher.publish(
            source.Get(), metadata, side_data, kOperationTimeoutMs),
        "publish side-data corruption fixture");

    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), registration),
        "register side-data corruption consumer");
    ScopedMappedView mapped(MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(
            registration.control_mapping_handle)),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(BusControlPageV4Fixture)));
    require(mapped.get() != nullptr, "map side-data corruption fixture");
    auto* page = static_cast<BusControlPageV3Fixture*>(mapped.get());
    BusSlotSideDataV3Fixture* committed = nullptr;
    for (auto& slot : page->side_data) {
        if (static_cast<std::uint64_t>(InterlockedCompareExchange64(
                &slot.sequence, 0, 0)) == publisher.sequence()) {
            committed = &slot;
            break;
        }
    }
    require(committed != nullptr,
        "published side data did not have a matching commit stamp");
    (void)InterlockedExchange64(&committed->sequence, 0);

    gpu::SharedFrameBusConsumer consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), registration, true, consumer),
        "open side-data corruption consumer");
    gpu::SharedFrameBusFrameLease lease;
    require_status(
        consumer.acquire_latest(kOperationTimeoutMs, lease),
        gpu::GpuStatus::system_error,
        "reject mismatched side-data commit stamp");
    require(!lease && !consumer.initialized(),
        "side-data corruption returned a lease or left the consumer active");
    require_gpu(consumer.close(), "close side-data corruption consumer");
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister side-data corruption consumer");
    require(publisher.stats().quarantined_slots == 1,
        "side-data corruption did not quarantine its texture slot");
}

void test_v3_cursor_shape_cache_round_trip_and_fail_closed() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(2, 1), publisher),
        "create protocol v3 cursor-shape bus");
    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), registration),
        "register protocol v3 cursor-shape consumer");
    ScopedMappedView mapped(MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(
            registration.control_mapping_handle)),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(BusControlPageV4Fixture)));
    require(mapped.get() != nullptr, "map cursor-shape cache fixture");
    auto* page = static_cast<BusControlPageV3Fixture*>(mapped.get());
    gpu::SharedFrameBusConsumer consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), registration, true, consumer),
        "open protocol v3 cursor-shape consumer");

    gpu::WgcCursorShape color;
    color.sequence = 1'001;
    color.kind = gpu::WgcCursorShapeKind::color_bgra8;
    color.width = 4;
    color.height = 3;
    color.hotspot_x = 1;
    color.hotspot_y = 2;
    color.stride_bytes = color.width * 4;
    color.data.resize(
        static_cast<std::size_t>(color.stride_bytes) * color.height);
    for (std::size_t index = 0; index < color.data.size(); ++index) {
        color.data[index] = static_cast<std::uint8_t>(index * 7u + 3u);
    }

    gpu::WgcCursorShape monochrome;
    monochrome.sequence = 1'002;
    monochrome.kind = gpu::WgcCursorShapeKind::monochrome_and_xor;
    monochrome.width = 9;
    monochrome.height = 5;
    monochrome.hotspot_x = 8;
    monochrome.hotspot_y = 4;
    monochrome.stride_bytes = 4;
    monochrome.data.resize(
        static_cast<std::size_t>(monochrome.stride_bytes)
            * monochrome.height * 2,
        0xa5);

    require_gpu(publisher.publish_cursor_shape(color),
        "publish color cursor shape");
    require_gpu(publisher.publish_cursor_shape(monochrome),
        "publish monochrome cursor shape");
    gpu::WgcCursorShape received;
    require_gpu(consumer.cursor_shape(color.sequence, received),
        "read color cursor shape");
    require(same_cursor_shape(received, color),
        "color cursor shape did not round-trip through protocol v3");
    require_gpu(consumer.cursor_shape(monochrome.sequence, received),
        "read monochrome cursor shape");
    require(same_cursor_shape(received, monochrome),
        "monochrome cursor shape did not round-trip through protocol v3");

    auto masked = color;
    masked.sequence = 1'003;
    masked.kind = gpu::WgcCursorShapeKind::masked_color_bgra8;
    require_gpu(publisher.publish_cursor_shape(masked),
        "publish masked-color cursor shape");
    require_gpu(consumer.cursor_shape(masked.sequence, received),
        "read masked-color cursor shape");
    require(same_cursor_shape(received, masked),
        "masked-color cursor shape did not round-trip through protocol v3");

    std::vector<gpu::WgcCursorShape> malformed;
    auto invalid = color;
    invalid.sequence = 0;
    malformed.push_back(invalid);
    invalid = color;
    invalid.kind = gpu::WgcCursorShapeKind::none;
    malformed.push_back(invalid);
    invalid = color;
    invalid.width = 0;
    malformed.push_back(invalid);
    invalid = color;
    invalid.hotspot_x = invalid.width;
    malformed.push_back(invalid);
    invalid = color;
    --invalid.stride_bytes;
    malformed.push_back(invalid);
    invalid = color;
    invalid.data.pop_back();
    malformed.push_back(invalid);
    invalid = color;
    invalid.data.resize(
        gpu::shared_frame_bus_max_cursor_shape_bytes + 1);
    malformed.push_back(std::move(invalid));
    for (const auto& shape : malformed) {
        require_status(
            publisher.publish_cursor_shape(shape),
            gpu::GpuStatus::invalid_argument,
            "reject malformed protocol v3 cursor shape");
    }
    require_gpu(consumer.cursor_shape(monochrome.sequence, received),
        "read cursor shape after rejected publications");
    require(same_cursor_shape(received, monochrome),
        "rejected cursor publication damaged the shape cache");

    auto replacement = color;
    replacement.sequence = 1'004;
    replacement.data[0] ^= 0xff;
    require_gpu(publisher.publish_cursor_shape(replacement),
        "publish replacement cursor shape");
    gpu::WgcCursorShape stale = monochrome;
    const gpu::GpuError stale_result =
        consumer.cursor_shape(color.sequence, stale);
    require(!stale_result
            && stale_result.status == gpu::GpuStatus::timeout
            && stale_result.hresult == HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
        "overwritten cursor shape did not report not-found timeout");
    require(stale.sequence == 0 && stale.data.empty(),
        "failed stale cursor lookup retained caller-visible shape data");

    BusCursorShapeSlotV3Fixture* replacement_slot = nullptr;
    for (auto& slot : page->cursor_shapes) {
        const std::uint64_t commit = static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&slot.commit_sequence, 0, 0));
        if (slot.shape_sequence == replacement.sequence
            && commit != 0
            && (commit & 1u) == 0) {
            replacement_slot = &slot;
            break;
        }
    }
    require(replacement_slot != nullptr,
        "replacement cursor shape did not have a stable cache commit stamp");
    const std::uint64_t saved_commit = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(
            &replacement_slot->commit_sequence, 0, 0));
    const std::uint32_t saved_data_size = replacement_slot->data_size;
    replacement_slot->data_size =
        gpu::shared_frame_bus_max_cursor_shape_bytes + 1;
    gpu::WgcCursorShape corrupt = monochrome;
    require_status(
        consumer.cursor_shape(replacement.sequence, corrupt),
        gpu::GpuStatus::system_error,
        "reject malformed cursor cache payload");
    require(corrupt.sequence == 0 && corrupt.data.empty(),
        "malformed cursor cache payload escaped into the output object");
    require(consumer.initialized()
            && publisher.stats().quarantined_slots == 0,
        "cursor cache corruption poisoned the consumer or a texture slot");

    (void)InterlockedExchange64(&replacement_slot->commit_sequence, 0);
    replacement_slot->data_size = saved_data_size;
    (void)InterlockedExchange64(
        &replacement_slot->commit_sequence,
        static_cast<LONG64>(saved_commit));
    require_gpu(consumer.cursor_shape(replacement.sequence, received),
        "read repaired cursor cache payload");
    require(same_cursor_shape(received, replacement),
        "cursor cache did not recover after restoring a valid payload");

    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    write_pattern(d3d.context.Get(), source.Get(), 230);
    require_gpu(publisher.publish(source.Get(), kOperationTimeoutMs),
        "publish texture after cursor cache corruption");
    gpu::SharedFrameBusFrameLease lease;
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire texture after cursor cache corruption");
    require_gpu(consumer.release(lease),
        "release texture after cursor cache corruption");
    require_gpu(consumer.close(), "close cursor-shape consumer");
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister cursor-shape consumer");
}

void test_slow_consumer_does_not_block_fast_consumer() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(3, 2), publisher),
        "create two-consumer bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    ChildProcess slow = ChildProcess::spawn(L"--consumer-child");
    ChildProcess fast = ChildProcess::spawn(L"--consumer-child");
    const auto slow_registration = attach_consumer(publisher, slow);
    const auto fast_registration = attach_consumer(publisher, fast);

    write_pattern(d3d.context.Get(), source.Get(), 30);
    require_gpu(publisher.publish(source.Get(), kOperationTimeoutMs), "initial publish");
    require(acquire_and_validate(slow, 30) == 1,
        "slow consumer did not acquire the initial frame");
    require(acquire_and_validate(fast, 30) == 1,
        "fast consumer did not acquire the initial frame");
    release_child_lease(fast);

    std::uint64_t previous = 1;
    for (std::uint32_t frame_id = 31; frame_id <= 40; ++frame_id) {
        write_pattern(d3d.context.Get(), source.Get(), frame_id);
        require_gpu(
            publisher.publish(source.Get(), kOperationTimeoutMs),
            "publish while slow consumer holds a lease");
        const std::uint64_t sequence = acquire_and_validate(fast, frame_id);
        require(sequence == previous + 1,
            "fast consumer stopped making continuous progress");
        previous = sequence;
        release_child_lease(fast);
    }

    const WireResponse still_valid = command_child(
        slow, WireOperation::validate_held, 30);
    require_wire_success(
        still_valid, WireOperation::validate_held, "validate slow held frame");
    require(still_valid.sequence == 1,
        "slow consumer's held lease changed sequence");
    release_child_lease(slow);
    require(publisher.stats().no_slot == 0,
        "a single slow consumer exhausted a three-slot bus");

    close_registered_consumer(publisher, slow_registration, slow);
    close_registered_consumer(publisher, fast_registration, fast);
}

void test_multi_inflight_out_of_order_retirement() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(4, 1), publisher),
        "create multi-inflight bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());

    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), registration),
        "register multi-inflight consumer");
    gpu::SharedFrameBusConsumer consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), registration, true, consumer),
        "open multi-inflight consumer");

    std::array<gpu::SharedFrameBusFrameLease, 4> leases;
    for (std::uint32_t frame = 1; frame <= leases.size(); ++frame) {
        write_pattern(d3d.context.Get(), source.Get(), frame);
        const auto metadata = frame_metadata(frame);
        require_gpu(
            publisher.publish(source.Get(), metadata, kOperationTimeoutMs),
            "publish multi-inflight frame");
        require_gpu(
            consumer.acquire_latest(
                kOperationTimeoutMs, leases[frame - 1]),
            "acquire simultaneous bus lease");
        require(leases[frame - 1].info().sequence == frame
                && same_metadata(leases[frame - 1].metadata(), metadata),
            "simultaneous bus lease metadata is inconsistent");
    }

    require_status(
        consumer.close(),
        gpu::GpuStatus::invalid_argument,
        "close with multiple bus leases");
    require_gpu(leases[2].release(), "release sequence 3 out of order");
    require_gpu(leases[3].release(), "release sequence 4 out of order");
    require_gpu(leases[1].release(), "release sequence 2 out of order");

    write_pattern(d3d.context.Get(), source.Get(), 5);
    require_status(
        publisher.publish(source.Get(), frame_metadata(5), 0),
        gpu::GpuStatus::timeout,
        "publish before the retirement frontier advances");

    require_gpu(leases[0].release(), "release sequence 1 retirement frontier");
    require_gpu(
        publisher.publish(
            source.Get(), frame_metadata(5), kOperationTimeoutMs),
        "publish after contiguous out-of-order retirement");
    gpu::SharedFrameBusFrameLease final_lease;
    require_gpu(
        consumer.acquire_latest(kOperationTimeoutMs, final_lease),
        "acquire after multi-inflight retirement");
    require(final_lease.info().sequence == 5,
        "multi-inflight retirement did not expose the next sequence");
    require_gpu(final_lease.release(), "release final multi-inflight frame");
    require_gpu(consumer.close(), "close multi-inflight consumer");
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister multi-inflight consumer");
    require(publisher.stats().quarantined_slots == 0,
        "out-of-order retirement quarantined a healthy slot");
}

void test_normal_close_reuses_consumer_index() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(2, 1), publisher),
        "create index-reuse bus");

    ChildProcess first = ChildProcess::spawn(L"--consumer-child");
    const auto first_registration = attach_consumer(publisher, first);
    close_registered_consumer(publisher, first_registration, first);

    ChildProcess second = ChildProcess::spawn(L"--consumer-child");
    const auto second_registration = attach_consumer(publisher, second);
    require(second_registration.consumer_index == first_registration.consumer_index,
        "normal consumer close did not release its registration index");
    close_registered_consumer(publisher, second_registration, second);

    const auto stats = publisher.stats();
    require(stats.consumer_registrations == 2
            && stats.consumer_reclamations == 2
            && stats.dead_consumer_reclamations == 0
            && stats.active_consumers == 0,
        "normal close/index-reuse statistics are inconsistent");
}

void test_crashed_consumer_reclaims_or_quarantines_its_held_slot() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(3, 2), publisher),
        "create crash-quarantine bus");
    ComPtr<ID3D11Texture2D> source = create_source_texture(d3d.device.Get());
    ChildProcess crashed = ChildProcess::spawn(L"--consumer-child");
    ChildProcess healthy = ChildProcess::spawn(L"--consumer-child");
    const auto crashed_registration = attach_consumer(publisher, crashed);
    const auto healthy_registration = attach_consumer(publisher, healthy);

    write_pattern(d3d.context.Get(), source.Get(), 50);
    require_gpu(publisher.publish(source.Get(), kOperationTimeoutMs), "pre-crash publish");
    require(acquire_and_validate(crashed, 50) == 1,
        "crash child did not hold the newest slot");
    crashed.terminate(91);
    require(WaitForSingleObject(crashed.process(), kOperationTimeoutMs)
            == WAIT_OBJECT_0,
        "crash child did not terminate");

    write_pattern(d3d.context.Get(), source.Get(), 51);
    require_gpu(
        publisher.publish(source.Get(), kOperationTimeoutMs),
        "publish after consumer crash");
    auto stats = publisher.stats();
    require(stats.dead_consumer_reclamations == 1
            && stats.consumer_reclamations == 1
            && stats.quarantined_slots == 1
            && stats.active_consumers == 1,
        "dead consumer quarantine stats: dead="
            + std::to_string(stats.dead_consumer_reclamations)
            + ", reclaimed=" + std::to_string(stats.consumer_reclamations)
            + ", quarantined=" + std::to_string(stats.quarantined_slots)
            + ", active=" + std::to_string(stats.active_consumers));
    require(acquire_and_validate(healthy, 51) == 2,
        "healthy consumer could not acquire after peer crash");
    release_child_lease(healthy);

    for (std::uint32_t frame_id = 52; frame_id <= 58; ++frame_id) {
        write_pattern(d3d.context.Get(), source.Get(), frame_id);
        require_gpu(
            publisher.publish(source.Get(), kOperationTimeoutMs),
            "continued publish after quarantine");
        require(acquire_and_validate(healthy, frame_id) == frame_id - 49,
            "healthy consumer lost sequence progress after quarantine");
        release_child_lease(healthy);
    }

    ChildProcess replacement = ChildProcess::spawn(L"--consumer-child");
    const auto replacement_registration = attach_consumer(publisher, replacement);
    require(replacement_registration.consumer_index
            == crashed_registration.consumer_index,
        "dead consumer index was not reusable");
    require(acquire_and_validate(replacement, 58) == publisher.sequence(),
        "replacement consumer could not acquire the current latest frame");
    release_child_lease(replacement);

    close_registered_consumer(publisher, healthy_registration, healthy);
    close_registered_consumer(publisher, replacement_registration, replacement);
    stats = publisher.stats();
    require(stats.quarantined_slots == 1
            && stats.dead_consumer_reclamations == 1
            && stats.active_consumers == 0,
        "quarantine or consumer statistics changed unexpectedly");
}

void test_producer_exit_wakes_finite_and_infinite_waits() {
    struct Scenario final {
        std::uint32_t timeout_ms;
        ProducerExitMode mode;
        const char* label;
    };
    constexpr Scenario scenarios[] = {
        {5'000, ProducerExitMode::graceful, "finite-graceful"},
        {INFINITE, ProducerExitMode::abrupt, "infinite-abrupt"}};
    for (const Scenario& scenario : scenarios) {
        ChildProcess child = ChildProcess::spawn(
            L"--death-scenario",
            {std::to_wstring(scenario.timeout_ms),
             std::to_wstring(static_cast<std::uint32_t>(scenario.mode))});
        const WireResponse response = child.receive<WireResponse>(
            kChildTimeoutMs, "producer-death scenario");
        require_wire_success(
            response, WireOperation::death_scenario,
            std::string("producer-death scenario ") + scenario.label);
        require(response.sequence < 3'000,
            "producer-death scenario exceeded its wake latency bound");
        child.wait(kOperationTimeoutMs, "producer-death scenario exit");
    }
}

void test_invalid_metadata_and_lease_states() {
    DeviceContext d3d = create_device();
    gpu::SharedFrameBusPublisher publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(3, 2), publisher),
        "create lease-state bus");

    require_status(
        publisher.publish(nullptr, 0),
        gpu::GpuStatus::invalid_argument,
        "publish null texture");
    D3D11_TEXTURE2D_DESC wrong_description{};
    wrong_description.Width = kWidth + 1;
    wrong_description.Height = kHeight;
    wrong_description.MipLevels = 1;
    wrong_description.ArraySize = 1;
    wrong_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    wrong_description.SampleDesc.Count = 1;
    ComPtr<ID3D11Texture2D> wrong_texture;
    HRESULT hr = d3d.device->CreateTexture2D(
        &wrong_description, nullptr, &wrong_texture);
    if (FAILED(hr)) fail("wrong-size texture creation failed");
    require_status(
        publisher.publish(wrong_texture.Get(), 0),
        gpu::GpuStatus::invalid_argument,
        "publish mismatched texture");

    gpu::SharedFrameBusPublisher other_publisher;
    require_gpu(
        gpu::SharedFrameBusPublisher::create(
            d3d.device.Get(), bus_config(3, 1), other_publisher),
        "create foreign publisher");
    gpu::SharedFrameBusWriteLease empty_write;
    require_status(
        other_publisher.commit(std::move(empty_write)),
        gpu::GpuStatus::invalid_argument,
        "commit empty write lease");

    gpu::SharedFrameBusWriteLease write;
    require_gpu(publisher.begin_publish(0, write), "begin lease-state publish");
    require_status(
        publisher.begin_publish(0, write),
        gpu::GpuStatus::invalid_argument,
        "begin with nonempty write output");
    require_status(
        other_publisher.commit(std::move(write)),
        gpu::GpuStatus::invalid_argument,
        "commit write lease on wrong publisher");
    require(static_cast<bool>(write),
        "wrong-publisher commit consumed the valid write lease");
    write_pattern(d3d.context.Get(), write.texture(), 70);
    require_gpu(publisher.commit(std::move(write)), "commit recovered write lease");
    require(!write, "successful commit did not clear the write lease");

    gpu::SharedFrameBusWriteLease cancelled;
    require_gpu(publisher.begin_publish(0, cancelled), "begin cancelled write");
    cancelled.reset();
    require_gpu(publisher.begin_publish(0, cancelled), "reuse cancelled write slot");
    cancelled.reset();

    gpu::SharedFrameBusRegistration registration;
    require_gpu(
        publisher.register_consumer(GetCurrentProcess(), registration),
        "register in-process metadata consumer");
    auto bad_registration = registration;
    ++bad_registration.width;
    gpu::SharedFrameBusConsumer rejected;
    require_status(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), bad_registration, false, rejected),
        gpu::GpuStatus::invalid_argument,
        "open mismatched registration metadata");
    require(!rejected.initialized(),
        "failed metadata open initialized a consumer");

    gpu::SharedFrameBusConsumer consumer;
    require_gpu(
        gpu::SharedFrameBusConsumer::open(
            d3d.device.Get(), registration, true, consumer),
        "open in-process consumer");
    gpu::SharedFrameBusFrameLease lease;
    require_gpu(consumer.acquire_latest(kOperationTimeoutMs, lease),
        "acquire lease-state frame");
    require(lease.info().sequence == 1
            && lease.info().width == kWidth
            && lease.info().height == kHeight
            && lease.info().format == DXGI_FORMAT_B8G8R8A8_UNORM,
        "frame lease metadata is inconsistent");
    validate_pattern(
        d3d.device.Get(), d3d.context.Get(), lease.texture(), 70);

    require_status(
        consumer.acquire_latest(0, lease),
        gpu::GpuStatus::invalid_argument,
        "acquire into nonempty frame lease");
    require_status(
        consumer.close(),
        gpu::GpuStatus::invalid_argument,
        "close consumer with held frame");
    gpu::SharedFrameBusConsumer foreign_consumer;
    require_status(
        foreign_consumer.release(lease),
        gpu::GpuStatus::invalid_argument,
        "release frame through foreign consumer");
    require(static_cast<bool>(lease),
        "foreign release consumed the valid frame lease");
    require_gpu(consumer.release(lease), "release valid frame lease");
    require(!lease, "successful release did not clear the frame lease");
    require_status(
        lease.release(),
        gpu::GpuStatus::invalid_argument,
        "release empty frame lease");

    gpu::SharedFrameBusFrameLease no_new_frame;
    require_status(
        consumer.acquire_latest(0, no_new_frame),
        gpu::GpuStatus::timeout,
        "acquire without newer frame");
    require_gpu(consumer.close(), "close in-process consumer");
    require_status(
        consumer.acquire_latest(0, no_new_frame),
        gpu::GpuStatus::invalid_argument,
        "acquire from closed consumer");
    require_gpu(
        publisher.unregister_consumer(registration, kOperationTimeoutMs),
        "unregister in-process consumer");
    require_status(
        publisher.unregister_consumer(registration, 0),
        gpu::GpuStatus::invalid_argument,
        "unregister stale registration");
}

void run_test(std::string_view name, const std::function<void()>& test) {
    const auto begin = Clock::now();
    test();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - begin);
    std::cout << "[PASS] " << name << " (" << elapsed.count() << " ms)\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc >= 4) {
        HANDLE input = parse_handle(argv[2]);
        HANDLE output = parse_handle(argv[3]);
        if (input == nullptr || output == nullptr) return 64;
        const std::wstring_view mode(argv[1]);
        int result = 65;
        if (mode == L"--consumer-child" && argc == 4) {
            result = run_consumer_child(input, output);
        } else if (mode == L"--producer-child" && argc == 5) {
            result = run_producer_child(
                input, output, parse_u32(argv[4], "target process id"));
        } else if (mode == L"--death-scenario" && argc == 6) {
            const auto timeout = parse_u32(argv[4], "acquire timeout");
            const auto raw_mode = parse_u32(argv[5], "producer exit mode");
            if (raw_mode == static_cast<std::uint32_t>(ProducerExitMode::graceful)
                || raw_mode == static_cast<std::uint32_t>(ProducerExitMode::abrupt)) {
                result = run_death_scenario(
                    output, timeout, static_cast<ProducerExitMode>(raw_mode));
            }
        }
        CloseHandle(input);
        CloseHandle(output);
        return result;
    }

    try {
        run_test(
            "cross-process copy/direct publish and latest sequence",
            test_copy_direct_and_latest_sequence);
        run_test(
            "protocol v5 side data round-trip, validation, and legacy clear",
            test_v5_side_data_round_trip_validation_and_legacy_clear);
        run_test(
            "metadata validation, legacy clear, and v1/v2 compatibility",
            test_metadata_validation_legacy_clear_and_v1_v2_open);
        run_test(
            "bus color configuration and per-frame contract validation",
            test_bus_color_contract_validation);
        run_test(
            "protocol v4 sequence-keyed move results and corruption isolation",
            test_v4_sequence_keyed_move_results_and_corruption_isolation);
        run_test(
            "protocol v3 prefix remains readable",
            test_protocol_v3_prefix_remains_readable);
        run_test(
            "metadata commit stamp corruption quarantines its slot",
            test_metadata_commit_stamp_corruption_quarantines_slot);
        run_test(
            "side-data commit stamp corruption quarantines its slot",
            test_side_data_commit_stamp_corruption_quarantines_slot);
        run_test(
            "protocol v3 cursor-shape cache round-trip and fail-closed safety",
            test_v3_cursor_shape_cache_round_trip_and_fail_closed);
        run_test(
            "slow consumer does not block fast consumer",
            test_slow_consumer_does_not_block_fast_consumer);
        run_test(
            "multiple in-flight leases retire safely out of order",
            test_multi_inflight_out_of_order_retirement);
        run_test(
            "normal close reuses consumer index",
            test_normal_close_reuses_consumer_index);
        run_test(
            "crashed consumer safely retires only its held slot",
            test_crashed_consumer_reclaims_or_quarantines_its_held_slot);
        run_test(
            "producer exit wakes finite and infinite waits",
            test_producer_exit_wakes_finite_and_infinite_waits);
        run_test(
            "invalid metadata and lease states",
            test_invalid_metadata_and_lease_states);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] SharedFrameBus tests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
