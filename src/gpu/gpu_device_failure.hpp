#ifndef FLUXCAP_GPU_DEVICE_FAILURE_HPP
#define FLUXCAP_GPU_DEVICE_FAILURE_HPP

#include <fluxcap/gpu.hpp>

#include <dxgi.h>

namespace fluxcap::gpu::internal {

struct DeviceFailureClassification final {
    bool device_lost = false;
    HRESULT hresult = S_OK;
};

inline constexpr bool is_device_lost_hresult(HRESULT hresult) noexcept {
    return hresult == DXGI_ERROR_DEVICE_REMOVED
        || hresult == DXGI_ERROR_DEVICE_RESET
        || hresult == DXGI_ERROR_DEVICE_HUNG
        || hresult == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// Some D3D-aware MFTs translate device removal into a generic MF failure or
// simply stop producing events. Query the application device before deciding
// which subsystem owns the failure.
inline DeviceFailureClassification classify_device_failure(
    ID3D11Device* device,
    HRESULT operation_hresult) noexcept {
    if (is_device_lost_hresult(operation_hresult)) {
        return {true, operation_hresult};
    }
    if (device != nullptr) {
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) return {true, removed};
    }
    return {false, operation_hresult};
}

inline constexpr AsyncGpuPipelineStatus async_status_from_encoder(
    GpuEncoderStatus status) noexcept {
    switch (status) {
    case GpuEncoderStatus::device_lost:
        return AsyncGpuPipelineStatus::device_lost;
    case GpuEncoderStatus::out_of_memory:
        return AsyncGpuPipelineStatus::out_of_memory;
    default:
        return AsyncGpuPipelineStatus::encoder_error;
    }
}

} // namespace fluxcap::gpu::internal

#endif
