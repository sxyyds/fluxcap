#include "gpu_move_inference.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace fluxcap::gpu::internal {
namespace {

using Microsoft::WRL::ComPtr;

constexpr char hash_shader_source[] = R"(
Texture2D<float4> InputTexture : register(t0);
RWStructuredBuffer<uint2> OutputHashes : register(u0);

cbuffer HashConstants : register(b0) {
    uint FrameWidth;
    uint FrameHeight;
    uint TilesX;
    uint TileCount;
    uint RegionX;
    uint RegionY;
    uint Reserved0;
    uint Reserved1;
};

uint mix_hash(uint value, uint input, uint multiplier) {
    value ^= input;
    value *= multiplier;
    value ^= value >> 13;
    return value;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint tile_index = dispatch_id.x;
    if (tile_index >= TileCount) return;

    const uint tile_x = tile_index % TilesX;
    const uint tile_y = tile_index / TilesX;
    const uint origin_x = tile_x * 16;
    const uint origin_y = tile_y * 16;
    const uint valid_width = min(16, FrameWidth - origin_x);
    const uint valid_height = min(16, FrameHeight - origin_y);

    uint first = 2166136261u ^ (valid_width | (valid_height << 8));
    uint second = 2246822519u ^ (valid_height | (valid_width << 8));
    [loop]
    for (uint y = 0; y < valid_height; ++y) {
        [loop]
        for (uint x = 0; x < valid_width; ++x) {
            const uint4 bits = asuint(InputTexture.Load(int3(
                RegionX + origin_x + x,
                RegionY + origin_y + y,
                0)));
            first = mix_hash(first, bits.x, 16777619u);
            first = mix_hash(first, bits.y, 16777619u);
            first = mix_hash(first, bits.z, 16777619u);
            first = mix_hash(first, bits.w, 16777619u);
            second = mix_hash(second, bits.w ^ (x * 374761393u), 3266489917u);
            second = mix_hash(second, bits.z ^ (y * 668265263u), 3266489917u);
            second = mix_hash(second, bits.y, 3266489917u);
            second = mix_hash(second, bits.x, 3266489917u);
        }
    }
    OutputHashes[tile_index] = uint2(first, second);
}
)";

constexpr char match_shader_source[] = R"(
StructuredBuffer<uint2> CurrentHashes : register(t0);
StructuredBuffer<uint2> PreviousHashes : register(t1);
StructuredBuffer<uint> CandidateTiles : register(t2);
RWStructuredBuffer<uint2> MatchResults : register(u0);

cbuffer MatchConstants : register(b0) {
    uint TileCount;
    uint CandidateCount;
    uint Reserved0;
    uint Reserved1;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint candidate_index = dispatch_id.x;
    if (candidate_index >= CandidateCount) return;
    const uint destination = CandidateTiles[candidate_index];
    const uint2 wanted = CurrentHashes[destination];
    uint source = 0xffffffffu;
    uint count = 0;
    [loop]
    for (uint index = 0; index < TileCount; ++index) {
        const uint2 candidate = PreviousHashes[index];
        if (candidate.x == wanted.x && candidate.y == wanted.y) {
            source = index;
            ++count;
            if (count > 1) break;
        }
    }
    MatchResults[candidate_index] = uint2(source, min(count, 2));
}
)";

struct HashConstants final {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t tiles_x;
    std::uint32_t tile_count;
    std::uint32_t region_x;
    std::uint32_t region_y;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
};

struct MatchConstants final {
    std::uint32_t tile_count;
    std::uint32_t candidate_count;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
};

struct MatchRecord final {
    std::uint32_t source;
    std::uint32_t count;
};

struct TileMatch final {
    std::uint32_t source;
    std::uint32_t destination;
};

struct Group final {
    std::uint32_t source_x;
    std::uint32_t source_y;
    std::uint32_t destination_x;
    std::uint32_t destination_y;
    std::uint32_t width_tiles;
    std::uint32_t height_tiles;
};

GpuError make_error(
    GpuStatus status,
    HRESULT hresult,
    const char* message) noexcept {
    GpuError output;
    output.status = status;
    output.hresult = hresult;
    if (message != nullptr) {
        (void)strncpy_s(
            output.message.data(), output.message.size(), message, _TRUNCATE);
    }
    return output;
}

GpuError success() noexcept {
    return make_error(GpuStatus::ok, S_OK, "ok");
}

GpuError d3d_error(
    ID3D11Device* device,
    HRESULT hresult,
    const char* message) noexcept {
    const HRESULT removed = device != nullptr
        ? device->GetDeviceRemovedReason()
        : S_OK;
    return make_error(
        FAILED(removed) ? GpuStatus::device_lost : GpuStatus::system_error,
        FAILED(removed) ? removed : hresult,
        message);
}

bool supported_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM
        || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool same_device(ID3D11Device* left, ID3D11Device* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> first;
    ComPtr<IUnknown> second;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&first)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&second)))
        && first.Get() == second.Get();
}

