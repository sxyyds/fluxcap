#include "gpu_transform.hpp"

#include <d3d10.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;

GpuError error(GpuStatus status, HRESULT hr, const char* message) noexcept {
    GpuError value;
    value.status = status;
    value.hresult = hr;
    if (message != nullptr) {
        (void)strncpy_s(
            value.message.data(), value.message.size(), message, _TRUNCATE);
    }
    return value;
}

GpuError ok() noexcept { return error(GpuStatus::ok, S_OK, "ok"); }

GpuError resource_error(
    ID3D11Device* device,
    HRESULT hr,
    GpuStatus fallback,
    const char* message) noexcept {
    const HRESULT removed = device == nullptr
        ? S_OK
        : device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        return error(GpuStatus::device_lost, removed, message);
    }
    if (hr == E_OUTOFMEMORY) {
        return error(GpuStatus::out_of_memory, hr, message);
    }
    return error(fallback, hr, message);
}

DXGI_FORMAT output_format(GpuPixelFormat format) noexcept {
    switch (format) {
    case GpuPixelFormat::bgra8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case GpuPixelFormat::nv12: return DXGI_FORMAT_NV12;
    case GpuPixelFormat::p010: return DXGI_FORMAT_P010;
    case GpuPixelFormat::rgba16_float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_COLOR_SPACE_TYPE resolved_output_color_space(
    const GpuTransformConfig& config) noexcept {
    if (config.output_color_space != DXGI_COLOR_SPACE_CUSTOM) {
        return config.output_color_space;
    }
    switch (config.output_format) {
    case GpuPixelFormat::bgra8:
        return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    case GpuPixelFormat::rgba16_float:
        return config.input_color_space;
    case GpuPixelFormat::p010:
        if (config.input_color_space
            == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        }
        [[fallthrough]];
    case GpuPixelFormat::nv12:
        return config.full_range_yuv
            ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
            : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    default:
        return DXGI_COLOR_SPACE_CUSTOM;
    }
}

bool same_device(ID3D11Device* left, ID3D11Device* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> a;
    ComPtr<IUnknown> b;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b)))
        && a.Get() == b.Get();
}

bool legacy_color_space_contract(
    const GpuTransformConfig& config,
    DXGI_FORMAT destination_format,
    DXGI_COLOR_SPACE_TYPE destination_color_space) noexcept {
    if (config.input_format != DXGI_FORMAT_B8G8R8A8_UNORM
        || config.input_color_space
            != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        return false;
    }
    if (destination_format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        return destination_color_space
            == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
    if (destination_format == DXGI_FORMAT_NV12
        || destination_format == DXGI_FORMAT_P010) {
        return destination_color_space
                == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
            || destination_color_space
                == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
    }
    return false;
}

bool valid_backend(GpuTransformBackend backend) noexcept {
    return backend == GpuTransformBackend::automatic
        || backend == GpuTransformBackend::video_processor
        || backend == GpuTransformBackend::deterministic_planar;
}

bool deterministic_planar_contract(
    const GpuTransformConfig& config,
    DXGI_COLOR_SPACE_TYPE output_color_space) noexcept {
    // A fully typed SRGB texture cannot be reinterpreted through an UNORM SRV
    // on all D3D11 drivers. Keep the deterministic contract to nonlinear P709
    // data in an explicitly UNORM resource; automatic uses VP otherwise.
    const bool bgra_input =
        config.input_format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool planar_output = config.output_format == GpuPixelFormat::nv12
        || config.output_format == GpuPixelFormat::p010;
    const bool p709_output = output_color_space
            == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
        || output_color_space
            == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
    return bgra_input
        && planar_output
        && p709_output
        && config.input_color_space
            == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

HRESULT compile_shader(
    const char* source,
    const char* entry,
    const char* target,
    ComPtr<ID3DBlob>& bytecode) noexcept {
    ComPtr<ID3DBlob> diagnostics;
    return D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &bytecode,
        &diagnostics);
}

// Both shaders use the same integer center-point tap. UV is a deterministic
// 2x2 box over the four output-luma samples belonging to that chroma sample.
// The CPU damage mapper mirrors tap_coordinate() exactly.
constexpr char deterministic_planar_shader[] = R"hlsl(
Texture2D<float4> source_texture : register(t0);

cbuffer TransformConstants : register(b0) {
    uint4 source_region;   // x, y, width, height
    uint4 output_contract; // width, height, is_p010, full_range
};

uint tap_coordinate(
    uint output_coordinate,
    uint source_extent,
    uint output_extent) {
    uint numerator = (output_coordinate * 2u + 1u) * source_extent;
    return min(source_extent - 1u, numerator / (output_extent * 2u));
}

float3 load_output_rgb(uint2 output_coordinate) {
    uint2 source_coordinate = uint2(
        tap_coordinate(
            output_coordinate.x, source_region.z, output_contract.x),
        tap_coordinate(
            output_coordinate.y, source_region.w, output_contract.y));
    return saturate(source_texture.Load(int3(
        int2(source_region.xy + source_coordinate), 0)).rgb);
}

float3 rgb_to_ycbcr(float3 rgb) {
    float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;
    return float3(y, cb, cr);
}

float rounded(float value) {
    return floor(value + 0.5);
}

float encode_luma(float y) {
    bool p010 = output_contract.z != 0u;
    bool full_range = output_contract.w != 0u;
    float code = p010
        ? (full_range
            ? rounded(saturate(y) * 1023.0)
            : rounded(64.0 + saturate(y) * 876.0))
        : (full_range
            ? rounded(saturate(y) * 255.0)
            : rounded(16.0 + saturate(y) * 219.0));
    return p010 ? (code * 64.0) / 65535.0 : code / 255.0;
}

float encode_chroma(float chroma) {
    bool p010 = output_contract.z != 0u;
    bool full_range = output_contract.w != 0u;
    chroma = clamp(chroma, -0.5, 0.5);
    float code = p010
        ? (full_range
            ? rounded((chroma + 0.5) * 1023.0)
            : rounded(512.0 + chroma * 896.0))
        : (full_range
            ? rounded((chroma + 0.5) * 255.0)
            : rounded(128.0 + chroma * 224.0));
    return p010 ? (code * 64.0) / 65535.0 : code / 255.0;
}

float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    float2 corner = float2(
        (vertex_id << 1u) & 2u,
        vertex_id & 2u);
    return float4(
        corner.x * 2.0 - 1.0,
        1.0 - corner.y * 2.0,
        0.0,
        1.0);
}

