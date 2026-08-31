#include "gpu_encoder.hpp"
#include "gpu_device_failure.hpp"

#include <strmif.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;

GpuEncoderResult result(
    GpuEncoderStatus status,
    HRESULT hr = S_OK,
    std::string message = {}) noexcept {
    GpuEncoderResult value;
    value.status = status;
    value.hresult = hr;
    try {
        value.message = message.empty() ? gpu_encoder_status_string(status) : std::move(message);
    } catch (...) {
    }
    return value;
}

GpuEncoderResult device_aware_result(
    ID3D11Device* device,
    GpuEncoderStatus fallback,
    HRESULT hresult,
    std::string message = {}) noexcept {
    const auto classification = internal::classify_device_failure(
        device, hresult);
    return result(
        classification.device_lost
            ? GpuEncoderStatus::device_lost
            : fallback,
        classification.hresult,
        std::move(message));
}

GpuEncoderResult exception_result() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return result(GpuEncoderStatus::out_of_memory, E_OUTOFMEMORY);
    } catch (const std::exception& error) {
        return result(GpuEncoderStatus::media_foundation_error, E_FAIL, error.what());
    } catch (...) {
        return result(GpuEncoderStatus::media_foundation_error, E_FAIL);
    }
}

const GUID& codec_subtype(VideoCodec codec) noexcept {
    switch (codec) {
    case VideoCodec::h264: return MFVideoFormat_H264;
    case VideoCodec::hevc: return MFVideoFormat_HEVC;
    case VideoCodec::av1: return MFVideoFormat_AV1;
    default: return GUID_NULL;
    }
}

const GUID& input_subtype(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_NV12: return MFVideoFormat_NV12;
    case DXGI_FORMAT_P010: return MFVideoFormat_P010;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return MFVideoFormat_ARGB32;
    default: return GUID_NULL;
    }
}

struct EncoderColorDescription final {
    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_CUSTOM;
    MFVideoPrimaries primaries = MFVideoPrimaries_Unknown;
    MFVideoTransferFunction transfer = MFVideoTransFunc_Unknown;
    MFVideoTransferMatrix matrix = MFVideoTransferMatrix_Unknown;
    MFNominalRange range = MFNominalRange_Unknown;
    MFVideoChromaSubsampling chroma = MFVideoChromaSubsampling_Unknown;
};

DXGI_COLOR_SPACE_TYPE resolved_encoder_color_space(
    const GpuEncoderConfig& config) noexcept {
    if (config.input_color_space != DXGI_COLOR_SPACE_CUSTOM) {
        return config.input_color_space;
    }
    return config.input_format == DXGI_FORMAT_B8G8R8A8_UNORM
        ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
        : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
}

bool encoder_color_description(
    const GpuEncoderConfig& config,
    EncoderColorDescription& output) noexcept {
    output = {};
    output.color_space = resolved_encoder_color_space(config);
    const bool rgb = config.input_format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool yuv = config.input_format == DXGI_FORMAT_NV12
        || config.input_format == DXGI_FORMAT_P010;
    switch (output.color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        if (!rgb) return false;
        output.primaries = MFVideoPrimaries_BT709;
        output.transfer = MFVideoTransFunc_22;
        output.matrix = MFVideoTransferMatrix_Identity;
        output.range = MFNominalRange_0_255;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        if (!rgb) return false;
        output.primaries = MFVideoPrimaries_BT709;
        output.transfer = MFVideoTransFunc_10;
        output.matrix = MFVideoTransferMatrix_Identity;
        output.range = MFNominalRange_0_255;
        break;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709:
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709:
        if (!yuv) return false;
        output.primaries = MFVideoPrimaries_BT709;
        output.transfer = MFVideoTransFunc_709;
        output.matrix = MFVideoTransferMatrix_BT709;
        output.range = output.color_space
                == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
            ? MFNominalRange_0_255
            : MFNominalRange_16_235;
        output.chroma = MFVideoChromaSubsampling_MPEG2;
        break;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020:
        if (!yuv) return false;
        output.primaries = MFVideoPrimaries_BT2020;
        output.transfer = MFVideoTransFunc_2020;
        output.matrix = MFVideoTransferMatrix_BT2020_10;
        output.range = output.color_space
                == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
            ? MFNominalRange_0_255
            : MFNominalRange_16_235;
        output.chroma = MFVideoChromaSubsampling_MPEG2;
        break;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
        if (config.input_format != DXGI_FORMAT_P010) return false;
        output.primaries = MFVideoPrimaries_BT2020;
        output.transfer = MFVideoTransFunc_2084;
        output.matrix = MFVideoTransferMatrix_BT2020_10;
        output.range = MFNominalRange_16_235;
        output.chroma = MFVideoChromaSubsampling_MPEG2;
        break;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        if (config.input_format != DXGI_FORMAT_P010) return false;
        output.primaries = MFVideoPrimaries_BT2020;
        output.transfer = MFVideoTransFunc_2084;
        output.matrix = MFVideoTransferMatrix_BT2020_10;
        output.range = MFNominalRange_16_235;
        output.chroma = MFVideoChromaSubsampling_Cosited;
        break;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        if (config.input_format != DXGI_FORMAT_P010) return false;
        output.primaries = MFVideoPrimaries_BT2020;
        output.transfer = MFVideoTransFunc_HLG;
        output.matrix = MFVideoTransferMatrix_BT2020_10;
        output.range = output.color_space
                == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020
            ? MFNominalRange_0_255
            : MFNominalRange_16_235;
        output.chroma = MFVideoChromaSubsampling_Cosited;
        break;
    default:
        return false;
    }
    return true;
}

bool encoder_output_color_description(
    const GpuEncoderConfig& config,
    EncoderColorDescription& output) noexcept {
    if (!encoder_color_description(config, output)) return false;
    if (config.input_format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        // The input media type is RGB, but H.264/HEVC/AV1 hardware encoders
        // produce 4:2:0 video. Do not leak the RGB identity matrix into VUI.
        output.matrix = MFVideoTransferMatrix_BT709;
        output.chroma = MFVideoChromaSubsampling_MPEG2;
    }
    return true;
}

bool valid_hdr10_static_metadata(const GpuEncoderConfig& config) noexcept {
    const auto& metadata = config.hdr10;
    if (!metadata.enabled) return true;
    if (config.input_format != DXGI_FORMAT_P010
        || (config.codec != VideoCodec::hevc
            && config.codec != VideoCodec::av1)
        || resolved_encoder_color_space(config)
            != DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020) {
        return false;
    }
    constexpr std::uint16_t coordinate_limit = 50'000;
    const std::uint16_t coordinates[] = {
        metadata.red_primary_x,
        metadata.red_primary_y,
        metadata.green_primary_x,
        metadata.green_primary_y,
        metadata.blue_primary_x,
        metadata.blue_primary_y,
        metadata.white_point_x,
        metadata.white_point_y};
    for (const std::uint16_t coordinate : coordinates) {
        if (coordinate > coordinate_limit) return false;
    }
    return metadata.max_mastering_luminance != 0
        && metadata.min_mastering_luminance
            <= static_cast<std::uint64_t>(
                metadata.max_mastering_luminance) * 10'000u
        && metadata.max_content_light_level != 0
        && metadata.max_frame_average_light_level != 0
        && metadata.max_frame_average_light_level
            <= metadata.max_content_light_level;
}

bool valid_bitstream_validation(const GpuEncoderConfig& config) noexcept {
    if (config.require_bitstream_hdr10_metadata
        && !config.hdr10.enabled) {
        return false;
    }
    if (config.require_video_encoder_input_bind
        && config.input_format != DXGI_FORMAT_NV12
        && config.input_format != DXGI_FORMAT_P010) {
        return false;
    }
    return true;
}

