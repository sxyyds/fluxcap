#include "image_encoder.hpp"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;

ImageEncoderResult result(
    ImageEncoderStatus status,
    HRESULT hresult = S_OK,
    std::size_t bytes_written = 0) noexcept {
    ImageEncoderResult value;
    value.status = status;
    value.hresult = hresult;
    value.bytes_written = bytes_written;
    try {
        value.message = image_encoder_status_string(status);
    } catch (...) {
        value.message.clear();
    }
    return value;
}

ImageEncoderResult exception_result() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return result(ImageEncoderStatus::out_of_memory, E_OUTOFMEMORY);
    } catch (...) {
        return result(ImageEncoderStatus::wic_error, E_FAIL);
    }
}

bool same_com_identity(IUnknown* left, IUnknown* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    ComPtr<IUnknown> left_identity;
    ComPtr<IUnknown> right_identity;
    if (FAILED(left->QueryInterface(IID_PPV_ARGS(&left_identity)))
        || FAILED(right->QueryInterface(IID_PPV_ARGS(&right_identity)))) {
        return false;
    }
    return left_identity.Get() == right_identity.Get();
}

enum class PixelLayout : std::uint8_t {
    bgra,
    bgrx,
    rgba
};

bool texture_layout(DXGI_FORMAT format, PixelLayout& layout) noexcept {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        layout = PixelLayout::bgra;
        return true;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        layout = PixelLayout::bgrx;
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        layout = PixelLayout::rgba;
        return true;
    default:
        return false;
    }
}

class MapGuard final {
public:
    MapGuard(ID3D11DeviceContext* context, ID3D11Resource* resource) noexcept
        : context_(context), resource_(resource) {}

    ~MapGuard() {
        if (context_ != nullptr && resource_ != nullptr) {
            context_->Unmap(resource_, 0);
        }
    }

    MapGuard(const MapGuard&) = delete;
    MapGuard& operator=(const MapGuard&) = delete;

private:
    ID3D11DeviceContext* context_;
    ID3D11Resource* resource_;
};

HRESULT set_jpeg_quality(IPropertyBag2* options, float quality) noexcept {
    if (options == nullptr) {
        return E_POINTER;
    }

    PROPBAG2 property{};
    property.dwType = PROPBAG2_TYPE_DATA;
    property.vt = VT_R4;
    property.pstrName = const_cast<wchar_t*>(L"ImageQuality");

    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = std::clamp(quality, 0.0F, 1.0F);
    const HRESULT hr = options->Write(1, &property, &value);
    VariantClear(&value);
    return hr;
}

HRESULT write_direct_bgra(
    IWICBitmapFrameEncode* frame,
    const D3D11_MAPPED_SUBRESOURCE& mapped,
    std::uint32_t height) noexcept {
    if (mapped.RowPitch == 0) {
        return E_INVALIDARG;
    }

    const auto* pixels = static_cast<const BYTE*>(mapped.pData);
    const std::uint32_t maximum_rows = std::max<std::uint32_t>(
        1,
        std::numeric_limits<UINT>::max() / mapped.RowPitch);

    std::uint32_t row = 0;
    while (row < height) {
        const std::uint32_t rows = std::min(maximum_rows, height - row);
        const std::uint64_t bytes64 = static_cast<std::uint64_t>(mapped.RowPitch) * rows;
        if (bytes64 > std::numeric_limits<UINT>::max()) {
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        }
        const HRESULT hr = frame->WritePixels(
            rows,
            mapped.RowPitch,
            static_cast<UINT>(bytes64),
            const_cast<BYTE*>(pixels + static_cast<std::size_t>(row) * mapped.RowPitch));
        if (FAILED(hr)) {
            return hr;
        }
        row += rows;
    }
    return S_OK;
}

