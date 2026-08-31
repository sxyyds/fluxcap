#include "wgc_dxgi_bridge.hpp"

#include <dxgi.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>

namespace fluxcap::gpu::internal {

HRESULT create_winrt_d3d_device(
    ID3D11Device* device,
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice& output) noexcept {
    if (device == nullptr) {
        return E_POINTER;
    }
    try {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        if (FAILED(hr)) {
            return hr;
        }

        winrt::com_ptr<IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(
            dxgi_device.Get(),
            inspectable.put());
        if (FAILED(hr)) {
            return hr;
        }
        output = inspectable.as<
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        return S_OK;
    } catch (const winrt::hresult_error& error) {
        return error.code();
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT get_texture_from_surface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) noexcept {
    output.Reset();
    if (surface == nullptr) {
        return E_POINTER;
    }
    try {
        auto access = surface.as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        return access->GetInterface(IID_PPV_ARGS(&output));
    } catch (const winrt::hresult_error& error) {
        return error.code();
    } catch (...) {
        return E_FAIL;
    }
}

} // namespace fluxcap::gpu::internal
