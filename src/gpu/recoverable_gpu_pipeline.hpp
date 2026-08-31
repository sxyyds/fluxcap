#pragma once

#include "fluxcap/gpu.hpp"
#include "pipeline_epoch_supervisor.hpp"

#include <cstdint>

namespace fluxcap::gpu::internal {

enum class RecoverableGpuPipelineBuildStage : std::uint8_t {
    device,
    shared_bus,
    consumer_registration,
    consumer_open,
    async_pipeline,
    capture,
    capture_start,
    complete
};

enum class RecoverableGpuPipelineTestFaultKind : std::uint8_t {
    none,
    device_lost,
    capture_error,
    async_device_lost,
    target_closed
};

struct RecoverableGpuPipelineTestFault final {
    RecoverableGpuPipelineTestFaultKind kind =
        RecoverableGpuPipelineTestFaultKind::none;
    HRESULT hresult = S_OK;
};

// Internal deterministic fault surface. Production code snapshots the hook at
// factory time, so replacing the global hook cannot mutate an existing graph.
// Tests using bypass_hardware still execute every build stage and the complete
// epoch-supervisor state machine without requiring a removable GPU device.
struct RecoverableGpuPipelineTestHook final {
    void* context = nullptr;
    bool bypass_hardware = false;
    RecoverableGpuPipelineResult (*before_build_stage)(
        void*, RecoverableGpuPipelineBuildStage,
        PipelineEpochTicket, DXGI_FORMAT) noexcept = nullptr;
    bool (*query_source_dimensions)(
        void*, PipelineEpochTicket,
        std::uint32_t&, std::uint32_t&) noexcept = nullptr;
    bool (*planar_topology_supported)(
        void*, PipelineEpochTicket,
        std::uint32_t, std::uint32_t) noexcept = nullptr;
    bool (*poll_fault)(
        void*, PipelineEpochTicket,
        RecoverableGpuPipelineTestFault&) noexcept = nullptr;
    bool (*frame_published)(
        void*, PipelineEpochTicket) noexcept = nullptr;
    void (*teardown_epoch)(
        void*, PipelineEpochTicket) noexcept = nullptr;
};

FLUXCAP_GPU_API void set_recoverable_gpu_pipeline_test_hook(
    const RecoverableGpuPipelineTestHook*) noexcept;

} // namespace fluxcap::gpu::internal