HRESULT set_hdr10_media_type_attributes(
    IMFMediaType* type,
    const GpuEncoderConfig& config) noexcept {
    if (!config.hdr10.enabled) return S_OK;
    const auto& metadata = config.hdr10;
    MT_CUSTOM_VIDEO_PRIMARIES primaries{};
    constexpr float scale = 1.0f / 50'000.0f;
    primaries.fRx = metadata.red_primary_x * scale;
    primaries.fRy = metadata.red_primary_y * scale;
    primaries.fGx = metadata.green_primary_x * scale;
    primaries.fGy = metadata.green_primary_y * scale;
    primaries.fBx = metadata.blue_primary_x * scale;
    primaries.fBy = metadata.blue_primary_y * scale;
    primaries.fWx = metadata.white_point_x * scale;
    primaries.fWy = metadata.white_point_y * scale;
    HRESULT hr = type->SetBlob(
        MF_MT_CUSTOM_VIDEO_PRIMARIES,
        reinterpret_cast<const UINT8*>(&primaries),
        sizeof(primaries));
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_MAX_MASTERING_LUMINANCE,
            metadata.max_mastering_luminance);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_MIN_MASTERING_LUMINANCE,
            metadata.min_mastering_luminance);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_MAX_LUMINANCE_LEVEL,
            metadata.max_content_light_level);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL,
            metadata.max_frame_average_light_level);
    }
    return hr;
}

std::uint32_t resolved_encoder_profile(
    const GpuEncoderConfig& config) noexcept {
    if (config.profile != 0) return config.profile;
    const bool main10 = config.input_format == DXGI_FORMAT_P010;
    switch (config.codec) {
    case VideoCodec::h264:
        return main10
            ? eAVEncH264VProfile_High10
            : eAVEncH264VProfile_Main;
    case VideoCodec::hevc:
        return main10
            ? eAVEncH265VProfile_Main_420_10
            : eAVEncH265VProfile_Main_420_8;
    case VideoCodec::av1:
        return main10
            ? eAVEncAV1VProfile_Main_420_10
            : eAVEncAV1VProfile_Main_420_8;
    default:
        return 0;
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

bool video_encoder_input_bind_supported(
    ID3D11Device* device,
    const GpuEncoderConfig& config) noexcept {
    if (device == nullptr
        || (config.input_format != DXGI_FORMAT_NV12
            && config.input_format != DXGI_FORMAT_P010)) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = config.width;
    description.Height = config.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = config.input_format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET
        | D3D11_BIND_VIDEO_ENCODER;
    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(
        &description, nullptr, &texture));
}

bool same_com_object(IUnknown* left, IUnknown* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> a;
    ComPtr<IUnknown> b;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b)))
        && a.Get() == b.Get();
}

HRESULT verify_dxgi_surface_identity(
    IMFMediaBuffer* buffer,
    ID3D11Texture2D* texture,
    std::uint32_t subresource) noexcept {
    if (buffer == nullptr || texture == nullptr) return E_POINTER;
    ComPtr<IMFDXGIBuffer> dxgi_buffer;
    HRESULT hr = buffer->QueryInterface(IID_PPV_ARGS(&dxgi_buffer));
    ComPtr<ID3D11Texture2D> wrapped;
    if (SUCCEEDED(hr)) {
        hr = dxgi_buffer->GetResource(IID_PPV_ARGS(&wrapped));
    }
    UINT wrapped_subresource = 0;
    if (SUCCEEDED(hr)) {
        hr = dxgi_buffer->GetSubresourceIndex(&wrapped_subresource);
    }
    if (SUCCEEDED(hr)
        && (!same_com_object(texture, wrapped.Get())
            || wrapped_subresource != subresource)) {
        hr = E_UNEXPECTED;
    }
    return hr;
}

bool subresource_dimensions_match(
    const D3D11_TEXTURE2D_DESC& description,
    std::uint32_t subresource,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    if (description.MipLevels == 0 || description.ArraySize == 0) return false;
    const std::uint64_t count =
        static_cast<std::uint64_t>(description.MipLevels)
        * description.ArraySize;
    if (subresource >= count) return false;
    const std::uint32_t mip = subresource % description.MipLevels;
    const std::uint32_t subresource_width = std::max(1u, description.Width >> mip);
    const std::uint32_t subresource_height = std::max(1u, description.Height >> mip);
    return subresource_width == width && subresource_height == height;
}

HRESULT set_media_type_common(
    IMFMediaType* type,
    const GUID& subtype,
    const GpuEncoderConfig& config,
    bool compressed_output) noexcept {
    EncoderColorDescription color;
    if (!(compressed_output
            ? encoder_output_color_description(config, color)
            : encoder_color_description(config, color))) {
        return E_INVALIDARG;
    }
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, config.width, config.height);
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(
            type,
            MF_MT_FRAME_RATE,
            config.frame_rate_numerator,
            config.frame_rate_denominator);
    }
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, color.primaries);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_TRANSFER_FUNCTION, color.transfer);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_YUV_MATRIX, color.matrix);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, color.range);
    if (SUCCEEDED(hr) && color.chroma != MFVideoChromaSubsampling_Unknown) {
        hr = type->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, color.chroma);
    }
    if (SUCCEEDED(hr)) {
        hr = set_hdr10_media_type_attributes(type, config);
    }
    const std::uint32_t profile = resolved_encoder_profile(config);
    if (SUCCEEDED(hr) && compressed_output && profile != 0) {
        hr = type->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
    }
    return hr;
}

void set_codec_uint32(ICodecAPI* api, const GUID& key, std::uint32_t value) noexcept {
    if (api == nullptr) return;
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;
    (void)api->SetValue(&key, &variant);
    VariantClear(&variant);
}

void set_codec_bool(ICodecAPI* api, const GUID& key, bool value) noexcept {
    if (api == nullptr) return;
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    (void)api->SetValue(&key, &variant);
    VariantClear(&variant);
}

struct ActivationArray final {
    IMFActivate** values = nullptr;
    UINT32 count = 0;
    ~ActivationArray() {
        if (values != nullptr) {
            for (UINT32 index = 0; index < count; ++index) {
                if (values[index] != nullptr) values[index]->Release();
            }
            CoTaskMemFree(values);
        }
    }
};

GpuEncoderMftIdentity activation_identity(IMFActivate* activation) noexcept {
    GpuEncoderMftIdentity output;
    if (activation == nullptr) return output;
    (void)activation->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &output.clsid);

    wchar_t* friendly_name = nullptr;
    UINT32 characters = 0;
    if (SUCCEEDED(activation->GetAllocatedString(
            MFT_FRIENDLY_NAME_Attribute, &friendly_name, &characters))
        && friendly_name != nullptr) {
        // MFT friendly names are short. If a vendor returns an unusually long
        // UTF-16 string, reduce the source length until a complete UTF-8 prefix
        // fits; never split a multibyte sequence or lose NUL termination.
        int source_characters = static_cast<int>(std::min<UINT32>(
            characters, static_cast<UINT32>(std::numeric_limits<int>::max())));
        while (source_characters > 0) {
            const int converted = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                friendly_name,
                source_characters,
                output.friendly_name.data(),
                static_cast<int>(output.friendly_name.size() - 1u),
                nullptr,
                nullptr);
            if (converted > 0) {
                output.friendly_name[static_cast<std::size_t>(converted)] = '\0';
                break;
            }
            --source_characters;
        }
    }
    CoTaskMemFree(friendly_name);
    return output;
}

HRESULT enumerate_encoders(
    const GpuEncoderConfig& config,
    ActivationArray& output) noexcept {
    MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, input_subtype(config.input_format)};
    MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, codec_subtype(config.codec)};
    return MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input_info,
        &output_info,
        &output.values,
        &output.count);
}

void wait_for_encoder_event(bool low_latency, std::uint32_t& misses) noexcept {
    ++misses;
    if (!low_latency) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    if (misses <= 256) {
        YieldProcessor();
        return;
    }
    // The asynchronous pipeline owns a dedicated worker, so yielding here
    // removes the fixed 1 ms latency step without blocking capture threads.
    SwitchToThread();
}

struct MfLifetime final {
    ~MfLifetime() { (void)MFShutdown(); }
};

struct ExternalInputState final {
    void add() noexcept {
        std::lock_guard lock(mutex);
        ++in_flight;
    }

    void remove(bool tracked_callback) noexcept {
        {
            std::lock_guard lock(mutex);
            if (in_flight != 0) --in_flight;
            if (tracked_callback) ++release_callbacks;
        }
        condition.notify_all();
    }

    std::uint64_t callback_count() const noexcept {
        std::lock_guard lock(mutex);
        return release_callbacks;
    }

