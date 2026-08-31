#include "shared_texture.hpp"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <atomic>
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
GpuError success() noexcept { return make_error(GpuStatus::ok, S_OK, "ok"); }

bool wait_abandoned(HRESULT hr) noexcept {
    return hr == static_cast<HRESULT>(WAIT_ABANDONED)
        || hr == HRESULT_FROM_WIN32(ERROR_ABANDONED_WAIT_0);
}

GpuError abandoned_error(const char* message) noexcept {
    return make_error(
        GpuStatus::device_lost,
        HRESULT_FROM_WIN32(ERROR_ABANDONED_WAIT_0),
        message);
}

bool same_device(ID3D11Device* left, ID3D11Device* right) noexcept {
    ComPtr<IUnknown> a;
    ComPtr<IUnknown> b;
    return left != nullptr && right != nullptr
        && SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b)))
        && a.Get() == b.Get();
}

} // namespace

class SharedTexturePublisher::Impl final {
public:
    ~Impl() {
        if (shared_handle != nullptr) CloseHandle(shared_handle);
    }

    GpuError initialize(ID3D11Device* source_device, const SharedTextureConfig& source_config) {
        if (source_device == nullptr || source_config.width == 0 || source_config.height == 0
            || source_config.format == DXGI_FORMAT_UNKNOWN
            || source_config.producer_key == source_config.consumer_key) {
            return make_error(GpuStatus::invalid_argument, E_INVALIDARG, "invalid shared texture configuration");
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
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE
            | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        HRESULT hr = device->CreateTexture2D(&description, nullptr, &texture);
        if (FAILED(hr)) {
            return make_error(GpuStatus::unsupported, hr, "shared NT texture creation failed");
        }
        hr = texture.As(&keyed_mutex);
        if (FAILED(hr)) return make_error(GpuStatus::unsupported, hr, "keyed mutex is unavailable");

        // A newly created keyed mutex always starts at key 0. Seed a custom
        // producer key once so every valid public configuration can publish.
        if (config.producer_key != 0) {
            hr = keyed_mutex->AcquireSync(0, 0);
            if (hr == WAIT_TIMEOUT || hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) {
                return make_error(GpuStatus::timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                    "timed out while initializing the shared texture producer key");
            }
            if (wait_abandoned(hr)) {
                return abandoned_error(
                    "shared texture mutex was abandoned during initialization");
            }
            if (FAILED(hr)) {
                return make_error(GpuStatus::system_error, hr,
                    "failed to initialize the shared texture producer key");
            }
            hr = keyed_mutex->ReleaseSync(config.producer_key);
            if (FAILED(hr)) {
                return make_error(GpuStatus::system_error, hr,
                    "failed to release the initialized shared texture producer key");
            }
        }

        ComPtr<IDXGIResource1> resource;
        hr = texture.As(&resource);
        if (SUCCEEDED(hr)) {
            hr = resource->CreateSharedHandle(
                nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                nullptr,
                &shared_handle);
        }
        if (FAILED(hr)) return make_error(GpuStatus::system_error, hr, "CreateSharedHandle failed");

        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapter_desc{};
        hr = device.As(&dxgi_device);
        if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetDesc(&adapter_desc);
        if (FAILED(hr)) return make_error(GpuStatus::system_error, hr, "adapter LUID query failed");
        adapter_luid = adapter_desc.AdapterLuid;
        return success();
    }

    GpuError publish(ID3D11Texture2D* source, std::uint32_t timeout_ms) {
        if (source == nullptr) return make_error(GpuStatus::invalid_argument, E_POINTER, "source texture is null");
        std::lock_guard lock(mutex);
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        ComPtr<ID3D11Device> source_device;
        source->GetDevice(&source_device);
        if (!same_device(device.Get(), source_device.Get())
            || description.Width != config.width
            || description.Height != config.height
            || description.Format != config.format
            || description.SampleDesc.Count != 1) {
            return make_error(GpuStatus::invalid_argument, E_INVALIDARG, "source texture does not match shared texture");
        }

        HRESULT hr = keyed_mutex->AcquireSync(config.producer_key, timeout_ms);
        if (hr == WAIT_TIMEOUT || hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) {
            return make_error(GpuStatus::timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT), "shared texture consumer did not release in time");
        }
        if (wait_abandoned(hr)) {
            return abandoned_error("shared texture consumer abandoned the keyed mutex");
        }
        if (FAILED(hr)) return make_error(GpuStatus::system_error, hr, "AcquireSync failed");

        context->CopyResource(texture.Get(), source);
        context->Flush();
        const HRESULT removed = device->GetDeviceRemovedReason();
        const HRESULT release = keyed_mutex->ReleaseSync(config.consumer_key);
        if (FAILED(removed)) return make_error(GpuStatus::device_lost, removed, "D3D11 device was removed");
        if (FAILED(release)) return make_error(GpuStatus::system_error, release, "ReleaseSync failed");
        ++publish_sequence;
        return success();
    }