HRESULT compile_shader(
    const char* source,
    std::size_t size,
    ComPtr<ID3DBlob>& bytecode) noexcept {
    ComPtr<ID3DBlob> diagnostics;
    return D3DCompile(
        source,
        size,
        nullptr,
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &bytecode,
        &diagnostics);
}

HRESULT create_structured_buffer(
    ID3D11Device* device,
    std::uint32_t element_count,
    std::uint32_t stride,
    UINT bind_flags,
    D3D11_USAGE usage,
    UINT cpu_access,
    ComPtr<ID3D11Buffer>& output) noexcept {
    if (element_count == 0
        || stride == 0
        || element_count > std::numeric_limits<UINT>::max() / stride) {
        return E_INVALIDARG;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = element_count * stride;
    description.Usage = usage;
    description.BindFlags = bind_flags;
    description.CPUAccessFlags = cpu_access;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = stride;
    return device->CreateBuffer(&description, nullptr, &output);
}

HRESULT create_buffer_srv(
    ID3D11Device* device,
    ID3D11Buffer* buffer,
    std::uint32_t element_count,
    ComPtr<ID3D11ShaderResourceView>& output) noexcept {
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    description.Buffer.FirstElement = 0;
    description.Buffer.NumElements = element_count;
    return device->CreateShaderResourceView(buffer, &description, &output);
}

HRESULT create_buffer_uav(
    ID3D11Device* device,
    ID3D11Buffer* buffer,
    std::uint32_t element_count,
    ComPtr<ID3D11UnorderedAccessView>& output) noexcept {
    D3D11_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    description.Buffer.FirstElement = 0;
    description.Buffer.NumElements = element_count;
    return device->CreateUnorderedAccessView(buffer, &description, &output);
}

HRESULT create_constant_buffer(
    ID3D11Device* device,
    std::uint32_t size,
    ComPtr<ID3D11Buffer>& output) noexcept {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = (size + 15u) & ~15u;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return device->CreateBuffer(&description, nullptr, &output);
}

template <typename Value>
HRESULT write_dynamic_buffer(
    ID3D11DeviceContext* context,
    ID3D11Buffer* buffer,
    const Value* values,
    std::size_t count) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(
        buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    if (count != 0) {
        std::memcpy(mapped.pData, values, sizeof(Value) * count);
    }
    context->Unmap(buffer, 0);
    return S_OK;
}

std::vector<std::uint32_t> candidate_tiles(
    const GpuMoveInferenceConfig& config,
    std::uint32_t tiles_x,
    std::uint32_t tiles_y,
    std::span<const WgcRect> dirty_rects,
    bool& overflow) {
    overflow = false;
    std::vector<std::uint8_t> selected(
        static_cast<std::size_t>(tiles_x) * tiles_y, 0);
    std::vector<std::uint32_t> output;
    output.reserve(std::min<std::size_t>(
        config.max_candidate_tiles, selected.size()));

    for (const WgcRect& rect : dirty_rects) {
        const std::int64_t raw_left = rect.x;
        const std::int64_t raw_top = rect.y;
        const std::int64_t raw_right = raw_left + rect.width;
        const std::int64_t raw_bottom = raw_top + rect.height;
        const std::int64_t left = std::clamp<std::int64_t>(
            raw_left, 0, config.width);
        const std::int64_t top = std::clamp<std::int64_t>(
            raw_top, 0, config.height);
        const std::int64_t right = std::clamp<std::int64_t>(
            raw_right, 0, config.width);
        const std::int64_t bottom = std::clamp<std::int64_t>(
            raw_bottom, 0, config.height);
        if (left >= right || top >= bottom) continue;

        const std::uint32_t first_x = static_cast<std::uint32_t>(
            (left + gpu_move_tile_size - 1) / gpu_move_tile_size);
        const std::uint32_t first_y = static_cast<std::uint32_t>(
            (top + gpu_move_tile_size - 1) / gpu_move_tile_size);
        const std::uint32_t last_x = static_cast<std::uint32_t>(
            right / gpu_move_tile_size);
        const std::uint32_t last_y = static_cast<std::uint32_t>(
            bottom / gpu_move_tile_size);
        for (std::uint32_t y = first_y; y < std::min(last_y, tiles_y); ++y) {
            for (std::uint32_t x = first_x; x < std::min(last_x, tiles_x); ++x) {
                const std::uint32_t index = y * tiles_x + x;
                if (selected[index] != 0) continue;
                selected[index] = 1;
                if (output.size() >= config.max_candidate_tiles) {
                    overflow = true;
                    return {};
                }
                output.push_back(index);
            }
        }
    }
    return output;
}

std::vector<Group> group_matches(
    std::uint32_t tiles_x,
    std::uint32_t tiles_y,
    std::span<const TileMatch> matches,
    std::uint32_t minimum_group_tiles) {
    const std::size_t tile_count = static_cast<std::size_t>(tiles_x) * tiles_y;
    std::vector<std::int32_t> source_by_destination(tile_count, -1);
    std::vector<std::uint16_t> source_use_count(tile_count, 0);
    for (const TileMatch& match : matches) {
        if (match.source >= tile_count
            || match.destination >= tile_count
            || match.source == match.destination) {
            continue;
        }
        source_by_destination[match.destination] =
            static_cast<std::int32_t>(match.source);
        if (source_use_count[match.source]
            != std::numeric_limits<std::uint16_t>::max()) {
            ++source_use_count[match.source];
        }
    }
    for (std::size_t destination = 0;
         destination < source_by_destination.size();
         ++destination) {
        const std::int32_t source = source_by_destination[destination];
        if (source >= 0 && source_use_count[static_cast<std::size_t>(source)] != 1) {
            source_by_destination[destination] = -1;
        }
    }

    std::vector<std::uint8_t> consumed(tile_count, 0);
    std::vector<Group> groups;
    for (std::uint32_t destination = 0;
         destination < tile_count;
         ++destination) {
        if (consumed[destination] != 0
            || source_by_destination[destination] < 0) {
            continue;
        }
        const std::uint32_t destination_x = destination % tiles_x;
        const std::uint32_t destination_y = destination / tiles_x;
        const std::uint32_t source = static_cast<std::uint32_t>(
            source_by_destination[destination]);
        const std::uint32_t source_x = source % tiles_x;
        const std::uint32_t source_y = source / tiles_x;
        const std::int64_t displacement_x =
            static_cast<std::int64_t>(source_x) - destination_x;
        const std::int64_t displacement_y =
            static_cast<std::int64_t>(source_y) - destination_y;

        const auto has_same_displacement = [&](std::uint32_t next) {
            if (consumed[next] != 0 || source_by_destination[next] < 0) {
                return false;
            }
            const std::uint32_t next_source = static_cast<std::uint32_t>(
                source_by_destination[next]);
            const std::uint32_t next_destination_x = next % tiles_x;
            const std::uint32_t next_destination_y = next / tiles_x;
            return static_cast<std::int64_t>(next_source % tiles_x)
                    - next_destination_x == displacement_x
                && static_cast<std::int64_t>(next_source / tiles_x)
                    - next_destination_y == displacement_y;
        };

        std::uint32_t width = 1;
        while (destination_x + width < tiles_x) {
            const std::uint32_t next = destination + width;
            if (!has_same_displacement(next)) {
                break;
            }
            ++width;
        }

        std::uint32_t height = 1;
        while (destination_y + height < tiles_y) {
            bool same_row = true;
            const std::uint32_t row_start = destination + height * tiles_x;
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint32_t next = row_start + x;
                if (!has_same_displacement(next)) {
                    same_row = false;
                    break;
                }
            }
            if (!same_row) break;
            ++height;
        }

        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                consumed[destination + y * tiles_x + x] = 1;
            }
        }
        if (width * height < minimum_group_tiles) continue;
        groups.push_back({
            source_x,
            source_y,
            destination_x,
            destination_y,
            width,
            height});
    }
    return groups;
}

} // namespace