    bool wait_until_idle(std::chrono::steady_clock::time_point deadline) noexcept {
        std::unique_lock lock(mutex);
        return condition.wait_until(lock, deadline, [this] { return in_flight == 0; });
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::size_t in_flight = 0;
    std::uint64_t release_callbacks = 0;
};

// A one-shot tracked-sample allocator. It deliberately does not retain the
// sample: the MFT owns the last sample reference, while this callback owns the
// caller's bus/texture lifetime token until Media Foundation releases it.
class ExternalInputLifetime final : public IMFAsyncCallback {
public:
    ExternalInputLifetime(
        std::shared_ptr<void> token,
        std::shared_ptr<ExternalInputState> state,
        std::shared_ptr<MfLifetime> mf_lifetime) noexcept
        : token_(std::move(token)),
          state_(std::move(state)),
          mf_lifetime_(std::move(mf_lifetime)) {
        state_->add();
    }

    void cancel() noexcept { complete(false); }

    STDMETHODIMP QueryInterface(REFIID iid, void** object) noexcept override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *object = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() noexcept override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() noexcept override {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }

    STDMETHODIMP Invoke(IMFAsyncResult*) noexcept override {
        complete(true);
        return S_OK;
    }

private:
    ~ExternalInputLifetime() { complete(false); }

    void complete(bool tracked_callback) noexcept {
        bool expected = false;
        if (!completed_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }
        token_.reset();
        state_->remove(tracked_callback);
    }

    std::atomic<ULONG> references_{1};
    std::atomic<bool> completed_{false};
    std::shared_ptr<void> token_;
    std::shared_ptr<ExternalInputState> state_;
    // MFShutdown must not run while an MFT-owned tracked sample can callback.
    std::shared_ptr<MfLifetime> mf_lifetime_;
};

// The transform can retain an input sample after it asks for another one. A
// tracked sample is the only authoritative signal that its texture is reusable.
class TrackedInputPool final : public IMFAsyncCallback {
public:
    struct Lease final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IMFSample> sample;
    };

    HRESULT initialize(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        DXGI_FORMAT format,
        std::uint32_t slot_count,
        bool require_video_encoder_bind,
        std::shared_ptr<MfLifetime> lifetime) {
        lifetime_ = std::move(lifetime);
        slots_.clear();
        slots_.reserve(slot_count);

        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        // The asynchronous pipeline writes VideoProcessor output directly
        // into these tracked surfaces before handing them to the MFT.
        const bool planar = format == DXGI_FORMAT_NV12
            || format == DXGI_FORMAT_P010;
        description.BindFlags = D3D11_BIND_RENDER_TARGET
            | (planar ? D3D11_BIND_VIDEO_ENCODER : 0u);

        for (std::uint32_t index = 0; index < slot_count; ++index) {
            Slot slot;
            HRESULT hr = device->CreateTexture2D(&description, nullptr, &slot.texture);
            if (FAILED(hr) && index == 0 && planar
                && !require_video_encoder_bind) {
                // Prefer an encoder-input resource even for the compatibility
                // mode. Older drivers can still use the legacy surface, with
                // the documented MFT-internal-copy boundary remaining unknown.
                description.BindFlags = D3D11_BIND_RENDER_TARGET;
                hr = device->CreateTexture2D(
                    &description, nullptr, &slot.texture);
            }
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFTrackedSample> tracked;
            if (SUCCEEDED(hr)) {
                hr = MFCreateDXGISurfaceBuffer(
                    __uuidof(ID3D11Texture2D),
                    slot.texture.Get(),
                    0,
                    FALSE,
                    &buffer);
            }
            if (SUCCEEDED(hr)) hr = MFCreateTrackedSample(&tracked);
            if (SUCCEEDED(hr)) hr = tracked->QueryInterface(IID_PPV_ARGS(&slot.sample));
            if (SUCCEEDED(hr)) hr = slot.sample->AddBuffer(buffer.Get());
            if (SUCCEEDED(hr)) hr = slot.sample->SetUINT32(slot_attribute(), index);
            if (FAILED(hr)) return hr;
            slots_.push_back(std::move(slot));
        }
        return S_OK;
    }

    HRESULT try_acquire(Lease& lease) noexcept {
        lease = {};
        std::lock_guard lock(mutex_);
        if (shutting_down_) return MF_E_SHUTDOWN;

        for (std::size_t offset = 0; offset < slots_.size(); ++offset) {
            const std::size_t index = (next_slot_ + offset) % slots_.size();
            Slot& slot = slots_[index];
            if (slot.in_flight || slot.sample == nullptr) continue;

            ComPtr<IMFTrackedSample> tracked;
            HRESULT hr = slot.sample.As(&tracked);
            if (SUCCEEDED(hr)) hr = tracked->SetAllocator(this, nullptr);
            if (hr == MF_E_NOTACCEPTING) {
                // Invoke can publish the sample just before the tracked sample
                // clears its old allocator. It becomes armable after Invoke returns.
                continue;
            }
            if (FAILED(hr)) return hr;

            slot.in_flight = true;
            ++in_flight_count_;
            next_slot_ = (index + 1) % slots_.size();
            lease.texture = slot.texture;
            lease.sample = std::move(slot.sample);
            return S_OK;
        }
        return S_FALSE;
    }

    void wait_for_change_until(std::chrono::steady_clock::time_point deadline) noexcept {
        std::unique_lock lock(mutex_);
        const auto poll_deadline = std::min(
            deadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
        condition_.wait_until(lock, poll_deadline);
    }

    bool wait_until_idle(std::chrono::steady_clock::time_point deadline) noexcept {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(lock, deadline, [this] { return in_flight_count_ == 0; });
    }

    void begin_shutdown() noexcept {
        std::lock_guard lock(mutex_);
        shutting_down_ = true;
        for (Slot& slot : slots_) slot.sample.Reset();
        condition_.notify_all();
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** object) noexcept override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *object = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() noexcept override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() noexcept override {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }

    STDMETHODIMP Invoke(IMFAsyncResult* async_result) noexcept override {
        if (async_result == nullptr) return E_POINTER;
        ComPtr<IUnknown> object;
        HRESULT hr = async_result->GetObject(&object);
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(hr)) hr = object.As(&sample);
        UINT32 index = 0;
        if (SUCCEEDED(hr)) hr = sample->GetUINT32(slot_attribute(), &index);
        if (FAILED(hr)) return hr;

        {
            std::lock_guard lock(mutex_);
            if (index >= slots_.size() || !slots_[index].in_flight) return E_UNEXPECTED;
            Slot& slot = slots_[index];
            slot.in_flight = false;
            --in_flight_count_;
            if (!shutting_down_) slot.sample = std::move(sample);
        }
        condition_.notify_all();
        return S_OK;
    }

private:
    struct Slot final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IMFSample> sample;
        bool in_flight = false;
    };

    static const GUID& slot_attribute() noexcept {
        static constexpr GUID value{
            0xd70d1989, 0xa054, 0x462e, {0x81, 0x41, 0x17, 0x44, 0xc1, 0xdf, 0x2b, 0xb7}};
        return value;
    }

    ~TrackedInputPool() = default;

    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Slot> slots_;
    std::size_t next_slot_ = 0;
    std::size_t in_flight_count_ = 0;
    bool shutting_down_ = false;
    std::shared_ptr<MfLifetime> lifetime_;
};

} // namespace

struct GpuEncoderInputLeaseState final {
    ComPtr<TrackedInputPool> pool;
    TrackedInputPool::Lease lease;
};

class GpuEncoder::Impl final {
public:
    ~Impl() { close(); }

    GpuEncoderResult initialize(
        ID3D11Device* source_device,
        const GpuEncoderConfig& source_config,
        EncodedPacketCallback source_callback,
        void* source_context) {
        EncoderColorDescription color;
        if (source_device == nullptr || source_callback == nullptr
            || source_config.width == 0 || source_config.height == 0
            || source_config.frame_rate_numerator == 0
            || source_config.frame_rate_denominator == 0
            || source_config.bitrate == 0 || source_config.input_pool_size == 0
            || source_config.input_pool_size > 32
            || IsEqualGUID(input_subtype(source_config.input_format), GUID_NULL)
            || IsEqualGUID(codec_subtype(source_config.codec), GUID_NULL)
            || !encoder_color_description(source_config, color)
            || !valid_hdr10_static_metadata(source_config)
            || !valid_bitstream_validation(source_config)) {
            return result(GpuEncoderStatus::invalid_argument, E_INVALIDARG);
        }
        if ((source_config.input_format == DXGI_FORMAT_NV12
                || source_config.input_format == DXGI_FORMAT_P010)
            && ((source_config.width & 1u) != 0 || (source_config.height & 1u) != 0)) {
            return result(
                GpuEncoderStatus::invalid_argument,
                E_INVALIDARG,
                "NV12/P010 encoder dimensions must be even");
        }

        const GpuEncoderResult initial_device_state = device_aware_result(
            source_device, GpuEncoderStatus::ok, S_OK);
        if (!initial_device_state) return initial_device_state;

        close();
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            return device_aware_result(
                source_device,
                GpuEncoderStatus::media_foundation_error,
                hr,
                "MFStartup failed");
        }
        try {
            mf_lifetime = std::make_shared<MfLifetime>();
        } catch (...) {
            (void)MFShutdown();
            throw;
        }
        try {
            external_inputs = std::make_shared<ExternalInputState>();
        } catch (...) {
            mf_lifetime.reset();
            throw;
        }
        device = source_device;
        device->GetImmediateContext(&context);
        config = source_config;
        callback = source_callback;
        callback_context = source_context;