    GpuError export_to_process(HANDLE process, SharedTextureExport& metadata) const {
        if (process == nullptr || shared_handle == nullptr) {
            return make_error(GpuStatus::invalid_argument, E_INVALIDARG, "target process or shared handle is invalid");
        }
        HANDLE duplicated = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(),
                shared_handle,
                process,
                &duplicated,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            return make_error(GpuStatus::system_error, HRESULT_FROM_WIN32(GetLastError()), "DuplicateHandle failed");
        }
        metadata = {};
        metadata.structure_size = sizeof(metadata);
        metadata.protocol_version = 1;
        metadata.width = config.width;
        metadata.height = config.height;
        metadata.format = static_cast<std::uint32_t>(config.format);
        metadata.adapter_luid_low = adapter_luid.LowPart;
        metadata.adapter_luid_high = adapter_luid.HighPart;
        metadata.handle_value = reinterpret_cast<std::uint64_t>(duplicated);
        metadata.producer_key = config.producer_key;
        metadata.consumer_key = config.consumer_key;
        metadata.publish_sequence = publish_sequence.load(std::memory_order_relaxed);
        return success();
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> keyed_mutex;
    HANDLE shared_handle = nullptr;
    LUID adapter_luid{};
    SharedTextureConfig config{};
    std::atomic<std::uint64_t> publish_sequence{0};
    std::mutex mutex;
};

SharedTexturePublisher::SharedTexturePublisher() noexcept = default;
SharedTexturePublisher::~SharedTexturePublisher() = default;
SharedTexturePublisher::SharedTexturePublisher(SharedTexturePublisher&&) noexcept = default;
SharedTexturePublisher& SharedTexturePublisher::operator=(SharedTexturePublisher&&) noexcept = default;

GpuError SharedTexturePublisher::create(
    ID3D11Device* device,
    const SharedTextureConfig& config,
    SharedTexturePublisher& output) noexcept {
    try {
        auto implementation = std::make_unique<Impl>();
        const GpuError initialized = implementation->initialize(device, config);
        if (!initialized) return initialized;
        output.impl_ = std::move(implementation);
        return success();
    } catch (const std::bad_alloc&) {
        return make_error(GpuStatus::out_of_memory, E_OUTOFMEMORY, "memory allocation failed");
    } catch (...) {
        return make_error(GpuStatus::system_error, E_FAIL, "unknown shared texture failure");
    }
}

GpuError SharedTexturePublisher::publish(ID3D11Texture2D* source, std::uint32_t timeout_ms) noexcept {
    if (!impl_) return make_error(GpuStatus::invalid_argument, E_UNEXPECTED, "publisher is uninitialized");
    try { return impl_->publish(source, timeout_ms); }
    catch (...) { return make_error(GpuStatus::system_error, E_FAIL, "unknown publish failure"); }
}

GpuError SharedTexturePublisher::export_to_process(
    HANDLE process,
    SharedTextureExport& metadata) const noexcept {
    if (!impl_) return make_error(GpuStatus::invalid_argument, E_UNEXPECTED, "publisher is uninitialized");
    try { return impl_->export_to_process(process, metadata); }
    catch (...) { return make_error(GpuStatus::system_error, E_FAIL, "unknown export failure"); }
}

ID3D11Texture2D* SharedTexturePublisher::texture() const noexcept {
    return impl_ ? impl_->texture.Get() : nullptr;
}
std::uint64_t SharedTexturePublisher::sequence() const noexcept {
    return impl_ ? impl_->publish_sequence.load(std::memory_order_relaxed) : 0;
}
SharedTextureConfig SharedTexturePublisher::config() const noexcept {
    return impl_ ? impl_->config : SharedTextureConfig{};
}
bool SharedTexturePublisher::initialized() const noexcept { return impl_ != nullptr; }

class SharedTextureConsumer::Impl final {
public:
    ~Impl() {
        if (owns_key) {
            (void)keyed_mutex->ReleaseSync(producer_key);
        }
    }