HRESULT write_converted_rows(
    IWICBitmapFrameEncode* frame,
    const D3D11_MAPPED_SUBRESOURCE& mapped,
    std::uint32_t width,
    std::uint32_t height,
    PixelLayout layout,
    ImageFormat format,
    bool preserve_alpha,
    std::vector<std::uint8_t>& row_buffer) {
    const std::uint32_t output_bytes_per_pixel = format == ImageFormat::png ? 4u : 3u;
    const std::uint64_t row_bytes64 =
        static_cast<std::uint64_t>(width) * output_bytes_per_pixel;
    if (row_bytes64 > std::numeric_limits<UINT>::max()) {
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    }
    const auto row_bytes = static_cast<std::uint32_t>(row_bytes64);
    row_buffer.resize(row_bytes);

    const auto* source_base = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* source =
            source_base + static_cast<std::size_t>(y) * mapped.RowPitch;
        std::uint8_t* destination = row_buffer.data();

        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t* pixel = source + static_cast<std::size_t>(x) * 4u;
            const bool rgba = layout == PixelLayout::rgba;
            destination[0] = rgba ? pixel[2] : pixel[0];
            destination[1] = pixel[1];
            destination[2] = rgba ? pixel[0] : pixel[2];
            if (format == ImageFormat::png) {
                destination[3] = preserve_alpha && layout != PixelLayout::bgrx
                    ? pixel[3]
                    : 0xffu;
            }
            destination += output_bytes_per_pixel;
        }

        const HRESULT hr = frame->WritePixels(
            1,
            row_bytes,
            row_bytes,
            row_buffer.data());
        if (FAILED(hr)) {
            return hr;
        }
    }
    return S_OK;
}

} // namespace