        hr = MFCreateDXGIDeviceManager(&device_manager_token, &device_manager);
        if (SUCCEEDED(hr)) hr = device_manager->ResetDevice(device.Get(), device_manager_token);
        if (FAILED(hr)) {
            const auto failure = device_aware_result(
                device.Get(),
                GpuEncoderStatus::d3d_error,
                hr,
                "DXGI device manager creation failed");
            close();
            return failure;
        }

        ActivationArray activations;
        hr = enumerate_encoders(config, activations);
        if (FAILED(hr) || activations.count == 0) {
            const auto failure = FAILED(hr)
                ? device_aware_result(
                    device.Get(),
                    GpuEncoderStatus::not_supported,
                    hr,
                    "hardware encoder enumeration failed")
                : result(
                    GpuEncoderStatus::not_supported,
                    MF_E_TOPO_CODEC_NOT_FOUND,
                    "no matching hardware encoder MFT was found");
            close();
            return failure;
        }

        GpuEncoderResult selected = result(GpuEncoderStatus::not_supported, MF_E_TOPO_CODEC_NOT_FOUND);
        for (UINT32 index = 0; index < activations.count; ++index) {
            ComPtr<IMFTransform> candidate;
            hr = activations.values[index]->ActivateObject(IID_PPV_ARGS(&candidate));
            if (FAILED(hr)) {
                selected = device_aware_result(
                    device.Get(),
                    GpuEncoderStatus::not_supported,
                    hr,
                    "hardware encoder activation failed");
                if (selected.status == GpuEncoderStatus::device_lost) break;
                continue;
            }
            selected = configure_transform(candidate.Get());
            if (selected) {
                selected_mft = activation_identity(activations.values[index]);
                transform = std::move(candidate);
                break;
            }
            if (selected.status == GpuEncoderStatus::device_lost) break;
        }
        if (transform == nullptr) {
            selected = device_aware_result(
                device.Get(), selected.status, selected.hresult, selected.message);
            close();
            return selected;
        }