    GpuError initialize(
        ID3D11Device* source_device,
        const SharedTextureExport& metadata,
        bool take_handle_ownership) {
        if (source_device == nullptr
            || metadata.structure_size != sizeof(SharedTextureExport)
            || metadata.protocol_version != 1
            || metadata.handle_value == 0
            || metadata.width == 0
            || metadata.height == 0
            || metadata.producer_key == metadata.consumer_key) {
            return make_error(GpuStatus::invalid_argument, E_INVALIDARG,
                "invalid shared texture export metadata");
        }
        HANDLE handle = reinterpret_cast<HANDLE>(
            static_cast<ULONG_PTR>(metadata.handle_value));
        const auto close_handle = [&] {
            if (take_handle_ownership && handle != nullptr) {
                CloseHandle(handle);
                handle = nullptr;
            }
        };

        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapter_desc{};
        HRESULT hr = source_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetDesc(&adapter_desc);
        if (FAILED(hr)) {
            close_handle();
            return make_error(GpuStatus::system_error, hr, "consumer adapter query failed");
        }
        if (adapter_desc.AdapterLuid.LowPart != metadata.adapter_luid_low
            || adapter_desc.AdapterLuid.HighPart != metadata.adapter_luid_high) {
            close_handle();
            return make_error(GpuStatus::unsupported, E_INVALIDARG,
                "shared texture belongs to a different GPU adapter");
        }

        ComPtr<ID3D11Device1> device1;
        hr = source_device->QueryInterface(IID_PPV_ARGS(&device1));
        if (SUCCEEDED(hr)) {
            hr = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&texture));
        }
        close_handle();
        if (FAILED(hr)) {
            return make_error(GpuStatus::system_error, hr, "OpenSharedResource1 failed");
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Width != metadata.width
            || description.Height != metadata.height
            || description.Format != static_cast<DXGI_FORMAT>(metadata.format)) {
            texture.Reset();
            return make_error(GpuStatus::invalid_argument, E_INVALIDARG,
                "opened shared texture does not match its metadata");
        }
        hr = texture.As(&keyed_mutex);
        if (FAILED(hr)) {
            texture.Reset();
            return make_error(GpuStatus::unsupported, hr, "shared texture has no keyed mutex");
        }
        producer_key = metadata.producer_key;
        consumer_key = metadata.consumer_key;
        observed_sequence = metadata.publish_sequence == 0
            ? 0
            : metadata.publish_sequence - 1;
        return success();
    }

    GpuError acquire(std::uint32_t timeout_ms) {
        std::lock_guard lock(mutex);
        if (owns_key) {
            return make_error(GpuStatus::invalid_argument, E_UNEXPECTED,
                "consumer already owns the shared texture");
        }
        const HRESULT hr = keyed_mutex->AcquireSync(consumer_key, timeout_ms);
        if (hr == WAIT_TIMEOUT || hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) {
            return make_error(GpuStatus::timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                "producer did not publish before the timeout");
        }
        if (wait_abandoned(hr)) {
            return abandoned_error("shared texture producer abandoned the keyed mutex");
        }
        if (FAILED(hr)) return make_error(GpuStatus::system_error, hr, "consumer AcquireSync failed");
        owns_key = true;
        ++observed_sequence;
        return success();
    }

    GpuError release() {
        std::lock_guard lock(mutex);
        if (!owns_key) {
            return make_error(GpuStatus::invalid_argument, E_UNEXPECTED,
                "consumer does not own the shared texture");
        }
        const HRESULT hr = keyed_mutex->ReleaseSync(producer_key);
        if (FAILED(hr)) return make_error(GpuStatus::system_error, hr, "consumer ReleaseSync failed");
        owns_key = false;
        return success();
    }

    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> keyed_mutex;
    std::uint64_t producer_key = 0;
    std::uint64_t consumer_key = 0;
    std::uint64_t observed_sequence = 0;
    bool owns_key = false;
    std::mutex mutex;
};

SharedTextureConsumer::SharedTextureConsumer() noexcept = default;
SharedTextureConsumer::~SharedTextureConsumer() = default;
SharedTextureConsumer::SharedTextureConsumer(SharedTextureConsumer&&) noexcept = default;
SharedTextureConsumer& SharedTextureConsumer::operator=(SharedTextureConsumer&&) noexcept = default;

GpuError SharedTextureConsumer::open(
    ID3D11Device* device,
    const SharedTextureExport& metadata,
    bool take_handle_ownership,
    SharedTextureConsumer& output) noexcept {
    try {
        auto implementation = std::make_unique<Impl>();
        const GpuError opened = implementation->initialize(
            device, metadata, take_handle_ownership);
        if (!opened) return opened;
        output.impl_ = std::move(implementation);
        return success();
    } catch (const std::bad_alloc&) {
        return make_error(GpuStatus::out_of_memory, E_OUTOFMEMORY, "memory allocation failed");
    } catch (...) {
        return make_error(GpuStatus::system_error, E_FAIL, "unknown shared consumer failure");
    }
}

GpuError SharedTextureConsumer::acquire(std::uint32_t timeout_ms) noexcept {
    if (!impl_) return make_error(GpuStatus::invalid_argument, E_UNEXPECTED, "consumer is uninitialized");
    try { return impl_->acquire(timeout_ms); }
    catch (...) { return make_error(GpuStatus::system_error, E_FAIL, "unknown acquire failure"); }
}

GpuError SharedTextureConsumer::release() noexcept {
    if (!impl_) return make_error(GpuStatus::invalid_argument, E_UNEXPECTED, "consumer is uninitialized");
    try { return impl_->release(); }
    catch (...) { return make_error(GpuStatus::system_error, E_FAIL, "unknown release failure"); }
}

ID3D11Texture2D* SharedTextureConsumer::texture() const noexcept {
    return impl_ ? impl_->texture.Get() : nullptr;
}
std::uint64_t SharedTextureConsumer::sequence() const noexcept {
    return impl_ ? impl_->observed_sequence : 0;
}
bool SharedTextureConsumer::acquired() const noexcept {
    return impl_ && impl_->owns_key;
}

} // namespace fluxcap::gpu