class WicImageEncoder::Impl final {
public:
    ImageEncoderResult initialize(ID3D11Device* device) {
        if (device == nullptr) {
            return result(ImageEncoderStatus::invalid_argument, E_POINTER);
        }

        close();

        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory2,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) {
            hr = CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory_));
        }
        if (FAILED(hr)) {
            return result(ImageEncoderStatus::wic_error, hr);
        }

        device_ = device;
        device_->GetImmediateContext(&context_);
        if (context_ == nullptr) {
            close();
            return result(ImageEncoderStatus::d3d_error, E_FAIL);
        }
        return result(ImageEncoderStatus::ok);
    }

    ImageEncoderResult encode_file(
        ID3D11Texture2D* texture,
        std::wstring_view path,
        const ImageEncodeOptions& options) {
        if (path.empty() || path.find(L'\0') != std::wstring_view::npos) {
            return result(ImageEncoderStatus::invalid_argument, E_INVALIDARG);
        }
        if (factory_ == nullptr) {
            return result(ImageEncoderStatus::invalid_state, E_UNEXPECTED);
        }

        std::wstring terminated_path(path);
        ComPtr<IWICStream> stream;
        HRESULT hr = factory_->CreateStream(&stream);
        if (SUCCEEDED(hr)) {
            hr = stream->InitializeFromFilename(terminated_path.c_str(), GENERIC_WRITE);
        }
        if (FAILED(hr)) {
            return result(ImageEncoderStatus::io_error, hr);
        }

        ImageEncoderResult encoded = encode_stream(texture, stream.Get(), options);
        if (!encoded) {
            return encoded;
        }

        STATSTG statistics{};
        if (SUCCEEDED(stream->Stat(&statistics, STATFLAG_NONAME))
            && statistics.cbSize.HighPart == 0) {
            encoded.bytes_written = statistics.cbSize.LowPart;
        }
        return encoded;
    }

    ImageEncoderResult encode_memory(
        ID3D11Texture2D* texture,
        std::vector<std::uint8_t>& output,
        const ImageEncodeOptions& options) {
        output.clear();
        if (factory_ == nullptr) {
            return result(ImageEncoderStatus::invalid_state, E_UNEXPECTED);
        }

        ComPtr<IStream> stream;
        HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (FAILED(hr)) {
            return result(ImageEncoderStatus::wic_error, hr);
        }

        ImageEncoderResult encoded = encode_stream(texture, stream.Get(), options);
        if (!encoded) {
            return encoded;
        }

        STATSTG statistics{};
        hr = stream->Stat(&statistics, STATFLAG_NONAME);
        if (FAILED(hr)
            || statistics.cbSize.QuadPart
                > static_cast<ULONGLONG>(std::numeric_limits<std::size_t>::max())) {
            return result(
                ImageEncoderStatus::wic_error,
                FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
        }

        HGLOBAL storage = nullptr;
        hr = GetHGlobalFromStream(stream.Get(), &storage);
        if (FAILED(hr) || storage == nullptr) {
            return result(ImageEncoderStatus::wic_error, FAILED(hr) ? hr : E_FAIL);
        }

        const auto size = static_cast<std::size_t>(statistics.cbSize.QuadPart);
        if (size != 0) {
            const void* bytes = GlobalLock(storage);
            if (bytes == nullptr) {
                return result(
                    ImageEncoderStatus::out_of_memory,
                    HRESULT_FROM_WIN32(GetLastError()));
            }
            try {
                const auto* first = static_cast<const std::uint8_t*>(bytes);
                output.assign(first, first + size);
            } catch (...) {
                GlobalUnlock(storage);
                throw;
            }
            GlobalUnlock(storage);
        }
        return result(ImageEncoderStatus::ok, S_OK, size);
    }

    void close() noexcept {
        staging_.Reset();
        staging_width_ = 0;
        staging_height_ = 0;
        staging_format_ = DXGI_FORMAT_UNKNOWN;
        row_buffer_.clear();
        context_.Reset();
        device_.Reset();
        factory_.Reset();
    }

    [[nodiscard]] bool initialized() const noexcept {
        return factory_ != nullptr && device_ != nullptr && context_ != nullptr;
    }

private:
    ImageEncoderResult encode_stream(
        ID3D11Texture2D* texture,
        IStream* stream,
        const ImageEncodeOptions& options) {
        if (texture == nullptr || stream == nullptr) {
            return result(ImageEncoderStatus::invalid_argument, E_POINTER);
        }
        if (!initialized()) {
            return result(ImageEncoderStatus::invalid_state, E_UNEXPECTED);
        }
        if (options.jpeg_quality < 0.0F || options.jpeg_quality > 1.0F
            || options.dpi_x <= 0.0 || options.dpi_y <= 0.0) {
            return result(ImageEncoderStatus::invalid_argument, E_INVALIDARG);
        }

        ComPtr<ID3D11Device> texture_device;
        texture->GetDevice(&texture_device);
        if (!same_com_identity(device_.Get(), texture_device.Get())) {
            return result(ImageEncoderStatus::device_mismatch, E_INVALIDARG);
        }

        D3D11_TEXTURE2D_DESC source_desc{};
        texture->GetDesc(&source_desc);
        PixelLayout layout{};
        if (!texture_layout(source_desc.Format, layout)
            || source_desc.SampleDesc.Count != 1
            || source_desc.MipLevels == 0
            || source_desc.ArraySize == 0) {
            return result(
                ImageEncoderStatus::unsupported_texture,
                HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        }

        const std::uint64_t subresource_count =
            static_cast<std::uint64_t>(source_desc.MipLevels) * source_desc.ArraySize;
        if (options.subresource >= subresource_count) {
            return result(ImageEncoderStatus::invalid_argument, E_INVALIDARG);
        }
        const std::uint32_t mip = options.subresource % source_desc.MipLevels;
        const std::uint32_t width = std::max(1u, source_desc.Width >> mip);
        const std::uint32_t height = std::max(1u, source_desc.Height >> mip);

        HRESULT hr = ensure_staging(width, height, source_desc.Format);
        if (FAILED(hr)) {
            return result(ImageEncoderStatus::d3d_error, hr);
        }

        context_->CopySubresourceRegion(
            staging_.Get(),
            0,
            0,
            0,
            0,
            texture,
            options.subresource,
            nullptr);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            return result(ImageEncoderStatus::d3d_error, hr);
        }
        MapGuard mapped_guard(context_.Get(), staging_.Get());

        const GUID& container = options.format == ImageFormat::png
            ? GUID_ContainerFormatPng
            : GUID_ContainerFormatJpeg;
        ComPtr<IWICBitmapEncoder> encoder;
        hr = factory_->CreateEncoder(container, nullptr, &encoder);
        if (SUCCEEDED(hr)) {
            hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        }

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> frame_options;
        if (SUCCEEDED(hr)) {
            hr = encoder->CreateNewFrame(&frame, &frame_options);
        }
        if (SUCCEEDED(hr) && options.format == ImageFormat::jpeg) {
            hr = set_jpeg_quality(frame_options.Get(), options.jpeg_quality);
        }
        if (SUCCEEDED(hr)) {
            hr = frame->Initialize(frame_options.Get());
        }
        if (SUCCEEDED(hr)) {
            hr = frame->SetSize(width, height);
        }
        if (SUCCEEDED(hr)) {
            hr = frame->SetResolution(options.dpi_x, options.dpi_y);
        }

        WICPixelFormatGUID requested_format = options.format == ImageFormat::png
            ? GUID_WICPixelFormat32bppBGRA
            : GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) {
            hr = frame->SetPixelFormat(&requested_format);
        }
        const WICPixelFormatGUID& required_format = options.format == ImageFormat::png
            ? GUID_WICPixelFormat32bppBGRA
            : GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr) && !IsEqualGUID(requested_format, required_format)) {
            hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        }

        if (SUCCEEDED(hr)
            && options.format == ImageFormat::png
            && layout == PixelLayout::bgra
            && options.preserve_png_alpha) {
            hr = write_direct_bgra(frame.Get(), mapped, height);
        } else if (SUCCEEDED(hr)) {
            hr = write_converted_rows(
                frame.Get(),
                mapped,
                width,
                height,
                layout,
                options.format,
                options.preserve_png_alpha,
                row_buffer_);
        }

        if (SUCCEEDED(hr)) {
            hr = frame->Commit();
        }
        if (SUCCEEDED(hr)) {
            hr = encoder->Commit();
        }
        if (FAILED(hr)) {
            return result(ImageEncoderStatus::wic_error, hr);
        }
        return result(ImageEncoderStatus::ok);
    }

    HRESULT ensure_staging(
        std::uint32_t width,
        std::uint32_t height,
        DXGI_FORMAT format) noexcept {
        if (staging_ != nullptr
            && staging_width_ == width
            && staging_height_ == height
            && staging_format_ == format) {
            return S_OK;
        }

        staging_.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        const HRESULT hr = device_->CreateTexture2D(
            &description,
            nullptr,
            &staging_);
        if (SUCCEEDED(hr)) {
            staging_width_ = width;
            staging_height_ = height;
            staging_format_ = format;
        }
        return hr;
    }

    ComPtr<IWICImagingFactory> factory_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> staging_;
    std::uint32_t staging_width_ = 0;
    std::uint32_t staging_height_ = 0;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
    std::vector<std::uint8_t> row_buffer_;
};