float ps_y(float4 position : SV_Position) : SV_Target {
    uint2 output_coordinate = uint2(position.xy);
    return encode_luma(rgb_to_ycbcr(
        load_output_rgb(output_coordinate)).x);
}

float2 ps_uv(float4 position : SV_Position) : SV_Target {
    uint2 base = uint2(position.xy) * 2u;
    float3 ycbcr =
        rgb_to_ycbcr(load_output_rgb(base))
        + rgb_to_ycbcr(load_output_rgb(base + uint2(1u, 0u)))
        + rgb_to_ycbcr(load_output_rgb(base + uint2(0u, 1u)))
        + rgb_to_ycbcr(load_output_rgb(base + uint2(1u, 1u)));
    ycbcr *= 0.25;
    return float2(encode_chroma(ycbcr.y), encode_chroma(ycbcr.z));
}
)hlsl";

struct DeterministicConstants final {
    std::uint32_t source_x = 0;
    std::uint32_t source_y = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t p010 = 0;
    std::uint32_t full_range = 0;
};

static_assert(sizeof(DeterministicConstants) == 32);
constexpr std::size_t maximum_cached_transform_views = 32;

} // namespace

class GpuTransform::Impl final {
public:
    struct CachedVideoInputView final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorInputView> view;
    };

    struct CachedVideoOutputView final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorOutputView> view;
    };

    struct CachedShaderInputView final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
    };

    struct CachedPlaneViews final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView1> y;
        ComPtr<ID3D11RenderTargetView1> uv;
    };

    class ContextStateGuard final {
    public:
        ContextStateGuard(
            ID3D11DeviceContext1* source_context,
            ID3DDeviceContextState* replacement) noexcept
            : context(source_context) {
            context->SwapDeviceContextState(replacement, &previous);
        }
        ~ContextStateGuard() {
            if (context != nullptr && previous != nullptr) {
                context->SwapDeviceContextState(previous.Get(), nullptr);
            }
        }
        ContextStateGuard(const ContextStateGuard&) = delete;
        ContextStateGuard& operator=(const ContextStateGuard&) = delete;
    private:
        ID3D11DeviceContext1* context = nullptr;
        ComPtr<ID3DDeviceContextState> previous;
    };

    class MultithreadGuard final {
    public:
        explicit MultithreadGuard(ID3D10Multithread* source) noexcept
            : multithread(source) {
            if (multithread != nullptr) multithread->Enter();
        }
        ~MultithreadGuard() {
            if (multithread != nullptr) multithread->Leave();
        }
        MultithreadGuard(const MultithreadGuard&) = delete;
        MultithreadGuard& operator=(const MultithreadGuard&) = delete;
    private:
        ID3D10Multithread* multithread = nullptr;
    };

    GpuError initialize(
        ID3D11Device* source_device,
        const GpuTransformConfig& source_config) {
        const bool full_input_region = source_config.input_region_width == 0
            && source_config.input_region_height == 0;
        const std::uint32_t source_region_width = full_input_region
            ? source_config.input_width
            : source_config.input_region_width;
        const std::uint32_t source_region_height = full_input_region
            ? source_config.input_height
            : source_config.input_region_height;
        const DXGI_FORMAT destination_format =
            output_format(source_config.output_format);
        if (source_device == nullptr
            || source_config.input_width == 0
            || source_config.input_height == 0
            || source_config.output_width == 0
            || source_config.output_height == 0
            || source_config.frame_rate_numerator == 0
            || source_config.frame_rate_denominator == 0
            || source_config.input_format == DXGI_FORMAT_UNKNOWN
            || destination_format == DXGI_FORMAT_UNKNOWN
            || source_config.input_color_space == DXGI_COLOR_SPACE_CUSTOM
            || source_config.input_color_space == DXGI_COLOR_SPACE_RESERVED
            || source_config.output_color_space == DXGI_COLOR_SPACE_RESERVED
            || !valid_backend(source_config.backend)) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "invalid transform configuration");
        }
        if ((!full_input_region
                && (source_config.input_region_width == 0
                    || source_config.input_region_height == 0))
            || source_config.input_region_x >= source_config.input_width
            || source_config.input_region_y >= source_config.input_height
            || source_region_width > source_config.input_width
                - source_config.input_region_x
            || source_region_height > source_config.input_height
                - source_config.input_region_y) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "transform input region is out of bounds");
        }
        if ((source_config.output_format == GpuPixelFormat::nv12
                || source_config.output_format == GpuPixelFormat::p010)
            && ((source_config.output_width & 1u) != 0
                || (source_config.output_height & 1u) != 0)) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "NV12/P010 dimensions must be even");
        }

        device = source_device;
        device->GetImmediateContext(&context);
        if (context == nullptr) {
            return error(
                GpuStatus::system_error,
                E_NOINTERFACE,
                "D3D11 device has no immediate context");
        }
        config = source_config;
        effective_output_color_space = resolved_output_color_space(config);
        if (effective_output_color_space == DXGI_COLOR_SPACE_CUSTOM
            || effective_output_color_space == DXGI_COLOR_SPACE_RESERVED) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "transform output color space is unspecified");
        }

        if (!config.external_output_only) {
            D3D11_TEXTURE2D_DESC texture_desc{};
            texture_desc.Width = config.output_width;
            texture_desc.Height = config.output_height;
            texture_desc.MipLevels = 1;
            texture_desc.ArraySize = 1;
            texture_desc.Format = destination_format;
            texture_desc.SampleDesc.Count = 1;
            texture_desc.Usage = D3D11_USAGE_DEFAULT;
            texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (destination_format == DXGI_FORMAT_B8G8R8A8_UNORM
                || destination_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                texture_desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            }
            const HRESULT hr = device->CreateTexture2D(
                &texture_desc, nullptr, &output);
            if (FAILED(hr)) {
                return resource_error(
                    device.Get(),
                    hr,
                    GpuStatus::unsupported,
                    "transform output texture creation failed");
            }
        }

        const bool deterministic_requested =
            config.backend != GpuTransformBackend::video_processor;
        GpuError deterministic_result = error(
            GpuStatus::unsupported,
            E_NOINTERFACE,
            "configuration is outside the deterministic planar contract");
        if (deterministic_requested
            && deterministic_planar_contract(
                config, effective_output_color_space)) {
            deterministic_result = initialize_deterministic();
        }
        if (config.backend == GpuTransformBackend::deterministic_planar) {
            if (!deterministic_result) return deterministic_result;
            selected_backend.store(
                GpuTransformBackend::deterministic_planar,
                std::memory_order_release);
            return ok();
        }
        if (!deterministic_result
            && deterministic_result.status != GpuStatus::unsupported) {
            return deterministic_result;
        }

        const GpuError video_result = initialize_video_processor();
        if (video_result) {
            selected_backend.store(
                deterministic_available
                    ? GpuTransformBackend::automatic
                    : GpuTransformBackend::video_processor,
                std::memory_order_release);
            return ok();
        }
        if (config.backend == GpuTransformBackend::automatic
            && deterministic_available
            && video_result.status == GpuStatus::unsupported) {
            selected_backend.store(
                GpuTransformBackend::automatic,
                std::memory_order_release);
            return ok();
        }
        return video_result;
    }

    GpuError initialize_deterministic() {
        ComPtr<ID3D11Device1> device1;
        HRESULT hr = device.As(&device1);
        if (SUCCEEDED(hr)) hr = device.As(&deterministic_device);
        if (SUCCEEDED(hr)) hr = context.As(&deterministic_context);
        if (SUCCEEDED(hr)) hr = context.As(&deterministic_multithread);
        if (FAILED(hr)) {
            reset_deterministic();
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "D3D11.3 plane render-target views are unavailable");
        }
        (void)deterministic_multithread->SetMultithreadProtected(TRUE);

        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> y_bytecode;
        ComPtr<ID3DBlob> uv_bytecode;
        hr = compile_shader(
            deterministic_planar_shader,
            "vs_main",
            "vs_5_0",
            vertex_bytecode);
        if (SUCCEEDED(hr)) {
            hr = compile_shader(
                deterministic_planar_shader,
                "ps_y",
                "ps_5_0",
                y_bytecode);
        }
        if (SUCCEEDED(hr)) {
            hr = compile_shader(
                deterministic_planar_shader,
                "ps_uv",
                "ps_5_0",
                uv_bytecode);
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreateVertexShader(
                vertex_bytecode->GetBufferPointer(),
                vertex_bytecode->GetBufferSize(),
                nullptr,
                &deterministic_vertex_shader);
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreatePixelShader(
                y_bytecode->GetBufferPointer(),
                y_bytecode->GetBufferSize(),
                nullptr,
                &deterministic_y_shader);
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreatePixelShader(
                uv_bytecode->GetBufferPointer(),
                uv_bytecode->GetBufferSize(),
                nullptr,
                &deterministic_uv_shader);
        }
        if (FAILED(hr)) {
            reset_deterministic();
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "deterministic planar shader creation failed");
        }

        D3D11_BUFFER_DESC constants_desc{};
        constants_desc.ByteWidth = sizeof(DeterministicConstants);
        constants_desc.Usage = D3D11_USAGE_DYNAMIC;
        constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(
            &constants_desc, nullptr, &deterministic_constants);
        if (SUCCEEDED(hr)) {
            const D3D_FEATURE_LEVEL requested = device->GetFeatureLevel();
            D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_9_1;
            hr = device1->CreateDeviceContextState(
                0,
                &requested,
                1,
                D3D11_SDK_VERSION,
                __uuidof(ID3D11Device),
                &chosen,
                &deterministic_state);
        }
        if (FAILED(hr)) {
            reset_deterministic();
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "deterministic planar pipeline state creation failed");
        }

        if (output != nullptr) {
            ID3D11RenderTargetView1* y = nullptr;
            ID3D11RenderTargetView1* uv = nullptr;
            const GpuError views = plane_views(output.Get(), y, uv);
            if (!views) {
                reset_deterministic();
                return views;
            }
        }
        deterministic_available = true;
        return ok();
    }

    void reset_deterministic() noexcept {
        deterministic_available = false;
        shader_input_views.clear();
        shader_output_views.clear();
        deterministic_state.Reset();
        deterministic_constants.Reset();
        deterministic_uv_shader.Reset();
        deterministic_y_shader.Reset();
        deterministic_vertex_shader.Reset();
        deterministic_multithread.Reset();
        deterministic_context.Reset();
        deterministic_device.Reset();
    }

    GpuError initialize_video_processor() {
        HRESULT hr = device.As(&video_device);
        if (SUCCEEDED(hr)) hr = context.As(&video_context);
        if (FAILED(hr)) {
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "D3D11 video processing is unavailable");
        }
        (void)context.As(&video_context1);

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {
            config.frame_rate_numerator,
            config.frame_rate_denominator};
        content.InputWidth = config.input_width;
        content.InputHeight = config.input_height;
        content.OutputFrameRate = content.InputFrameRate;
        content.OutputWidth = config.output_width;
        content.OutputHeight = config.output_height;
        content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        hr = video_device->CreateVideoProcessorEnumerator(
            &content, &enumerator);
        if (FAILED(hr)) {
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "video processor enumerator creation failed");
        }

        UINT input_support = 0;
        UINT output_support = 0;
        const DXGI_FORMAT destination_format = output_format(
            config.output_format);
        hr = enumerator->CheckVideoProcessorFormat(
            config.input_format, &input_support);
        if (SUCCEEDED(hr)) {
            hr = enumerator->CheckVideoProcessorFormat(
                destination_format, &output_support);
        }
        if (FAILED(hr)) {
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "video processor format capability query failed");
        }
        if ((input_support
                & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0
            || (output_support
                & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
            return error(
                GpuStatus::unsupported,
                E_NOINTERFACE,
                "requested video processor format conversion is unsupported");
        }

        ComPtr<ID3D11VideoProcessorEnumerator1> enumerator1;
        const HRESULT enumerator1_result = enumerator.As(&enumerator1);
        if (SUCCEEDED(enumerator1_result)) {
            BOOL conversion_supported = FALSE;
            hr = enumerator1->CheckVideoProcessorFormatConversion(
                config.input_format,
                config.input_color_space,
                destination_format,
                effective_output_color_space,
                &conversion_supported);
            if (FAILED(hr)) {
                return resource_error(
                    device.Get(),
                    hr,
                    GpuStatus::unsupported,
                    "video processor format/color-space capability query failed");
            }
            if (conversion_supported == FALSE) {
                return error(
                    GpuStatus::unsupported,
                    E_NOINTERFACE,
                    "requested video processor format/color-space conversion is unsupported");
            }
        } else if (!legacy_color_space_contract(
                config, destination_format, effective_output_color_space)) {
            return error(
                GpuStatus::unsupported,
                enumerator1_result,
                "color-space-aware conversion capability probing is unavailable");
        }
        if (video_context1 == nullptr
            && !legacy_color_space_contract(
                config, destination_format, effective_output_color_space)) {
            return error(
                GpuStatus::unsupported,
                E_NOINTERFACE,
                "color-space-aware conversion requires ID3D11VideoContext1");
        }

        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        hr = enumerator->GetVideoProcessorCaps(&caps);
        if (FAILED(hr)) {
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "video processor capability query failed");
        }
        if (caps.RateConversionCapsCount == 0) {
            return error(
                GpuStatus::unsupported,
                E_NOINTERFACE,
                "video processor exposes no rate-conversion capability");
        }
        hr = video_device->CreateVideoProcessor(
            enumerator.Get(), 0, &processor);
        if (FAILED(hr)) {
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "video processor creation failed");
        }

        if (output != nullptr) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
            output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            output_desc.Texture2D.MipSlice = 0;
            hr = video_device->CreateVideoProcessorOutputView(
                output.Get(), enumerator.Get(), &output_desc, &output_view);
            if (FAILED(hr)) {
                return resource_error(
                    device.Get(),
                    hr,
                    GpuStatus::unsupported,
                    "video processor output view creation failed");
            }
        }
        video_available = true;
        return ok();
    }

    GpuError validate_resources(
        ID3D11Texture2D* input,
        ID3D11Texture2D* destination,
        D3D11_TEXTURE2D_DESC& input_desc,
        D3D11_TEXTURE2D_DESC& destination_desc) const {
        if (input == nullptr || destination == nullptr) {
            return error(
                GpuStatus::invalid_argument,
                E_POINTER,
                "source/destination texture is null or transform is uninitialized");
        }
        if (input == destination) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "transform source and destination must not alias");
        }

        input->GetDesc(&input_desc);
        if (input_desc.Width != config.input_width
            || input_desc.Height != config.input_height
            || (input_desc.Format != config.input_format
                && !(config.input_format == DXGI_FORMAT_B8G8R8A8_UNORM
                    && input_desc.Format
                        == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
            || input_desc.MipLevels != 1
            || input_desc.ArraySize != 1
            || input_desc.SampleDesc.Count != 1
            || input_desc.SampleDesc.Quality != 0) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "input texture does not match transform configuration");
        }
        ComPtr<ID3D11Device> input_device;
        input->GetDevice(&input_device);
        if (!same_device(device.Get(), input_device.Get())) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "input texture belongs to a different D3D11 device");
        }

        destination->GetDesc(&destination_desc);
        ComPtr<ID3D11Device> destination_device;
        destination->GetDevice(&destination_device);
        if (!same_device(device.Get(), destination_device.Get())) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "destination texture belongs to a different D3D11 device");
        }
        if (destination_desc.Width != config.output_width
            || destination_desc.Height != config.output_height
            || destination_desc.Format != output_format(config.output_format)
            || destination_desc.MipLevels != 1
            || destination_desc.ArraySize != 1
            || destination_desc.SampleDesc.Count != 1
            || destination_desc.SampleDesc.Quality != 0
            || destination_desc.Usage != D3D11_USAGE_DEFAULT
            || destination_desc.CPUAccessFlags != 0
            || (destination_desc.BindFlags
                & D3D11_BIND_RENDER_TARGET) == 0) {
            return error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "destination texture does not match transform configuration");
        }
        return ok();
    }

    GpuError process(ID3D11Texture2D* input) {
        if (output == nullptr) {
            return error(
                GpuStatus::invalid_argument,
                E_UNEXPECTED,
                "process() is unavailable for an external-output-only transform");
        }
        return process_into(input, output.Get());
    }

    GpuError process_into(
        ID3D11Texture2D* input,
        ID3D11Texture2D* destination) {
        std::lock_guard lock(mutex);
        D3D11_TEXTURE2D_DESC input_desc{};
        D3D11_TEXTURE2D_DESC destination_desc{};
        const GpuError validated = validate_resources(
            input, destination, input_desc, destination_desc);
        if (!validated) return validated;

        const GpuTransformBackend selected = selected_backend.load(
            std::memory_order_acquire);
        if (selected == GpuTransformBackend::deterministic_planar) {
            return process_deterministic(input, destination, input_desc);
        }
        if (selected == GpuTransformBackend::video_processor) {
            return process_video(input, destination);
        }

        if (deterministic_available) {
            const GpuError deterministic = process_deterministic(
                input, destination, input_desc);
            if (deterministic) {
                selected_backend.store(
                    GpuTransformBackend::deterministic_planar,
                    std::memory_order_release);
                return deterministic;
            }
            if (deterministic.status != GpuStatus::unsupported
                || !video_available) {
                return deterministic;
            }
        }
        if (!video_available) {
            return error(
                GpuStatus::unsupported,
                E_NOINTERFACE,
                "no transform backend accepts this source/destination pair");
        }
        const GpuError video = process_video(input, destination);
        if (video) {
            selected_backend.store(
                GpuTransformBackend::video_processor,
                std::memory_order_release);
        }
        return video;
    }

    GpuError shader_input_view(
        ID3D11Texture2D* input,
        const D3D11_TEXTURE2D_DESC& description,
        ID3D11ShaderResourceView*& result) {
        result = nullptr;
        for (auto& cached : shader_input_views) {
            if (cached.texture.Get() == input) {
                result = cached.view.Get();
                return ok();
            }
        }
        if ((description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
            return error(
                GpuStatus::unsupported,
                E_NOINTERFACE,
                "deterministic planar source has no shader-resource bind");
        }
        CachedShaderInputView cached;
        cached.texture = input;
        D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
        // YUV coefficients consume nonlinear P709 values, so an sRGB-tagged
        // BGRA resource is deliberately viewed through the UNORM member.
        view_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MostDetailedMip = 0;
        view_desc.Texture2D.MipLevels = 1;
        const HRESULT hr = device->CreateShaderResourceView(
            input, &view_desc, &cached.view);
        if (FAILED(hr)) {
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "deterministic planar source SRV creation failed");
        }
        result = cached.view.Get();
        if (shader_input_views.size() >= maximum_cached_transform_views) {
            shader_input_views.erase(shader_input_views.begin());
        }
        shader_input_views.push_back(std::move(cached));
        return ok();
    }

    GpuError plane_views(
        ID3D11Texture2D* destination,
        ID3D11RenderTargetView1*& y,
        ID3D11RenderTargetView1*& uv) {
        y = nullptr;
        uv = nullptr;
        for (auto& cached : shader_output_views) {
            if (cached.texture.Get() == destination) {
                y = cached.y.Get();
                uv = cached.uv.Get();
                return ok();
            }
        }
        if (deterministic_device == nullptr) {
            return error(
                GpuStatus::unsupported,
                E_NOINTERFACE,
                "D3D11.3 plane render-target views are unavailable");
        }

        CachedPlaneViews cached;
        cached.texture = destination;
        const bool p010 = config.output_format == GpuPixelFormat::p010;
        D3D11_RENDER_TARGET_VIEW_DESC1 view_desc{};
        view_desc.Format = p010
            ? DXGI_FORMAT_R16_UNORM
            : DXGI_FORMAT_R8_UNORM;
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
        view_desc.Texture2D.PlaneSlice = 0;
        HRESULT hr = deterministic_device->CreateRenderTargetView1(
            destination, &view_desc, &cached.y);
        if (SUCCEEDED(hr)) {
            view_desc.Format = p010
                ? DXGI_FORMAT_R16G16_UNORM
                : DXGI_FORMAT_R8G8_UNORM;
            view_desc.Texture2D.PlaneSlice = 1;
            hr = deterministic_device->CreateRenderTargetView1(
                destination, &view_desc, &cached.uv);
        }
        if (FAILED(hr)) {
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::unsupported,
                "planar destination plane RTV creation failed");
        }
        y = cached.y.Get();
        uv = cached.uv.Get();
        if (shader_output_views.size() >= maximum_cached_transform_views) {
            shader_output_views.erase(shader_output_views.begin());
        }
        shader_output_views.push_back(std::move(cached));
        return ok();
    }

    GpuError process_deterministic(
        ID3D11Texture2D* input,
        ID3D11Texture2D* destination,
        const D3D11_TEXTURE2D_DESC& input_desc) {
        ID3D11ShaderResourceView* source_view = nullptr;
        GpuError view_result = shader_input_view(
            input, input_desc, source_view);
        if (!view_result) return view_result;

        ID3D11RenderTargetView1* y_view = nullptr;
        ID3D11RenderTargetView1* uv_view = nullptr;
        view_result = plane_views(destination, y_view, uv_view);
        if (!view_result) return view_result;

        const bool full_input_region = config.input_region_width == 0
            && config.input_region_height == 0;
        const DeterministicConstants constants{
            config.input_region_x,
            config.input_region_y,
            full_input_region
                ? config.input_width
                : config.input_region_width,
            full_input_region
                ? config.input_height
                : config.input_region_height,
            config.output_width,
            config.output_height,
            config.output_format == GpuPixelFormat::p010 ? 1u : 0u,
            effective_output_color_space
                    == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                ? 1u : 0u};
        {
            MultithreadGuard serialized(deterministic_multithread.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = context->Map(
                deterministic_constants.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped);
            if (FAILED(hr)) {
                return resource_error(
                    device.Get(),
                    hr,
                    GpuStatus::system_error,
                    "deterministic planar constants upload failed");
            }
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(deterministic_constants.Get(), 0);

            ContextStateGuard state(
                deterministic_context.Get(), deterministic_state.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(
                deterministic_vertex_shader.Get(), nullptr, 0);
            ID3D11Buffer* constant_buffer = deterministic_constants.Get();
            context->PSSetConstantBuffers(0, 1, &constant_buffer);
            context->PSSetShaderResources(0, 1, &source_view);

            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(config.output_width);
            viewport.Height = static_cast<float>(config.output_height);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            context->RSSetViewports(1, &viewport);
            ID3D11RenderTargetView* y_target = y_view;
            context->OMSetRenderTargets(1, &y_target, nullptr);
            context->PSSetShader(
                deterministic_y_shader.Get(), nullptr, 0);
            context->Draw(3, 0);

            viewport.Width = static_cast<float>(config.output_width / 2u);
            viewport.Height = static_cast<float>(config.output_height / 2u);
            context->RSSetViewports(1, &viewport);
            ID3D11RenderTargetView* uv_target = uv_view;
            context->OMSetRenderTargets(1, &uv_target, nullptr);
            context->PSSetShader(
                deterministic_uv_shader.Get(), nullptr, 0);
            context->Draw(3, 0);
        }

        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            return error(
                GpuStatus::device_lost,
                removed,
                "device was removed during deterministic planar draw");
        }
        generation.fetch_add(1, std::memory_order_release);
        return ok();
    }

    GpuError process_video(
        ID3D11Texture2D* input,
        ID3D11Texture2D* destination) {
        ID3D11Texture2D* processor_input = input;
        D3D11_TEXTURE2D_DESC input_desc{};
        input->GetDesc(&input_desc);
        if (config.input_format == DXGI_FORMAT_B8G8R8A8_UNORM
            && input_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            if (video_normalized_input == nullptr) {
                D3D11_TEXTURE2D_DESC normalized_desc = input_desc;
                normalized_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                normalized_desc.BindFlags = 0;
                normalized_desc.CPUAccessFlags = 0;
                normalized_desc.MiscFlags = 0;
                const HRESULT created = device->CreateTexture2D(
                    &normalized_desc, nullptr, &video_normalized_input);
                if (FAILED(created)) {
                    return resource_error(
                        device.Get(),
                        created,
                        GpuStatus::unsupported,
                        "typed-SRGB VideoProcessor normalization texture creation failed");
                }
            }
            context->CopyResource(video_normalized_input.Get(), input);
            const HRESULT removed = device->GetDeviceRemovedReason();
            if (FAILED(removed)) {
                return error(
                    GpuStatus::device_lost,
                    removed,
                    "device was removed during typed-SRGB normalization copy");
            }
            compatibility_copies.fetch_add(1, std::memory_order_relaxed);
            processor_input = video_normalized_input.Get();
        }

        ID3D11VideoProcessorInputView* input_view = nullptr;
        for (auto& cached : video_input_views) {
            if (cached.texture.Get() == processor_input) {
                input_view = cached.view.Get();
                break;
            }
        }
        if (input_view == nullptr) {
            CachedVideoInputView cached;
            cached.texture = processor_input;
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view_desc{};
            view_desc.FourCC = 0;
            view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            view_desc.Texture2D.MipSlice = 0;
            view_desc.Texture2D.ArraySlice = 0;
            const HRESULT hr = video_device->CreateVideoProcessorInputView(
                processor_input, enumerator.Get(), &view_desc, &cached.view);
            if (FAILED(hr)) {
                return resource_error(
                    device.Get(),
                    hr,
                    GpuStatus::unsupported,
                    "video processor input view creation failed");
            }
            input_view = cached.view.Get();
            if (video_input_views.size() >= maximum_cached_transform_views) {
                video_input_views.erase(video_input_views.begin());
            }
            video_input_views.push_back(std::move(cached));
        }

        ID3D11VideoProcessorOutputView* destination_view = nullptr;
        if (destination == output.Get()) {
            destination_view = output_view.Get();
        } else {
            for (auto& cached : video_output_views) {
                if (cached.texture.Get() == destination) {
                    destination_view = cached.view.Get();
                    break;
                }
            }
            if (destination_view == nullptr) {
                CachedVideoOutputView cached;
                cached.texture = destination;
                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
                view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                view_desc.Texture2D.MipSlice = 0;
                const HRESULT hr =
                    video_device->CreateVideoProcessorOutputView(
                        destination,
                        enumerator.Get(),
                        &view_desc,
                        &cached.view);
                if (FAILED(hr)) {
                    return resource_error(
                        device.Get(),
                        hr,
                        GpuStatus::unsupported,
                        "video processor destination output view creation failed");
                }
                destination_view = cached.view.Get();
                if (video_output_views.size()
                    >= maximum_cached_transform_views) {
                    video_output_views.erase(video_output_views.begin());
                }
                video_output_views.push_back(std::move(cached));
            }
        }

        const bool full_input_region = config.input_region_width == 0
            && config.input_region_height == 0;
        const std::uint32_t source_width = full_input_region
            ? config.input_width
            : config.input_region_width;
        const std::uint32_t source_height = full_input_region
            ? config.input_height
            : config.input_region_height;
        const RECT source_rect{
            static_cast<LONG>(config.input_region_x),
            static_cast<LONG>(config.input_region_y),
            static_cast<LONG>(config.input_region_x + source_width),
            static_cast<LONG>(config.input_region_y + source_height)};
        const RECT destination_rect{
            0,
            0,
            static_cast<LONG>(config.output_width),
            static_cast<LONG>(config.output_height)};
        video_context->VideoProcessorSetStreamSourceRect(
            processor.Get(), 0, TRUE, &source_rect);
        video_context->VideoProcessorSetStreamDestRect(
            processor.Get(), 0, TRUE, &destination_rect);
        video_context->VideoProcessorSetOutputTargetRect(
            processor.Get(), TRUE, &destination_rect);
        video_context->VideoProcessorSetStreamFrameFormat(
            processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        // This disables adaptive enhancement, but scaling/filter support still
        // remains driver-defined and therefore is not a deterministic damage
        // contract.
        video_context->VideoProcessorSetStreamAutoProcessingMode(
            processor.Get(), 0, FALSE);

        if (video_context1 != nullptr) {
            video_context1->VideoProcessorSetStreamColorSpace1(
                processor.Get(), 0, config.input_color_space);
            video_context1->VideoProcessorSetOutputColorSpace1(
                processor.Get(), effective_output_color_space);
        } else {
            if (!legacy_color_space_contract(
                    config,
                    output_format(config.output_format),
                    effective_output_color_space)) {
                return error(
                    GpuStatus::unsupported,
                    E_NOINTERFACE,
                    "HDR/color-space-aware conversion requires ID3D11VideoContext1");
            }
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space{};
            input_space.RGB_Range = 0;
            input_space.YCbCr_Matrix = 1;
            video_context->VideoProcessorSetStreamColorSpace(
                processor.Get(), 0, &input_space);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_space{};
            output_space.YCbCr_Matrix = 1;
            output_space.Nominal_Range = effective_output_color_space
                    == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            video_context->VideoProcessorSetOutputColorSpace(
                processor.Get(), &output_space);
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view;
        const HRESULT hr = video_context->VideoProcessorBlt(
            processor.Get(), destination_view, 0, 1, &stream);
        if (FAILED(hr)) {
            return resource_error(
                device.Get(),
                hr,
                GpuStatus::system_error,
                "VideoProcessorBlt failed");
        }
        generation.fetch_add(1, std::memory_order_release);
        return ok();
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> output;

    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoContext1> video_context1;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    ComPtr<ID3D11Texture2D> video_normalized_input;
    std::vector<CachedVideoInputView> video_input_views;
    std::vector<CachedVideoOutputView> video_output_views;
    bool video_available = false;

    ComPtr<ID3D11Device3> deterministic_device;
    ComPtr<ID3D11DeviceContext1> deterministic_context;
    ComPtr<ID3D10Multithread> deterministic_multithread;
    ComPtr<ID3D11VertexShader> deterministic_vertex_shader;
    ComPtr<ID3D11PixelShader> deterministic_y_shader;
    ComPtr<ID3D11PixelShader> deterministic_uv_shader;
    ComPtr<ID3D11Buffer> deterministic_constants;
    ComPtr<ID3DDeviceContextState> deterministic_state;
    std::vector<CachedShaderInputView> shader_input_views;
    std::vector<CachedPlaneViews> shader_output_views;
    bool deterministic_available = false;

    GpuTransformConfig config{};
    DXGI_COLOR_SPACE_TYPE effective_output_color_space =
        DXGI_COLOR_SPACE_CUSTOM;
    std::atomic<GpuTransformBackend> selected_backend{
        GpuTransformBackend::automatic};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> compatibility_copies{0};
    std::mutex mutex;
};

GpuTransform::GpuTransform() noexcept = default;
GpuTransform::~GpuTransform() = default;
GpuTransform::GpuTransform(GpuTransform&&) noexcept = default;
GpuTransform& GpuTransform::operator=(GpuTransform&&) noexcept = default;

GpuError GpuTransform::create(
    ID3D11Device* device,
    const GpuTransformConfig& config,
    GpuTransform& output) noexcept {
    try {
        auto implementation = std::make_unique<Impl>();
        const GpuError initialized = implementation->initialize(device, config);
        if (!initialized) return initialized;
        output.impl_ = std::move(implementation);
        return ok();
    } catch (const std::bad_alloc&) {
        return error(
            GpuStatus::out_of_memory,
            E_OUTOFMEMORY,
            "memory allocation failed");
    } catch (...) {
        return error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown transform failure");
    }
}

GpuError GpuTransform::process(ID3D11Texture2D* input) noexcept {
    if (!impl_) {
        return error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "transform is uninitialized");
    }
    try {
        return impl_->process(input);
    } catch (const std::bad_alloc&) {
        return error(
            GpuStatus::out_of_memory,
            E_OUTOFMEMORY,
            "transform view cache allocation failed");
    } catch (...) {
        return error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown transform failure");
    }
}

GpuError GpuTransform::process_into(
    ID3D11Texture2D* source,
    ID3D11Texture2D* destination) noexcept {
    if (!impl_) {
        return error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "transform is uninitialized");
    }
    try {
        return impl_->process_into(source, destination);
    } catch (const std::bad_alloc&) {
        return error(
            GpuStatus::out_of_memory,
            E_OUTOFMEMORY,
            "transform view cache allocation failed");
    } catch (...) {
        return error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown transform failure");
    }
}

ID3D11Texture2D* GpuTransform::output_texture() const noexcept {
    return impl_ ? impl_->output.Get() : nullptr;
}

std::uint64_t GpuTransform::generation() const noexcept {
    return impl_
        ? impl_->generation.load(std::memory_order_acquire)
        : 0;
}

std::uint64_t GpuTransform::compatibility_copy_submissions() const noexcept {
    return impl_
        ? impl_->compatibility_copies.load(std::memory_order_relaxed)
        : 0;
}

GpuTransformConfig GpuTransform::config() const noexcept {
    return impl_ ? impl_->config : GpuTransformConfig{};
}

GpuTransformBackend GpuTransform::active_backend() const noexcept {
    return impl_
        ? impl_->selected_backend.load(std::memory_order_acquire)
        : GpuTransformBackend::automatic;
}

bool GpuTransform::spatially_deterministic() const noexcept {
    return active_backend() == GpuTransformBackend::deterministic_planar;
}

bool GpuTransform::initialized() const noexcept { return impl_ != nullptr; }

} // namespace fluxcap::gpu
