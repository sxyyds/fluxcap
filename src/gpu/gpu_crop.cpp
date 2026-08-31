#include "gpu_crop.hpp"

#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>
#include <utility>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;

GpuError make_error(GpuStatus status, HRESULT hr, const char* message) noexcept {
    GpuError value;
    value.status = status;
    value.hresult = hr;
    if (message != nullptr) {
        (void)strncpy_s(value.message.data(), value.message.size(), message, _TRUNCATE);
    }
    return value;
}

GpuError success() noexcept {
    return make_error(GpuStatus::ok, S_OK, "ok");
}

bool same_device(ID3D11Device* left, ID3D11Device* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> first;
    ComPtr<IUnknown> second;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&first)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&second)))
        && first.Get() == second.Get();
}

bool valid_region(const GpuCropConfig& config) noexcept {
    return config.input_width != 0
        && config.input_height != 0
        && config.width != 0
        && config.height != 0
        && config.x < config.input_width
        && config.y < config.input_height
        && config.width <= config.input_width - config.x
        && config.height <= config.input_height - config.y
        && (config.format == DXGI_FORMAT_B8G8R8A8_UNORM
            || config.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}

bool same_object(IUnknown* left, IUnknown* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> first;
    ComPtr<IUnknown> second;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&first)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&second)))
        && first.Get() == second.Get();
}

} // namespace

class GpuCrop::Impl final {
public:
    GpuError initialize(
        ID3D11Device* source_device,
        const GpuCropConfig& source_config) {
        if (source_device == nullptr || !valid_region(source_config)) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "invalid GPU crop configuration");
        }

        device = source_device;
        device->GetImmediateContext(&context);
        config = source_config;

        D3D11_TEXTURE2D_DESC description{};
        description.Width = config.width;
        description.Height = config.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = config.format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = config.bind_flags;
        const HRESULT hr = device->CreateTexture2D(&description, nullptr, &output);
        if (FAILED(hr)) {
            return make_error(
                hr == E_OUTOFMEMORY ? GpuStatus::out_of_memory : GpuStatus::unsupported,
                hr,
                "GPU crop output texture creation failed");
        }
        return success();
    }

    GpuError process(ID3D11Texture2D* input) {
        if (input == nullptr || output == nullptr) {
            return make_error(
                GpuStatus::invalid_argument,
                E_POINTER,
                "GPU crop input is null or crop is uninitialized");
        }

        std::lock_guard lock(mutex);
        D3D11_TEXTURE2D_DESC description{};
        input->GetDesc(&description);
        if (description.Width != config.input_width
            || description.Height != config.input_height
            || description.Format != config.format
            || description.SampleDesc.Count != 1
            || description.MipLevels == 0
            || description.ArraySize == 0) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "input texture does not match GPU crop configuration");
        }

        ComPtr<ID3D11Device> input_device;
        input->GetDevice(&input_device);
        if (!same_device(device.Get(), input_device.Get())) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "input texture belongs to a different D3D11 device");
        }
        if (config.width == config.input_width
            && config.height == config.input_height
            && same_object(input, output.Get())) {
            return make_error(
                GpuStatus::invalid_argument,
                E_INVALIDARG,
                "GPU crop input and output textures must not alias");
        }

        const D3D11_BOX region{
            config.x,
            config.y,
            0,
            config.x + config.width,
            config.y + config.height,
            1};
        context->CopySubresourceRegion(
            output.Get(), 0, 0, 0, 0, input, 0, &region);
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            return make_error(
                GpuStatus::device_lost,
                removed,
                "device was removed during GPU crop submission");
        }
        generation.fetch_add(1, std::memory_order_release);
        return success();
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> output;
    GpuCropConfig config{};
    std::atomic<std::uint64_t> generation{0};
    std::mutex mutex;
};

GpuCrop::GpuCrop() noexcept = default;
GpuCrop::~GpuCrop() = default;
GpuCrop::GpuCrop(GpuCrop&&) noexcept = default;
GpuCrop& GpuCrop::operator=(GpuCrop&&) noexcept = default;

GpuError GpuCrop::create(
    ID3D11Device* device,
    const GpuCropConfig& config,
    GpuCrop& output) noexcept {
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
            "GPU crop allocation failed");
    } catch (...) {
        return make_error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown GPU crop failure");
    }
}

GpuError GpuCrop::process(ID3D11Texture2D* input) noexcept {
    if (!impl_) {
        return make_error(
            GpuStatus::invalid_argument,
            E_UNEXPECTED,
            "GPU crop is uninitialized");
    }
    try {
        return impl_->process(input);
    } catch (...) {
        return make_error(
            GpuStatus::system_error,
            E_FAIL,
            "unknown GPU crop failure");
    }
}

ID3D11Texture2D* GpuCrop::output_texture() const noexcept {
    return impl_ ? impl_->output.Get() : nullptr;
}

std::uint64_t GpuCrop::generation() const noexcept {
    return impl_ ? impl_->generation.load(std::memory_order_acquire) : 0;
}

GpuCropConfig GpuCrop::config() const noexcept {
    return impl_ ? impl_->config : GpuCropConfig{};
}

bool GpuCrop::initialized() const noexcept {
    return impl_ != nullptr;
}

} // namespace fluxcap::gpu