const char* image_encoder_status_string(ImageEncoderStatus status) noexcept {
    switch (status) {
    case ImageEncoderStatus::ok:
        return "ok";
    case ImageEncoderStatus::invalid_argument:
        return "invalid argument";
    case ImageEncoderStatus::invalid_state:
        return "invalid state";
    case ImageEncoderStatus::unsupported_texture:
        return "unsupported texture";
    case ImageEncoderStatus::device_mismatch:
        return "device mismatch";
    case ImageEncoderStatus::out_of_memory:
        return "out of memory";
    case ImageEncoderStatus::d3d_error:
        return "D3D error";
    case ImageEncoderStatus::wic_error:
        return "WIC error";
    case ImageEncoderStatus::io_error:
        return "I/O error";
    default:
        return "unknown image encoder status";
    }
}

WicImageEncoder::WicImageEncoder()
    : impl_(std::make_unique<Impl>()) {}

WicImageEncoder::~WicImageEncoder() = default;
WicImageEncoder::WicImageEncoder(WicImageEncoder&&) noexcept = default;
WicImageEncoder& WicImageEncoder::operator=(WicImageEncoder&&) noexcept = default;

ImageEncoderResult WicImageEncoder::initialize(ID3D11Device* device) noexcept {
    try {
        if (impl_ == nullptr) {
            impl_ = std::make_unique<Impl>();
        }
        return impl_->initialize(device);
    } catch (...) {
        return exception_result();
    }
}

ImageEncoderResult WicImageEncoder::encode_texture_to_file_sync(
    ID3D11Texture2D* texture,
    std::wstring_view path,
    const ImageEncodeOptions& options) noexcept {
    try {
        if (impl_ == nullptr) {
            return result(ImageEncoderStatus::invalid_state, E_UNEXPECTED);
        }
        return impl_->encode_file(texture, path, options);
    } catch (...) {
        return exception_result();
    }
}

ImageEncoderResult WicImageEncoder::encode_texture_to_memory_sync(
    ID3D11Texture2D* texture,
    std::vector<std::uint8_t>& output,
    const ImageEncodeOptions& options) noexcept {
    try {
        if (impl_ == nullptr) {
            output.clear();
            return result(ImageEncoderStatus::invalid_state, E_UNEXPECTED);
        }
        return impl_->encode_memory(texture, output, options);
    } catch (...) {
        output.clear();
        return exception_result();
    }
}

void WicImageEncoder::close() noexcept {
    if (impl_ != nullptr) {
        impl_->close();
    }
}

bool WicImageEncoder::initialized() const noexcept {
    return impl_ != nullptr && impl_->initialized();
}

} // namespace fluxcap::gpu