        hr = create_input_pool();
        if (FAILED(hr)) {
            const auto failure = device_aware_result(
                device.Get(),
                GpuEncoderStatus::out_of_memory,
                hr,
                "encoder input texture pool creation failed");
            close();
            return failure;
        }
        hr = create_output_sample();
        if (FAILED(hr)) {
            const auto failure = device_aware_result(
                device.Get(),
                GpuEncoderStatus::out_of_memory,
                hr,
                "encoder output sample creation failed");
            close();
            return failure;
        }

        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (SUCCEEDED(hr)) hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) {
            const auto failure = device_aware_result(
                device.Get(),
                GpuEncoderStatus::media_foundation_error,
                hr,
                "encoder stream start failed");
            close();
            return failure;
        }
        initialized_flag = true;
        return result(GpuEncoderStatus::ok);
    }

    GpuEncoderResult configure_transform(IMFTransform* candidate) {
        ComPtr<IMFAttributes> attributes;
        HRESULT hr = candidate->GetAttributes(&attributes);
        UINT32 aware = FALSE;
        UINT32 async_value = FALSE;
        if (SUCCEEDED(hr)) {
            (void)attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware);
            (void)attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_value);
            if (async_value != FALSE) {
                (void)attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
        }
        if (aware == FALSE) {
            return result(GpuEncoderStatus::not_supported, MF_E_UNSUPPORTED_D3D_TYPE,
                "hardware encoder is not D3D11 aware");
        }
        hr = candidate->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(device_manager.Get()));
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::d3d_error, hr);
        }

        DWORD candidate_input = 0;
        DWORD candidate_output = 0;
        hr = candidate->GetStreamIDs(1, &candidate_input, 1, &candidate_output);
        if (hr == E_NOTIMPL) {
            candidate_input = 0;
            candidate_output = 0;
            hr = S_OK;
        }
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr);
        }

        ComPtr<IMFMediaType> output_type;
        ComPtr<IMFMediaType> input_type;
        hr = MFCreateMediaType(&output_type);
        if (SUCCEEDED(hr)) {
            hr = set_media_type_common(
                output_type.Get(), codec_subtype(config.codec), config, true);
        }
        if (SUCCEEDED(hr)) hr = output_type->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate);
        if (SUCCEEDED(hr)) hr = candidate->SetOutputType(candidate_output, output_type.Get(), 0);
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input_type);
        if (SUCCEEDED(hr)) {
            hr = set_media_type_common(
                input_type.Get(), input_subtype(config.input_format), config, false);
        }
        if (SUCCEEDED(hr)) hr = candidate->SetInputType(candidate_input, input_type.Get(), 0);
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::unsupported_input_format, hr,
                "encoder rejected the requested media types");
        }

        ComPtr<ICodecAPI> codec_api;
        if (SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&codec_api)))) {
            EncoderColorDescription color;
            (void)encoder_output_color_description(config, color);
            set_codec_bool(codec_api.Get(), CODECAPI_AVLowLatencyMode, config.low_latency);
            set_codec_uint32(codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate, config.bitrate);
            set_codec_uint32(codec_api.Get(), CODECAPI_AVEncMPVGOPSize, config.gop_size);
            set_codec_uint32(
                codec_api.Get(),
                CODECAPI_AVEncCommonRateControlMode,
                eAVEncCommonRateControlMode_CBR);
            set_codec_uint32(
                codec_api.Get(), CODECAPI_AVEncVideoOutputColorPrimaries,
                color.primaries);
            set_codec_uint32(
                codec_api.Get(), CODECAPI_AVEncVideoOutputColorTransferFunction,
                color.transfer);
            set_codec_uint32(
                codec_api.Get(), CODECAPI_AVEncVideoOutputColorTransferMatrix,
                color.matrix);
            set_codec_uint32(
                codec_api.Get(), CODECAPI_AVEncVideoOutputColorNominalRange,
                color.range);
            const std::uint32_t profile = resolved_encoder_profile(config);
            if (profile != 0) {
                set_codec_uint32(
                    codec_api.Get(), CODECAPI_AVEncMPVProfile, profile);
            }
            if (config.low_latency && config.codec == VideoCodec::h264) {
                set_codec_uint32(codec_api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            }
        }

        transform_async = async_value != FALSE;
        input_stream_id = candidate_input;
        output_stream_id = candidate_output;
        output_media_type = std::move(output_type);
        if (transform_async) {
            hr = candidate->QueryInterface(IID_PPV_ARGS(&event_generator));
            if (FAILED(hr)) {
                return device_aware_result(
                    device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                    "async encoder exposes no media event generator");
            }
        }
        return result(GpuEncoderStatus::ok);
    }

    HRESULT create_input_pool() {
        ComPtr<TrackedInputPool> pool;
        pool.Attach(new (std::nothrow) TrackedInputPool());
        if (pool == nullptr) return E_OUTOFMEMORY;
        const HRESULT hr = pool->initialize(
            device.Get(),
            config.width,
            config.height,
            config.input_format,
            config.input_pool_size,
            config.require_video_encoder_input_bind,
            mf_lifetime);
        if (SUCCEEDED(hr)) input_pool = std::move(pool);
        return hr;
    }

    HRESULT create_output_sample() {
        MFT_OUTPUT_STREAM_INFO info{};
        HRESULT hr = transform->GetOutputStreamInfo(output_stream_id, &info);
        if (FAILED(hr)) return hr;
        output_provides_samples = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        if (output_provides_samples) return S_OK;

        std::uint64_t requested = config.output_buffer_bytes;
        if (requested == 0) {
            requested = std::max<std::uint64_t>(
                1u << 20,
                static_cast<std::uint64_t>(config.width) * config.height * 2u);
        }
        requested = std::max<std::uint64_t>(requested, info.cbSize);
        if (requested > std::numeric_limits<DWORD>::max()) return E_OUTOFMEMORY;
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(requested), &buffer);
        if (SUCCEEDED(hr)) hr = MFCreateSample(&output_sample);
        if (SUCCEEDED(hr)) hr = output_sample->AddBuffer(buffer.Get());
        return hr;
    }

    GpuEncoderResult encode(
        ID3D11Texture2D* source,
        std::int64_t timestamp,
        std::int64_t duration,
        bool force_keyframe,
        std::uint32_t subresource) {
        if (!initialized_flag || source == nullptr) {
            return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
        }
        std::lock_guard lock(mutex);
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        ComPtr<ID3D11Device> source_device;
        source->GetDevice(&source_device);
        if (!same_device(device.Get(), source_device.Get())) {
            return result(GpuEncoderStatus::device_mismatch, E_INVALIDARG);
        }
        if (source_desc.Format != config.input_format
            || source_desc.SampleDesc.Count != 1
            || source_desc.SampleDesc.Quality != 0
            || !subresource_dimensions_match(
                source_desc, subresource, config.width, config.height)) {
            return result(GpuEncoderStatus::unsupported_input_format, E_INVALIDARG);
        }
        TrackedInputPool::Lease input;
        GpuEncoderResult ready = acquire_input_surface(input);
        if (!ready) return ready;
        context->CopySubresourceRegion(
            input.texture.Get(), 0, 0, 0, 0, source, subresource, nullptr);
        const GpuEncoderResult copied = device_aware_result(
            device.Get(),
            GpuEncoderStatus::ok,
            S_OK,
            "device was removed during encoder input copy");
        if (!copied) return copied;
        return submit_acquired(
            std::move(input),
            timestamp,
            duration,
            force_keyframe,
            false);
    }

    GpuEncoderResult submit_external(
        ID3D11Texture2D* source,
        std::shared_ptr<void> lifetime_token,
        std::int64_t timestamp,
        std::int64_t duration,
        bool force_keyframe,
        std::uint32_t subresource) {
        if (source == nullptr || lifetime_token == nullptr) {
            return result(GpuEncoderStatus::invalid_argument, E_INVALIDARG,
                "external encoder input requires a texture and lifetime token");
        }
        if (!initialized_flag) {
            return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
        }

        std::lock_guard lock(mutex);
        if (!initialized_flag || external_inputs == nullptr) {
            return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
        }
        if (config.input_format != DXGI_FORMAT_NV12
            && config.input_format != DXGI_FORMAT_P010) {
            return result(GpuEncoderStatus::not_supported, MF_E_UNSUPPORTED_D3D_TYPE,
                "external submission is restricted to planar NV12/P010 input");
        }

        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        ComPtr<ID3D11Device> source_device;
        source->GetDevice(&source_device);
        if (!same_device(device.Get(), source_device.Get())) {
            return result(GpuEncoderStatus::device_mismatch, E_INVALIDARG);
        }
        if (source_desc.Format != config.input_format
            || source_desc.SampleDesc.Count != 1
            || source_desc.SampleDesc.Quality != 0
            || source_desc.Usage != D3D11_USAGE_DEFAULT
            || source_desc.CPUAccessFlags != 0
            || (config.require_video_encoder_input_bind
                && (source_desc.BindFlags
                    & D3D11_BIND_VIDEO_ENCODER) == 0)
            || !subresource_dimensions_match(
                source_desc, subresource, config.width, config.height)) {
            return result(GpuEncoderStatus::unsupported_input_format, E_INVALIDARG,
                config.require_video_encoder_input_bind
                        && (source_desc.BindFlags
                            & D3D11_BIND_VIDEO_ENCODER) == 0
                    ? "external encoder input surface lacks D3D11_BIND_VIDEO_ENCODER"
                    : "external encoder input surface does not match the configured planar stream");
        }

        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFTrackedSample> tracked;
        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateDXGISurfaceBuffer(
            __uuidof(ID3D11Texture2D), source, subresource, FALSE, &buffer);
        if (SUCCEEDED(hr)) {
            hr = verify_dxgi_surface_identity(
                buffer.Get(), source, subresource);
        }
        if (SUCCEEDED(hr)) hr = MFCreateTrackedSample(&tracked);
        if (SUCCEEDED(hr)) hr = tracked.As(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
        if (duration == 0) {
            duration = static_cast<std::int64_t>(
                10'000'000ull * config.frame_rate_denominator
                / config.frame_rate_numerator);
        }
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(timestamp);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                "external tracked sample creation failed");
        }

        ComPtr<ExternalInputLifetime> tracked_lifetime;
        tracked_lifetime.Attach(new (std::nothrow) ExternalInputLifetime(
            std::move(lifetime_token), external_inputs, mf_lifetime));
        if (tracked_lifetime == nullptr) {
            return result(GpuEncoderStatus::out_of_memory, E_OUTOFMEMORY);
        }
        hr = tracked->SetAllocator(tracked_lifetime.Get(), nullptr);
        if (FAILED(hr)) {
            tracked.Reset();
            sample.Reset();
            buffer.Reset();
            tracked_lifetime->cancel();
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                "external tracked sample could not arm its lifetime callback");
        }

        // Keep exactly one local sample reference. Its buffer owns the texture;
        // the caller's token independently protects the shared-bus slot.
        tracked.Reset();
        buffer.Reset();
        GpuEncoderResult ready = wait_until_input_ready();
        if (!ready) {
            sample.Reset();
            tracked_lifetime->cancel();
            return ready;
        }

        context->Flush();
        const GpuEncoderResult flushed = device_aware_result(
            device.Get(),
            GpuEncoderStatus::ok,
            S_OK,
            "device was removed before external encoder submission");
        if (!flushed) {
            sample.Reset();
            tracked_lifetime->cancel();
            return flushed;
        }
        ComPtr<ICodecAPI> codec_api;
        if (force_keyframe && SUCCEEDED(transform.As(&codec_api))) {
            set_codec_bool(codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, true);
        }
        hr = transform->ProcessInput(input_stream_id, sample.Get(), 0);
        if (force_keyframe && codec_api != nullptr) {
            set_codec_bool(codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, false);
        }
        if (FAILED(hr)) {
            // A failed ProcessInput does not transfer ownership to the MFT.
            // Destroy the sample first, then cancel as a defensive fallback if
            // a broken transform did not invoke the tracked callback.
            sample.Reset();
            tracked_lifetime->cancel();
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                "external ProcessInput failed");
        }

        // On success only the MFT can determine when this texture is reusable.
        // Dropping both local references leaves token release to Invoke().
        sample.Reset();
        tracked_lifetime.Reset();
        direct_submissions.fetch_add(1, std::memory_order_relaxed);
        external_submissions.fetch_add(1, std::memory_order_relaxed);
        external_identity_verified_submissions.fetch_add(
            1, std::memory_order_relaxed);
        if ((source_desc.BindFlags & D3D11_BIND_VIDEO_ENCODER) != 0) {
            external_video_encoder_bound_submissions.fetch_add(
                1, std::memory_order_relaxed);
        }
        return transform_async
            ? pump_async_until_need_input()
            : drain_synchronous_output();
    }

    GpuEncoderResult acquire(std::shared_ptr<GpuEncoderInputLeaseState>& output) {
        if (!initialized_flag) {
            return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
        }
        std::lock_guard lock(mutex);
        if (!initialized_flag || input_pool == nullptr) {
            return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
        }
        TrackedInputPool::Lease acquired;
        GpuEncoderResult ready = acquire_input_surface(acquired);
        if (!ready) return ready;

        auto state = std::make_shared<GpuEncoderInputLeaseState>();
        state->pool = input_pool;
        state->lease = std::move(acquired);
        output = std::move(state);
        return result(GpuEncoderStatus::ok);
    }

    GpuEncoderResult submit(
        std::shared_ptr<GpuEncoderInputLeaseState> input,
        std::int64_t timestamp,
        std::int64_t duration,
        bool force_keyframe) {
        if (input == nullptr || input->lease.texture == nullptr
            || input->lease.sample == nullptr) {
            return result(
                GpuEncoderStatus::invalid_argument,
                E_INVALIDARG,
                "encoder input lease is empty");
        }
        std::lock_guard lock(mutex);
        if (!initialized_flag || input_pool == nullptr) {
            return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
        }
        if (input->pool.Get() != input_pool.Get()) {
            return result(
                GpuEncoderStatus::invalid_argument,
                E_INVALIDARG,
                "encoder input lease belongs to a different encoder");
        }
        return submit_acquired(
            std::move(input->lease),
            timestamp,
            duration,
            force_keyframe,
            true);
    }

    GpuEncoderResult submit_acquired(
        TrackedInputPool::Lease input,
        std::int64_t timestamp,
        std::int64_t duration,
        bool force_keyframe,
        bool direct) {
        if (input.texture == nullptr || input.sample == nullptr) {
            return result(GpuEncoderStatus::invalid_argument, E_INVALIDARG);
        }
        if (duration == 0) {
            duration = static_cast<std::int64_t>(
                10'000'000ull * config.frame_rate_denominator
                / config.frame_rate_numerator);
        }

        GpuEncoderResult ready = wait_until_input_ready();
        if (!ready) return ready;

        // Publish every queued copy/VideoProcessor write before the D3D-aware
        // MFT acquires the tracked input surface.
        context->Flush();
        const GpuEncoderResult flushed = device_aware_result(
            device.Get(),
            GpuEncoderStatus::ok,
            S_OK,
            "device was removed before encoder submission");
        if (!flushed) return flushed;
        (void)input.sample->SetSampleTime(timestamp);
        (void)input.sample->SetSampleDuration(duration);

        ComPtr<ICodecAPI> codec_api;
        if (force_keyframe && SUCCEEDED(transform.As(&codec_api))) {
            set_codec_bool(codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, true);
        }
        const HRESULT hr = transform->ProcessInput(
            input_stream_id,
            input.sample.Get(),
            0);
        if (force_keyframe && codec_api != nullptr) {
            set_codec_bool(codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, false);
        }
        // Drop our reference immediately. The tracked sample callback returns
        // this exact slot only after the transform drops its final reference.
        input.sample.Reset();
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(),
                GpuEncoderStatus::media_foundation_error,
                hr,
                "ProcessInput failed");
        }
        if (direct) {
            direct_submissions.fetch_add(1, std::memory_order_relaxed);
        } else {
            copied_submissions.fetch_add(1, std::memory_order_relaxed);
        }

        return transform_async
            ? pump_async_until_need_input()
            : drain_synchronous_output();
    }

    GpuEncoderResult acquire_input_surface(TrackedInputPool::Lease& lease) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(config.event_timeout_ms);
        for (;;) {
            const HRESULT hr = input_pool->try_acquire(lease);
            if (hr == S_OK) return result(GpuEncoderStatus::ok);
            if (FAILED(hr)) {
                return device_aware_result(
                    device.Get(),
                    GpuEncoderStatus::media_foundation_error,
                    hr,
                    "encoder input pool acquisition failed");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return device_aware_result(
                    device.Get(),
                    GpuEncoderStatus::timeout,
                    HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                    "encoder input pool remained in flight");
            }
            input_pool->wait_for_change_until(deadline);
        }
    }

    GpuEncoderResult wait_until_input_ready() {
        if (!transform_async) {
            return drain_synchronous_output();
        }
        if (async_need_input) {
            async_need_input = false;
            return result(GpuEncoderStatus::ok);
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(config.event_timeout_ms);
        std::uint32_t misses = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = event_generator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                wait_for_encoder_event(config.low_latency, misses);
                continue;
            }
            misses = 0;
            if (FAILED(hr)) {
                return device_aware_result(
                    device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                    "encoder input event retrieval failed");
            }
            MediaEventType type = MEUnknown;
            HRESULT event_status = S_OK;
            (void)event->GetType(&type);
            (void)event->GetStatus(&event_status);
            if (FAILED(event_status)) {
                return device_aware_result(
                    device.Get(),
                    GpuEncoderStatus::media_foundation_error,
                    event_status,
                    "encoder input event reported failure");
            }
            if (type == METransformNeedInput) return result(GpuEncoderStatus::ok);
            if (type == METransformHaveOutput) {
                const auto output = process_one_output();
                if (!output && output.hresult != MF_E_TRANSFORM_NEED_MORE_INPUT) return output;
            }
        }
        return device_aware_result(
            device.Get(),
            GpuEncoderStatus::timeout,
            HRESULT_FROM_WIN32(WAIT_TIMEOUT),
            "encoder did not request input before the deadline");
    }

    GpuEncoderResult pump_async_until_need_input() {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(config.event_timeout_ms);
        std::uint32_t misses = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = event_generator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                wait_for_encoder_event(config.low_latency, misses);
                continue;
            }
            misses = 0;
            if (FAILED(hr)) {
                return device_aware_result(
                    device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                    "encoder output event retrieval failed");
            }
            MediaEventType type = MEUnknown;
            HRESULT event_status = S_OK;
            (void)event->GetType(&type);
            (void)event->GetStatus(&event_status);
            if (FAILED(event_status)) {
                return device_aware_result(
                    device.Get(),
                    GpuEncoderStatus::media_foundation_error,
                    event_status,
                    "encoder output event reported failure");
            }
            if (type == METransformNeedInput) {
                async_need_input = true;
                return result(GpuEncoderStatus::ok);
            }
            if (type == METransformHaveOutput) {
                const auto output = process_one_output();
                if (!output && output.hresult != MF_E_TRANSFORM_NEED_MORE_INPUT) return output;
            }
        }
        return device_aware_result(
            device.Get(),
            GpuEncoderStatus::timeout,
            HRESULT_FROM_WIN32(WAIT_TIMEOUT),
            "encoder did not become ready before the deadline");
    }

    GpuEncoderResult drain_synchronous_output() {
        for (;;) {
            const auto output = process_one_output();
            if (!output && output.hresult == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return result(GpuEncoderStatus::ok);
            }
            if (!output) return output;
        }
    }

    GpuEncoderResult process_one_output() {
        if (!output_provides_samples && output_sample != nullptr) {
            DWORD buffers = 0;
            (void)output_sample->GetBufferCount(&buffers);
            for (DWORD index = 0; index < buffers; ++index) {
                ComPtr<IMFMediaBuffer> buffer;
                if (SUCCEEDED(output_sample->GetBufferByIndex(index, &buffer))) {
                    (void)buffer->SetCurrentLength(0);
                }
            }
        }

        MFT_OUTPUT_DATA_BUFFER data{};
        data.dwStreamID = output_stream_id;
        data.pSample = output_provides_samples ? nullptr : output_sample.Get();
        DWORD status = 0;
        HRESULT hr = transform->ProcessOutput(0, 1, &data, &status);
        if (data.pEvents != nullptr) data.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            hr = transform->SetOutputType(output_stream_id, output_media_type.Get(), 0);
            return FAILED(hr)
                ? device_aware_result(
                    device.Get(),
                    GpuEncoderStatus::media_foundation_error,
                    hr,
                    "encoder rejected output type after stream change")
                : result(GpuEncoderStatus::ok);
        }
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                "ProcessOutput failed");
        }

        ComPtr<IMFSample> produced;
        if (data.pSample != nullptr) {
            produced.Attach(data.pSample);
            if (!output_provides_samples) produced.Detach();
        }
        if ((data.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) != 0) {
            return result(GpuEncoderStatus::ok);
        }
        IMFSample* sample = output_provides_samples ? produced.Get() : output_sample.Get();
        if (sample == nullptr) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, E_FAIL,
                "encoder returned no output sample");
        }

        ComPtr<IMFMediaBuffer> contiguous;
        hr = sample->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                "encoder output buffer conversion failed");
        }
        BYTE* bytes = nullptr;
        DWORD maximum = 0;
        DWORD length = 0;
        hr = contiguous->Lock(&bytes, &maximum, &length);
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                "encoder output buffer lock failed");
        }
        if (length == 0 || bytes == nullptr) {
            contiguous->Unlock();
            return result(GpuEncoderStatus::ok);
        }

        LONGLONG timestamp = 0;
        LONGLONG duration = 0;
        (void)sample->GetSampleTime(&timestamp);
        (void)sample->GetSampleDuration(&duration);
        UINT32 clean = FALSE;
        (void)sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
        EncodedPacket packet{
            bytes,
            length,
            config.codec,
            timestamp,
            duration,
            clean != FALSE,
            config.codec != VideoCodec::av1};
        if (config.require_bitstream_color_metadata
            || config.require_bitstream_hdr10_metadata) {
            const GpuError inspected = inspect_encoded_packet_metadata(
                packet, observed_bitstream_metadata);
            if (!inspected) {
                contiguous->Unlock();
                return result(
                    GpuEncoderStatus::not_supported,
                    inspected.hresult,
                    inspected.what());
            }
        }
        try {
            callback(callback_context, packet);
        } catch (...) {
            contiguous->Unlock();
            return result(GpuEncoderStatus::callback_failed, E_FAIL);
        }
        contiguous->Unlock();
        return result(GpuEncoderStatus::ok);
    }

    GpuEncoderResult validate_required_bitstream_metadata() const {
        if (!config.require_bitstream_color_metadata
            && !config.require_bitstream_hdr10_metadata) {
            return result(GpuEncoderStatus::ok);
        }
        GpuHdr10StaticMetadata expected;
        if (config.require_bitstream_hdr10_metadata) expected = config.hdr10;
        const GpuError validated = validate_encoded_video_metadata(
            observed_bitstream_metadata,
            resolved_encoder_color_space(config),
            expected);
        return validated
            ? result(GpuEncoderStatus::ok)
            : result(
                GpuEncoderStatus::not_supported,
                validated.hresult,
                validated.what());
    }

    GpuEncoderResult drain() {
        if (!initialized_flag) return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
        std::lock_guard lock(mutex);
        HRESULT hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (SUCCEEDED(hr)) hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                "encoder drain command failed");
        }

        if (!transform_async) {
            const GpuEncoderResult drained = drain_synchronous_output();
            return drained ? validate_required_bitstream_metadata() : drained;
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(config.event_timeout_ms);
        std::uint32_t misses = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ComPtr<IMFMediaEvent> event;
            hr = event_generator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                wait_for_encoder_event(config.low_latency, misses);
                continue;
            }
            misses = 0;
            if (FAILED(hr)) {
                return device_aware_result(
                    device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                    "encoder drain event retrieval failed");
            }
            MediaEventType type{};
            HRESULT event_status = S_OK;
            (void)event->GetType(&type);
            (void)event->GetStatus(&event_status);
            if (FAILED(event_status)) {
                return device_aware_result(
                    device.Get(),
                    GpuEncoderStatus::media_foundation_error,
                    event_status,
                    "encoder drain event reported failure");
            }
            if (type == METransformDrainComplete) {
                return validate_required_bitstream_metadata();
            }
            if (type == METransformHaveOutput) {
                const auto output = process_one_output();
                if (!output && output.hresult != MF_E_TRANSFORM_NEED_MORE_INPUT) return output;
            }
        }
        return device_aware_result(
            device.Get(),
            GpuEncoderStatus::timeout,
            HRESULT_FROM_WIN32(WAIT_TIMEOUT),
            "encoder drain timed out");
    }

    GpuEncoderResult flush() {
        if (!initialized_flag) return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
        std::lock_guard lock(mutex);
        HRESULT hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (SUCCEEDED(hr)) hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        async_need_input = false;
        if (FAILED(hr)) {
            return device_aware_result(
                device.Get(), GpuEncoderStatus::media_foundation_error, hr,
                "encoder flush command failed");
        }
        observed_bitstream_metadata = {};
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(config.event_timeout_ms);
        if (!input_pool->wait_until_idle(deadline)
            || (external_inputs != nullptr
                && !external_inputs->wait_until_idle(deadline))) {
            return device_aware_result(
                device.Get(),
                GpuEncoderStatus::timeout,
                HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                "encoder did not release all input samples after flush");
        }
        return result(GpuEncoderStatus::ok);
    }

    void close() noexcept {
        initialized_flag = false;
        ComPtr<TrackedInputPool> closing_pool = std::move(input_pool);
        if (closing_pool != nullptr) closing_pool->begin_shutdown();
        if (transform != nullptr) {
            (void)transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            (void)transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        output_sample.Reset();
        event_generator.Reset();
        output_media_type.Reset();
        transform.Reset();
        selected_mft = {};
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(config.event_timeout_ms);
        if (closing_pool != nullptr) {
            (void)closing_pool->wait_until_idle(deadline);
            closing_pool.Reset();
        }
        if (external_inputs != nullptr) {
            (void)external_inputs->wait_until_idle(deadline);
        }
        device_manager.Reset();
        context.Reset();
        device.Reset();
        external_inputs.reset();
        mf_lifetime.reset();
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> device_manager;
    UINT device_manager_token = 0;
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaEventGenerator> event_generator;
    ComPtr<IMFMediaType> output_media_type;
    ComPtr<IMFSample> output_sample;
    ComPtr<TrackedInputPool> input_pool;
    std::shared_ptr<ExternalInputState> external_inputs;
    DWORD input_stream_id = 0;
    DWORD output_stream_id = 0;
    bool output_provides_samples = false;
    bool transform_async = false;
    bool async_need_input = false;
    bool initialized_flag = false;
    std::shared_ptr<MfLifetime> mf_lifetime;
    std::atomic<std::uint64_t> copied_submissions{0};
    std::atomic<std::uint64_t> direct_submissions{0};
    std::atomic<std::uint64_t> external_submissions{0};
    std::atomic<std::uint64_t> external_identity_verified_submissions{0};
    std::atomic<std::uint64_t>
        external_video_encoder_bound_submissions{0};
    EncodedVideoMetadata observed_bitstream_metadata{};
    GpuEncoderConfig config{};
    GpuEncoderMftIdentity selected_mft{};
    EncodedPacketCallback callback = nullptr;
    void* callback_context = nullptr;
    std::mutex mutex;
};

const char* gpu_encoder_status_string(GpuEncoderStatus status) noexcept {
    switch (status) {
    case GpuEncoderStatus::ok: return "ok";
    case GpuEncoderStatus::invalid_argument: return "invalid argument";
    case GpuEncoderStatus::invalid_state: return "invalid state";
    case GpuEncoderStatus::not_supported: return "not supported";
    case GpuEncoderStatus::unsupported_input_format: return "unsupported input format";
    case GpuEncoderStatus::device_mismatch: return "device mismatch";
    case GpuEncoderStatus::timeout: return "timeout";
    case GpuEncoderStatus::callback_failed: return "packet callback failed";
    case GpuEncoderStatus::out_of_memory: return "out of memory";
    case GpuEncoderStatus::media_foundation_error: return "Media Foundation error";
    case GpuEncoderStatus::d3d_error: return "D3D error";
    case GpuEncoderStatus::device_lost: return "device lost";
    default: return "unknown encoder status";
    }
}

GpuEncoderInputLease::GpuEncoderInputLease() noexcept = default;
GpuEncoderInputLease::~GpuEncoderInputLease() = default;
GpuEncoderInputLease::GpuEncoderInputLease(GpuEncoderInputLease&&) noexcept = default;
GpuEncoderInputLease& GpuEncoderInputLease::operator=(
    GpuEncoderInputLease&&) noexcept = default;
GpuEncoderInputLease::operator bool() const noexcept {
    return state_ != nullptr
        && state_->lease.texture != nullptr
        && state_->lease.sample != nullptr;
}
ID3D11Texture2D* GpuEncoderInputLease::texture() const noexcept {
    return state_ != nullptr ? state_->lease.texture.Get() : nullptr;
}
void GpuEncoderInputLease::reset() noexcept { state_.reset(); }

GpuEncoder::GpuEncoder() = default;
GpuEncoder::~GpuEncoder() = default;
GpuEncoder::GpuEncoder(GpuEncoder&&) noexcept = default;
GpuEncoder& GpuEncoder::operator=(GpuEncoder&&) noexcept = default;

GpuEncoderResult GpuEncoder::probe(
    ID3D11Device* device,
    const GpuEncoderConfig& config,
    GpuEncoderSupport& support) noexcept {
    support = {};
    EncoderColorDescription color;
    if (device == nullptr
        || config.width == 0 || config.height == 0
        || config.frame_rate_numerator == 0
        || config.frame_rate_denominator == 0
        || config.bitrate == 0
        || config.input_pool_size == 0 || config.input_pool_size > 32
        || IsEqualGUID(input_subtype(config.input_format), GUID_NULL)
        || IsEqualGUID(codec_subtype(config.codec), GUID_NULL)
        || !encoder_color_description(config, color)
        || !valid_hdr10_static_metadata(config)
        || !valid_bitstream_validation(config)
        || ((config.input_format == DXGI_FORMAT_NV12
                || config.input_format == DXGI_FORMAT_P010)
            && ((config.width | config.height) & 1u) != 0)) {
        return result(GpuEncoderStatus::invalid_argument, E_INVALIDARG);
    }
    const GpuEncoderResult initial_device_state = device_aware_result(
        device, GpuEncoderStatus::ok, S_OK);
    if (!initial_device_state) return initial_device_state;
    support.video_encoder_input_bind_supported =
        video_encoder_input_bind_supported(device, config);
    if (config.require_video_encoder_input_bind
        && !support.video_encoder_input_bind_supported) {
        return result(GpuEncoderStatus::ok);
    }
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        return device_aware_result(
            device, GpuEncoderStatus::media_foundation_error, hr,
            "MFStartup failed during encoder probe");
    }
    ActivationArray activations;
    hr = enumerate_encoders(config, activations);
    support.matching_transform_count = activations.count;
    if (FAILED(hr)) {
        (void)MFShutdown();
        return device_aware_result(
            device,
            GpuEncoderStatus::media_foundation_error,
            hr,
            "hardware encoder enumeration failed during probe");
    }
    if (activations.count == 0) {
        (void)MFShutdown();
        return device_aware_result(
            device, GpuEncoderStatus::ok, S_OK);
    }

    Impl exact_probe;
    exact_probe.device = device;
    exact_probe.device->GetImmediateContext(&exact_probe.context);
    exact_probe.config = config;
    hr = MFCreateDXGIDeviceManager(
        &exact_probe.device_manager_token,
        &exact_probe.device_manager);
    if (SUCCEEDED(hr)) {
        hr = exact_probe.device_manager->ResetDevice(
            exact_probe.device.Get(), exact_probe.device_manager_token);
    }
    const bool device_manager_ready = SUCCEEDED(hr);
    GpuEncoderResult exact_result = SUCCEEDED(hr)
        ? result(GpuEncoderStatus::not_supported, MF_E_TOPO_CODEC_NOT_FOUND)
        : device_aware_result(
            device,
            GpuEncoderStatus::d3d_error,
            hr,
            "DXGI device manager creation failed during encoder probe");
    if (SUCCEEDED(hr)) {
        for (UINT32 index = 0; index < activations.count; ++index) {
            ComPtr<IMFTransform> transform;
            hr = activations.values[index]->ActivateObject(
                IID_PPV_ARGS(&transform));
            if (FAILED(hr)) {
                exact_result = device_aware_result(
                    device,
                    GpuEncoderStatus::not_supported,
                    hr,
                    "hardware encoder activation failed during probe");
            } else {
                exact_result = exact_probe.configure_transform(transform.Get());
                if (exact_result) {
                    support.supported = true;
                    support.d3d11_aware = true;
                    support.asynchronous = exact_probe.transform_async;
                    support.external_planar_input =
                        config.input_format == DXGI_FORMAT_NV12
                        || config.input_format == DXGI_FORMAT_P010;
                    support.external_surface_identity_verifiable =
                        support.external_planar_input;
                    support.mft_internal_copy_observable = false;
                    support.hdr10_static_metadata_media_type =
                        config.hdr10.enabled;
                    support.runtime_copy_evidence_capability =
                        !support.external_planar_input
                        ? GpuCopyEvidenceLevel::none
                        : support.video_encoder_input_bind_supported
                        ? GpuCopyEvidenceLevel::
                            l2_external_surface_identity_and_encoder_bind
                        : GpuCopyEvidenceLevel::
                            l1_no_fluxcap_explicit_copy;
                    break;
                }
            }
            if (exact_result.status == GpuEncoderStatus::device_lost) break;
        }
    }
    exact_probe.close();
    (void)MFShutdown();
    if (!device_manager_ready
        || exact_result.status == GpuEncoderStatus::device_lost) {
        return exact_result;
    }
    return device_aware_result(
        device, GpuEncoderStatus::ok, S_OK);
}