class GpuMoveInference::Impl final {
public:
    struct HashBuffer final {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
    };

    struct ReadbackSlot final {
        ComPtr<ID3D11Buffer> staging;
        ComPtr<ID3D11Query> completion;
        std::vector<std::uint32_t> candidates;
        std::uint64_t epoch = 0;
        std::uint64_t sequence = 0;
        std::uint64_t base_sequence = 0;
        std::uint64_t submission_token = 0;
        std::uint64_t serial = 0;
        bool pending = false;
        bool discard = false;
    };

    struct ActiveSubmission final {
        std::uint64_t token = 0;
        std::uint64_t epoch = 0;
        std::uint64_t sequence = 0;
        std::uint64_t base_sequence = 0;
        std::uint64_t prior_sequence = 0;
        std::uint32_t current_hash = 0;
        std::uint32_t previous_hash = 0;
        bool had_previous = false;
        bool active = false;
    };

    GpuError initialize(
        ID3D11Device* source_device,
        const GpuMoveInferenceConfig& source_config) {
        GpuMoveInferenceConfig normalized = source_config;
        if (normalized.texture_width == 0) {
            normalized.texture_width = normalized.width;
        }
        if (normalized.texture_height == 0) {
            normalized.texture_height = normalized.height;
        }
        if (source_device == nullptr
            || normalized.width == 0
            || normalized.height == 0
            || normalized.texture_width == 0
            || normalized.texture_height == 0
            || normalized.region_x > normalized.texture_width
            || normalized.region_y > normalized.texture_height
            || normalized.width > normalized.texture_width - normalized.region_x
            || normalized.height > normalized.texture_height - normalized.region_y
            || !supported_format(normalized.format)
            || normalized.max_candidate_tiles == 0
            || normalized.max_candidate_tiles > 4096
            || normalized.readback_slots == 0
            || normalized.readback_slots > 8
            || normalized.minimum_group_tiles == 0) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "invalid GPU move-inference configuration");
        }
        const std::uint64_t computed_tiles_x =
            (static_cast<std::uint64_t>(normalized.width)
                + gpu_move_tile_size - 1)
            / gpu_move_tile_size;
        const std::uint64_t computed_tiles_y =
            (static_cast<std::uint64_t>(normalized.height)
                + gpu_move_tile_size - 1)
            / gpu_move_tile_size;
        if (computed_tiles_x == 0
            || computed_tiles_y == 0
            || computed_tiles_x * computed_tiles_y
                > std::numeric_limits<std::uint32_t>::max()) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "GPU move-inference tile grid is too large");
        }

        device = source_device;
        device->GetImmediateContext(&context);
        if (context == nullptr) {
            return make_error(
                GpuStatus::system_error,
                E_NOINTERFACE,
                "D3D11 device has no immediate context");
        }
        config = normalized;
        tiles_x = static_cast<std::uint32_t>(computed_tiles_x);
        tiles_y = static_cast<std::uint32_t>(computed_tiles_y);
        tile_count = tiles_x * tiles_y;

        ComPtr<ID3DBlob> hash_bytecode;
        ComPtr<ID3DBlob> match_bytecode;
        HRESULT hr = compile_shader(
            hash_shader_source,
            sizeof(hash_shader_source) - 1,
            hash_bytecode);
        if (SUCCEEDED(hr)) {
            hr = compile_shader(
                match_shader_source,
                sizeof(match_shader_source) - 1,
                match_bytecode);
        }
        if (FAILED(hr)) {
            return make_error(
                GpuStatus::unsupported,
                hr,
                "DirectCompute move-inference shader compilation failed");
        }
        hr = device->CreateComputeShader(
            hash_bytecode->GetBufferPointer(),
            hash_bytecode->GetBufferSize(),
            nullptr,
            &hash_shader);
        if (SUCCEEDED(hr)) {
            hr = device->CreateComputeShader(
                match_bytecode->GetBufferPointer(),
                match_bytecode->GetBufferSize(),
                nullptr,
                &match_shader);
        }
        if (FAILED(hr)) {
            return make_error(
                GpuStatus::unsupported,
                hr,
                "DirectCompute move-inference shader creation failed");
        }

        for (HashBuffer& hashes : hash_buffers) {
            hr = create_structured_buffer(
                device.Get(),
                tile_count,
                sizeof(std::uint32_t) * 2,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                D3D11_USAGE_DEFAULT,
                0,
                hashes.buffer);
            if (SUCCEEDED(hr)) {
                hr = create_buffer_srv(
                    device.Get(), hashes.buffer.Get(), tile_count, hashes.srv);
            }
            if (SUCCEEDED(hr)) {
                hr = create_buffer_uav(
                    device.Get(), hashes.buffer.Get(), tile_count, hashes.uav);
            }
            if (FAILED(hr)) break;
        }
        if (SUCCEEDED(hr)) {
            hr = create_structured_buffer(
                device.Get(),
                config.max_candidate_tiles,
                sizeof(std::uint32_t),
                D3D11_BIND_SHADER_RESOURCE,
                D3D11_USAGE_DYNAMIC,
                D3D11_CPU_ACCESS_WRITE,
                candidate_buffer);
        }
        if (SUCCEEDED(hr)) {
            hr = create_buffer_srv(
                device.Get(),
                candidate_buffer.Get(),
                config.max_candidate_tiles,
                candidate_srv);
        }
        if (SUCCEEDED(hr)) {
            hr = create_structured_buffer(
                device.Get(),
                config.max_candidate_tiles,
                sizeof(MatchRecord),
                D3D11_BIND_UNORDERED_ACCESS,
                D3D11_USAGE_DEFAULT,
                0,
                match_buffer);
        }
        if (SUCCEEDED(hr)) {
            hr = create_buffer_uav(
                device.Get(),
                match_buffer.Get(),
                config.max_candidate_tiles,
                match_uav);
        }
        if (SUCCEEDED(hr)) {
            hr = create_constant_buffer(
                device.Get(), sizeof(HashConstants), hash_constants);
        }
        if (SUCCEEDED(hr)) {
            hr = create_constant_buffer(
                device.Get(), sizeof(MatchConstants), match_constants);
        }
        if (FAILED(hr)) {
            return make_error(
                hr == E_OUTOFMEMORY
                    ? GpuStatus::out_of_memory
                    : GpuStatus::unsupported,
                hr,
                "GPU move-inference resource creation failed");
        }

        readbacks.resize(config.readback_slots);
        for (ReadbackSlot& slot : readbacks) {
            hr = create_structured_buffer(
                device.Get(),
                config.max_candidate_tiles,
                sizeof(MatchRecord),
                0,
                D3D11_USAGE_STAGING,
                D3D11_CPU_ACCESS_READ,
                slot.staging);
            D3D11_QUERY_DESC query_description{};
            query_description.Query = D3D11_QUERY_EVENT;
            if (SUCCEEDED(hr)) {
                hr = device->CreateQuery(
                    &query_description, &slot.completion);
            }
            if (FAILED(hr)) {
                return make_error(
                    hr == E_OUTOFMEMORY
                        ? GpuStatus::out_of_memory
                        : GpuStatus::unsupported,
                    hr,
                    "GPU move-inference readback creation failed");
            }
            slot.candidates.reserve(config.max_candidate_tiles);
        }
        return success();
    }

    GpuError validate_texture(
        ID3D11Texture2D* texture,
        ComPtr<ID3D11ShaderResourceView>& srv) const {
        if (texture == nullptr) {
            return make_error(
                GpuStatus::invalid_argument,
                E_POINTER,
                "GPU move-inference texture is null");
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Width != config.texture_width
            || description.Height != config.texture_height
            || description.Format != config.format
            || description.MipLevels != 1
            || description.ArraySize != 1
            || description.SampleDesc.Count != 1
            || description.SampleDesc.Quality != 0
            || (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "GPU move-inference texture does not match its configuration");
        }
        ComPtr<ID3D11Device> texture_device;
        texture->GetDevice(&texture_device);
        if (!same_device(device.Get(), texture_device.Get())) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "GPU move-inference texture belongs to another device");
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_description{};
        srv_description.Format = config.format;
        srv_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_description.Texture2D.MostDetailedMip = 0;
        srv_description.Texture2D.MipLevels = 1;
        const HRESULT hr = device->CreateShaderResourceView(
            texture, &srv_description, &srv);
        if (FAILED(hr)) {
            return make_error(
                GpuStatus::unsupported,
                hr,
                "GPU move-inference texture cannot be sampled");
        }
        return success();
    }

    GpuError dispatch_hash(ID3D11ShaderResourceView* input_srv) {
        const HashConstants constants{
            config.width,
            config.height,
            tiles_x,
            tile_count,
            config.region_x,
            config.region_y,
            0,
            0};
        HRESULT hr = write_dynamic_buffer(
            context.Get(), hash_constants.Get(), &constants, 1);
        if (FAILED(hr)) {
            return d3d_error(
                device.Get(), hr, "GPU move-inference hash constants failed");
        }

        ID3D11Buffer* constant = hash_constants.Get();
        ID3D11UnorderedAccessView* output = hash_buffers[current_hash].uav.Get();
        context->CSSetShader(hash_shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &constant);
        context->CSSetShaderResources(0, 1, &input_srv);
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->Dispatch((tile_count + 63u) / 64u, 1, 1);
        ID3D11UnorderedAccessView* null_uav = nullptr;
        ID3D11ShaderResourceView* null_srv = nullptr;
        ID3D11Buffer* null_buffer = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        context->CSSetShaderResources(0, 1, &null_srv);
        context->CSSetConstantBuffers(0, 1, &null_buffer);
        context->CSSetShader(nullptr, nullptr, 0);
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            return make_error(
                GpuStatus::device_lost,
                removed,
                "device was removed during move hash submission");
        }
        return success();
    }

    GpuError dispatch_match(
        std::span<const std::uint32_t> candidates,
        ReadbackSlot& slot) {
        HRESULT hr = write_dynamic_buffer(
            context.Get(),
            candidate_buffer.Get(),
            candidates.data(),
            candidates.size());
        if (FAILED(hr)) {
            return d3d_error(
                device.Get(), hr, "GPU move-inference candidate upload failed");
        }
        const MatchConstants constants{
            tile_count,
            static_cast<std::uint32_t>(candidates.size()),
            0,
            0};
        hr = write_dynamic_buffer(
            context.Get(), match_constants.Get(), &constants, 1);
        if (FAILED(hr)) {
            return d3d_error(
                device.Get(), hr, "GPU move-inference match constants failed");
        }

        ID3D11Buffer* constant = match_constants.Get();
        std::array<ID3D11ShaderResourceView*, 3> inputs{
            hash_buffers[current_hash].srv.Get(),
            hash_buffers[previous_hash].srv.Get(),
            candidate_srv.Get()};
        ID3D11UnorderedAccessView* output = match_uav.Get();
        context->CSSetShader(match_shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &constant);
        context->CSSetShaderResources(
            0, static_cast<UINT>(inputs.size()), inputs.data());
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->Dispatch(
            (static_cast<std::uint32_t>(candidates.size()) + 63u) / 64u,
            1,
            1);

        ID3D11UnorderedAccessView* null_uav = nullptr;
        std::array<ID3D11ShaderResourceView*, 3> null_srvs{};
        ID3D11Buffer* null_buffer = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        context->CSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        context->CSSetConstantBuffers(0, 1, &null_buffer);
        context->CSSetShader(nullptr, nullptr, 0);
        context->CopyResource(slot.staging.Get(), match_buffer.Get());
        context->End(slot.completion.Get());
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            return make_error(
                GpuStatus::device_lost,
                removed,
                "device was removed during move matching submission");
        }
        return success();
    }

    GpuError submit(
        ID3D11Texture2D* texture,
        std::uint64_t sequence,
        std::span<const WgcRect> dirty_rects,
        GpuMoveInferenceSubmitInfo& output) {
        output = {};
        output.epoch = epoch_value;
        output.sequence = sequence;
        output.base_sequence = previous_sequence;
        if (active_submission.active) {
            return make_error(
                GpuStatus::invalid_argument,
                E_UNEXPECTED,
                "previous GPU move-inference submission is not finalized");
        }
        if (sequence == 0 || sequence <= last_sequence) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "GPU move-inference sequence must increase");
        }

        ComPtr<ID3D11ShaderResourceView> texture_srv;
        GpuError status = validate_texture(texture, texture_srv);
        if (!status) return status;
        bool overflow = false;
        std::vector<std::uint32_t> candidates = candidate_tiles(
            config, tiles_x, tiles_y, dirty_rects, overflow);
        output.candidate_count = static_cast<std::uint32_t>(candidates.size());

        status = dispatch_hash(texture_srv.Get());
        if (!status) return status;

        std::uint64_t token = next_submission_token + 1;
        if (token == 0) ++token;
        next_submission_token = token;

        if (!has_previous) {
            output.flags = gpu_move_submit_baseline;
        } else if (overflow) {
            output.flags = gpu_move_submit_fail_closed
                | gpu_move_submit_candidate_overflow;
        } else if (candidates.empty()) {
            output.flags = gpu_move_submit_no_candidates;
        } else {
            ReadbackSlot* slot = nullptr;
            for (ReadbackSlot& candidate : readbacks) {
                if (!candidate.pending) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot == nullptr) {
                output.flags = gpu_move_submit_fail_closed
                    | gpu_move_submit_no_readback_slot;
            } else {
                status = dispatch_match(candidates, *slot);
                if (!status) return status;
                slot->candidates = std::move(candidates);
                slot->epoch = epoch_value;
                slot->sequence = sequence;
                slot->base_sequence = previous_sequence;
                slot->submission_token = token;
                slot->serial = ++submission_serial;
                slot->pending = true;
                slot->discard = false;
                output.flags = gpu_move_submit_scheduled;
            }
        }

        active_submission = {
            token,
            epoch_value,
            sequence,
            previous_sequence,
            last_sequence,
            current_hash,
            previous_hash,
            has_previous,
            true};
        output.submission_token = token;
        std::swap(current_hash, previous_hash);
        previous_sequence = sequence;
        last_sequence = sequence;
        has_previous = true;
        return success();
    }

    bool submission_matches(
        const GpuMoveInferenceSubmitInfo& submission) const noexcept {
        return active_submission.active
            && submission.submission_token != 0
            && submission.submission_token == active_submission.token
            && submission.epoch == active_submission.epoch
            && submission.sequence == active_submission.sequence
            && submission.base_sequence == active_submission.base_sequence;
    }

    GpuError reconfigure_source(
        std::uint32_t texture_width,
        std::uint32_t texture_height,
        std::uint32_t region_x,
        std::uint32_t region_y) noexcept {
        if (texture_width == 0
            || texture_height == 0
            || region_x > texture_width
            || region_y > texture_height
            || config.width > texture_width - region_x
            || config.height > texture_height - region_y) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "invalid GPU move-inference source configuration");
        }
        if (active_submission.active) {
            return make_error(
                GpuStatus::invalid_argument,
                E_UNEXPECTED,
                "cannot reconfigure a provisional GPU move-inference submission");
        }
        if (config.texture_width == texture_width
            && config.texture_height == texture_height
            && config.region_x == region_x
            && config.region_y == region_y) {
            return success();
        }
        config.texture_width = texture_width;
        config.texture_height = texture_height;
        config.region_x = region_x;
        config.region_y = region_y;
        has_previous = false;
        previous_sequence = 0;
        return success();
    }

    bool commit_submission(
        const GpuMoveInferenceSubmitInfo& submission) noexcept {
        if (!submission_matches(submission)) return false;
        active_submission = {};
        return true;
    }

    bool cancel_submission(
        const GpuMoveInferenceSubmitInfo& submission) noexcept {
        if (!submission_matches(submission)) return false;
        for (ReadbackSlot& slot : readbacks) {
            if (slot.pending
                && slot.submission_token == active_submission.token) {
                slot.discard = true;
                break;
            }
        }
        current_hash = active_submission.current_hash;
        previous_hash = active_submission.previous_hash;
        previous_sequence = active_submission.base_sequence;
        last_sequence = active_submission.prior_sequence;
        has_previous = active_submission.had_previous;
        active_submission = {};
        return true;
    }

    GpuError try_resolve(
        GpuMoveInferenceResult& output,
        bool& ready) {
        output = {};
        ready = false;
        for (;;) {
            ReadbackSlot* oldest = nullptr;
            for (ReadbackSlot& slot : readbacks) {
                if (slot.pending
                    && (oldest == nullptr || slot.serial < oldest->serial)) {
                    oldest = &slot;
                }
            }
            if (oldest == nullptr) return success();
            if (active_submission.active
                && oldest->submission_token == active_submission.token) {
                return success();
            }

            const HRESULT query = context->GetData(
                oldest->completion.Get(),
                nullptr,
                0,
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (query == S_FALSE) return success();
            if (FAILED(query)) {
                return d3d_error(
                    device.Get(), query, "GPU move-inference query failed");
            }

            std::vector<TileMatch> matches;
            matches.reserve(oldest->candidates.size());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT mapped_status = context->Map(
                oldest->staging.Get(),
                0,
                D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT,
                &mapped);
            if (mapped_status == DXGI_ERROR_WAS_STILL_DRAWING) return success();
            if (FAILED(mapped_status)) {
                return d3d_error(
                    device.Get(),
                    mapped_status,
                    "GPU move-inference readback map failed");
            }
            const auto* records = static_cast<const MatchRecord*>(mapped.pData);
            for (std::size_t index = 0;
                 index < oldest->candidates.size();
                 ++index) {
                if (records[index].count == 1
                    && records[index].source < tile_count) {
                    matches.push_back({
                        records[index].source,
                        oldest->candidates[index]});
                }
            }
            context->Unmap(oldest->staging.Get(), 0);

            const bool discard = oldest->discard
                || oldest->epoch != epoch_value;
            const std::uint64_t result_epoch = oldest->epoch;
            const std::uint64_t result_sequence = oldest->sequence;
            const std::uint64_t result_base = oldest->base_sequence;
            oldest->candidates.clear();
            oldest->pending = false;
            oldest->discard = false;
            oldest->submission_token = 0;
            if (discard) continue;

            output.epoch = result_epoch;
            output.sequence = result_sequence;
            output.base_sequence = result_base;
            output.flags = gpu_move_result_valid
                | gpu_move_result_dirty_preserved;
            const std::vector<Group> groups = group_matches(
                tiles_x,
                tiles_y,
                matches,
                config.minimum_group_tiles);
            if (groups.size() > output.moves.size()) {
                output.flags |= gpu_move_result_fail_closed
                    | gpu_move_result_capacity_exceeded;
                ready = true;
                return success();
            }

            for (const Group& group : groups) {
                GpuInferredMove& move = output.moves[output.move_count++];
                move.rectangle.source_x = static_cast<std::int32_t>(
                    group.source_x * gpu_move_tile_size);
                move.rectangle.source_y = static_cast<std::int32_t>(
                    group.source_y * gpu_move_tile_size);
                move.rectangle.destination.x = static_cast<std::int32_t>(
                    group.destination_x * gpu_move_tile_size);
                move.rectangle.destination.y = static_cast<std::int32_t>(
                    group.destination_y * gpu_move_tile_size);
                move.rectangle.destination.width =
                    group.width_tiles * gpu_move_tile_size;
                move.rectangle.destination.height =
                    group.height_tiles * gpu_move_tile_size;
                move.tile_count = group.width_tiles * group.height_tiles;
                move.evidence_flags = gpu_move_evidence_dual_hash
                    | gpu_move_evidence_unique_previous
                    | gpu_move_evidence_complete_tiles
                    | gpu_move_evidence_equal_displacement
                    | gpu_move_evidence_dirty_preserved;
                move.confidence = move.tile_count > 1
                    ? GpuMoveConfidence::dual_hash_unique_grouped
                    : GpuMoveConfidence::dual_hash_unique;
            }
            if (output.move_count != 0) {
                output.flags |= gpu_move_result_inferred;
            }
            ready = true;
            return success();
        }
    }

    void reset_baseline() noexcept {
        std::lock_guard lock(mutex);
        if (active_submission.active) {
            GpuMoveInferenceSubmitInfo submission;
            submission.epoch = active_submission.epoch;
            submission.sequence = active_submission.sequence;
            submission.base_sequence = active_submission.base_sequence;
            submission.submission_token = active_submission.token;
            (void)cancel_submission(submission);
        }
        has_previous = false;
        previous_sequence = 0;
    }

    void reset_history() noexcept {
        std::lock_guard lock(mutex);
        ++epoch_value;
        if (epoch_value == 0) ++epoch_value;
        has_previous = false;
        previous_sequence = 0;
        last_sequence = 0;
        active_submission = {};
        for (ReadbackSlot& slot : readbacks) {
            if (slot.pending) slot.discard = true;
        }
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> hash_shader;
    ComPtr<ID3D11ComputeShader> match_shader;
    std::array<HashBuffer, 2> hash_buffers;
    ComPtr<ID3D11Buffer> candidate_buffer;
    ComPtr<ID3D11ShaderResourceView> candidate_srv;
    ComPtr<ID3D11Buffer> match_buffer;
    ComPtr<ID3D11UnorderedAccessView> match_uav;
    ComPtr<ID3D11Buffer> hash_constants;
    ComPtr<ID3D11Buffer> match_constants;
    std::vector<ReadbackSlot> readbacks;
    GpuMoveInferenceConfig config{};
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;
    std::uint32_t tile_count = 0;
    std::uint32_t current_hash = 0;
    std::uint32_t previous_hash = 1;
    std::uint64_t epoch_value = 1;
    std::uint64_t previous_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t submission_serial = 0;
    std::uint64_t next_submission_token = 0;
    ActiveSubmission active_submission{};
    bool has_previous = false;
    mutable std::mutex mutex;
};

GpuMoveInference::GpuMoveInference() noexcept = default;
GpuMoveInference::~GpuMoveInference() = default;
GpuMoveInference::GpuMoveInference(GpuMoveInference&&) noexcept = default;
GpuMoveInference& GpuMoveInference::operator=(GpuMoveInference&&) noexcept = default;

GpuError GpuMoveInference::create(
    ID3D11Device* device,
    const GpuMoveInferenceConfig& config,
    GpuMoveInference& output) noexcept {
    try {
        auto implementation = std::make_unique<Impl>();
        const GpuError initialized = implementation->initialize(device, config);
        if (!initialized) return initialized;
        output.impl_ = std::move(implementation);
        return success();
    } catch (const std::bad_alloc&) {
        return make_error(
            GpuStatus::out_of_memory,
            E_OUTOFMEMORY,
            "GPU move-inference allocation failed");
    } catch (...) {
        return make_error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown GPU move-inference initialization failure");
    }
}

GpuError GpuMoveInference::submit(
    ID3D11Texture2D* texture,
    std::uint64_t sequence,
    std::span<const WgcRect> dirty_rects,
    GpuMoveInferenceSubmitInfo& output) noexcept {
    if (!impl_) {
        return make_error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "GPU move-inference engine is uninitialized");
    }
    try {
        std::lock_guard lock(impl_->mutex);
        return impl_->submit(texture, sequence, dirty_rects, output);
    } catch (const std::bad_alloc&) {
        output = {};
        return make_error(
            GpuStatus::out_of_memory,
            E_OUTOFMEMORY,
            "GPU move-inference submission allocation failed");
    } catch (...) {
        output = {};
        return make_error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown GPU move-inference submission failure");
    }
}

