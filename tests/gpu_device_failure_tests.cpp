#include "gpu_device_failure.hpp"

#include <d3d11.h>
#include <mferror.h>
#include <wrl/client.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Microsoft::WRL::ComPtr;
namespace gpu = fluxcap::gpu;
namespace internal = fluxcap::gpu::internal;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

ComPtr<ID3D11Device> create_device() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    constexpr std::array<D3D_FEATURE_LEVEL, 2> levels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL created{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        levels.data(),
        static_cast<UINT>(levels.size()),
        D3D11_SDK_VERSION,
        &device,
        &created,
        &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            levels.data(),
            static_cast<UINT>(levels.size()),
            D3D11_SDK_VERSION,
            &device,
            &created,
            &context);
    }
    if (FAILED(hr)) fail("D3D11 device creation failed: " + std::to_string(hr));
    return device;
}

void test_direct_hresult_classification() {
    constexpr std::array<HRESULT, 4> removed_errors{
        DXGI_ERROR_DEVICE_REMOVED,
        DXGI_ERROR_DEVICE_RESET,
        DXGI_ERROR_DEVICE_HUNG,
        DXGI_ERROR_DRIVER_INTERNAL_ERROR};
    for (const HRESULT hresult : removed_errors) {
        if (!internal::is_device_lost_hresult(hresult)) {
            fail("known DXGI device-removal HRESULT was not classified");
        }
        const auto classified = internal::classify_device_failure(
            nullptr, hresult);
        if (!classified.device_lost || classified.hresult != hresult) {
            fail("direct device-removal HRESULT lost its classification");
        }
    }
    if (internal::is_device_lost_hresult(E_FAIL)
        || internal::is_device_lost_hresult(MF_E_TRANSFORM_TYPE_NOT_SET)) {
        fail("ordinary MF/system failure was classified as device loss");
    }
}

void test_healthy_device_preserves_failure() {
    const ComPtr<ID3D11Device> device = create_device();
    const auto generic = internal::classify_device_failure(
        device.Get(), MF_E_TRANSFORM_TYPE_NOT_SET);
    if (generic.device_lost
        || generic.hresult != MF_E_TRANSFORM_TYPE_NOT_SET) {
        fail("healthy D3D device changed an ordinary MFT failure");
    }
    const auto success = internal::classify_device_failure(device.Get(), S_OK);
    if (success.device_lost || FAILED(success.hresult)) {
        fail("healthy D3D device was reported removed");
    }
}

void test_encoder_pipeline_status_mapping() {
    if (internal::async_status_from_encoder(
            gpu::GpuEncoderStatus::device_lost)
            != gpu::AsyncGpuPipelineStatus::device_lost
        || internal::async_status_from_encoder(
            gpu::GpuEncoderStatus::out_of_memory)
            != gpu::AsyncGpuPipelineStatus::out_of_memory
        || internal::async_status_from_encoder(
            gpu::GpuEncoderStatus::media_foundation_error)
            != gpu::AsyncGpuPipelineStatus::encoder_error
        || internal::async_status_from_encoder(
            gpu::GpuEncoderStatus::d3d_error)
            != gpu::AsyncGpuPipelineStatus::encoder_error) {
        fail("encoder-to-pipeline status mapping is incorrect");
    }
}

} // namespace

int main() {
    try {
        test_direct_hresult_classification();
        test_healthy_device_preserves_failure();
        test_encoder_pipeline_status_mapping();
        std::cout << "gpu_device_failure_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "gpu_device_failure_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