GpuEncoderResult GpuEncoder::initialize(
    ID3D11Device* device,
    const GpuEncoderConfig& config,
    EncodedPacketCallback callback,
    void* context) noexcept {
    try {
        auto implementation = std::make_unique<Impl>();
        auto initialized = implementation->initialize(device, config, callback, context);
        if (!initialized) return initialized;
        impl_ = std::move(implementation);
        return result(GpuEncoderStatus::ok);
    } catch (...) {
        return exception_result();
    }
}

GpuEncoderResult GpuEncoder::acquire_input(
    GpuEncoderInputLease& output) noexcept {
    output.reset();
    if (!impl_) return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
    try {
        return impl_->acquire(output.state_);
    } catch (...) {
        output.reset();
        return exception_result();
    }
}

GpuEncoderResult GpuEncoder::submit_input(
    GpuEncoderInputLease&& input,
    std::int64_t timestamp,
    std::int64_t duration,
    bool force_keyframe) noexcept {
    auto state = std::move(input.state_);
    if (!impl_) return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
    try {
        return impl_->submit(
            std::move(state),
            timestamp,
            duration,
            force_keyframe);
    } catch (...) {
        return exception_result();
    }
}

GpuEncoderResult GpuEncoder::submit_external_texture(
    ID3D11Texture2D* texture,
    std::shared_ptr<void> lifetime_token,
    std::int64_t timestamp,
    std::int64_t duration,
    bool force_keyframe,
    std::uint32_t subresource) noexcept {
    if (!impl_) return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
    try {
        return impl_->submit_external(
            texture,
            std::move(lifetime_token),
            timestamp,
            duration,
            force_keyframe,
            subresource);
    } catch (...) {
        return exception_result();
    }
}