GpuError GpuMoveInference::reconfigure_source(
    std::uint32_t texture_width,
    std::uint32_t texture_height,
    std::uint32_t region_x,
    std::uint32_t region_y) noexcept {
    if (!impl_) {
        return make_error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "GPU move-inference engine is uninitialized");
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->reconfigure_source(
        texture_width, texture_height, region_x, region_y);
}

bool GpuMoveInference::commit_submission(
    const GpuMoveInferenceSubmitInfo& submission) noexcept {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->commit_submission(submission);
}

bool GpuMoveInference::cancel_submission(
    const GpuMoveInferenceSubmitInfo& submission) noexcept {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->cancel_submission(submission);
}

GpuError GpuMoveInference::try_resolve(
    GpuMoveInferenceResult& output,
    bool& ready) noexcept {
    if (!impl_) {
        output = {};
        ready = false;
        return make_error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "GPU move-inference engine is uninitialized");
    }
    try {
        std::lock_guard lock(impl_->mutex);
        return impl_->try_resolve(output, ready);
    } catch (const std::bad_alloc&) {
        output = {};
        ready = false;
        return make_error(
            GpuStatus::out_of_memory,
            E_OUTOFMEMORY,
            "GPU move-inference result allocation failed");
    } catch (...) {
        output = {};
        ready = false;
        return make_error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown GPU move-inference result failure");
    }
}

void GpuMoveInference::reset_baseline() noexcept {
    if (impl_) impl_->reset_baseline();
}

void GpuMoveInference::reset_history() noexcept {
    if (impl_) impl_->reset_history();
}

GpuMoveInferenceConfig GpuMoveInference::config() const noexcept {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    return impl_->config;
}

std::uint64_t GpuMoveInference::epoch() const noexcept {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->epoch_value;
}

std::uint32_t GpuMoveInference::pending_count() const noexcept {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mutex);
    return static_cast<std::uint32_t>(std::count_if(
        impl_->readbacks.begin(),
        impl_->readbacks.end(),
        [](const Impl::ReadbackSlot& slot) { return slot.pending; }));
}

bool GpuMoveInference::initialized() const noexcept {
    return impl_ != nullptr;
}

} // namespace fluxcap::gpu::internal
