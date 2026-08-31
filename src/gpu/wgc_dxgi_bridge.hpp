#ifndef FLUXCAP_GPU_WGC_DXGI_BRIDGE_HPP
#define FLUXCAP_GPU_WGC_DXGI_BRIDGE_HPP

#include <d3d11.h>
#include <wrl/client.h>

#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace fluxcap::gpu::internal {

HRESULT create_winrt_d3d_device(
    ID3D11Device* device,
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice& output) noexcept;

HRESULT get_texture_from_surface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) noexcept;

} // namespace fluxcap::gpu::internal

#endif