GpuEncoderResult GpuEncoder::encode_texture(
    ID3D11Texture2D* texture,
    std::int64_t timestamp,
    std::int64_t duration,
    bool force_keyframe,
    std::uint32_t subresource) noexcept {
    if (!impl_) return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
    try { return impl_->encode(texture, timestamp, duration, force_keyframe, subresource); }
    catch (...) { return exception_result(); }
}

GpuEncoderResult GpuEncoder::drain() noexcept {
    if (!impl_) return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
    try { return impl_->drain(); } catch (...) { return exception_result(); }
}

GpuEncoderResult GpuEncoder::flush() noexcept {
    if (!impl_) return result(GpuEncoderStatus::invalid_state, E_UNEXPECTED);
    try { return impl_->flush(); } catch (...) { return exception_result(); }
}

void GpuEncoder::close() noexcept {
    if (impl_) impl_->close();
    impl_.reset();
}
GpuEncoderStats GpuEncoder::stats() const noexcept {
    if (!impl_) return {};
    const std::uint64_t external =
        impl_->external_submissions.load(std::memory_order_relaxed);
    const std::uint64_t identity =
        impl_->external_identity_verified_submissions.load(
            std::memory_order_relaxed);
    const std::uint64_t encoder_bound =
        impl_->external_video_encoder_bound_submissions.load(
            std::memory_order_relaxed);
    GpuCopyEvidenceLevel evidence = GpuCopyEvidenceLevel::none;
    if (external != 0) {
        evidence = identity == external && encoder_bound == external
            ? GpuCopyEvidenceLevel::
                l2_external_surface_identity_and_encoder_bind
            : GpuCopyEvidenceLevel::l1_no_fluxcap_explicit_copy;
    }
    return {
        impl_->copied_submissions.load(std::memory_order_relaxed),
        impl_->direct_submissions.load(std::memory_order_relaxed),
        external,
        identity,
        encoder_bound,
        impl_->external_inputs != nullptr
            ? impl_->external_inputs->callback_count()
            : 0,
        evidence};
}
GpuEncoderMftIdentity GpuEncoder::mft_identity() const noexcept {
    return impl_ ? impl_->selected_mft : GpuEncoderMftIdentity{};
}
bool GpuEncoder::initialized() const noexcept {
    return impl_ && impl_->initialized_flag;
}

} // namespace fluxcap::gpu
