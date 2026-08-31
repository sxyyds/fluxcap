#include <fluxcap/fluxcap.hpp>
#include <fluxcap/gpu.hpp>

#include <cstring>

int main() {
    const auto config = fluxcap_config_default();
    fluxcap::gpu::GpuTransform transform;
    fluxcap::gpu::SharedTextureConsumer consumer;
    fluxcap::gpu::SharedFrameBusPublisher frame_bus;
    fluxcap::gpu::SharedFrameBusConsumer frame_bus_consumer;
    fluxcap::gpu::SharedFrameBusWriteLease write_lease;
    fluxcap::gpu::SharedFrameBusFrameLease frame_lease;
    fluxcap::gpu::GpuEncoder video_encoder;
    const auto encoder_identity = video_encoder.mft_identity();
    fluxcap::gpu::GpuEncoderConfig encoder_config;
    fluxcap::gpu::GpuEncoderSupport encoder_support;
    fluxcap::gpu::AsyncGpuPipeline async_pipeline;
    fluxcap::gpu::WicImageEncoder image_encoder;
    fluxcap::gpu::DesktopDuplicationCapture duplication_capture;
    fluxcap::gpu::RecoverableGpuPipeline recoverable_pipeline;
    fluxcap::gpu::RecoverableGpuPipelineConfig recoverable_config;
    recoverable_config.monitor_backend =
        fluxcap::gpu::RecoverableMonitorCaptureBackend::desktop_duplication;
    const auto recoverable_snapshot = recoverable_pipeline.snapshot();
    const auto window_to_bus =
        &fluxcap::gpu::WgcCapture::create_for_window_to_bus;
    const auto monitor_to_bus =
        &fluxcap::gpu::WgcCapture::create_for_monitor_to_bus;
    const auto duplicate_monitor_to_bus =
        &fluxcap::gpu::DesktopDuplicationCapture::create_for_monitor_to_bus;
    const bool gpu_imports_work = !transform.initialized()
        && transform.compatibility_copy_submissions() == 0
        && !consumer.acquired()
        && !frame_bus.initialized()
        && !frame_bus_consumer.initialized()
        && !write_lease
        && !frame_lease
        && !video_encoder.initialized()
        && IsEqualGUID(encoder_identity.clsid, GUID_NULL)
        && encoder_identity.friendly_name[0] == '\0'
        && !encoder_config.require_video_encoder_input_bind
        && !encoder_support.video_encoder_input_bind_supported
        && !encoder_support.mft_internal_copy_observable
        && !async_pipeline.initialized()
        && !image_encoder.initialized()
        && !duplication_capture.initialized()
        && duplication_capture.desktop_present_frames() == 0
        && !recoverable_pipeline.initialized()
        && recoverable_config.monitor_backend
            == fluxcap::gpu::RecoverableMonitorCaptureBackend::desktop_duplication
        && recoverable_snapshot.active_monitor_backend
            == fluxcap::gpu::RecoverableMonitorCaptureBackend::windows_graphics_capture
        && recoverable_snapshot.active_bus_color_space == DXGI_COLOR_SPACE_CUSTOM
        && !recoverable_snapshot.active_external_encoder_input
        && recoverable_snapshot.active_source_width == 0
        && recoverable_snapshot.active_source_height == 0
        && recoverable_snapshot.active_bus_width == 0
        && recoverable_snapshot.active_bus_height == 0
        && window_to_bus != nullptr
        && monitor_to_bus != nullptr
        && duplicate_monitor_to_bus != nullptr
        && std::strcmp(
            fluxcap::gpu::wgc_status_string(fluxcap::gpu::WgcStatus::ok),
            "ok") == 0;
    return config.abi_version == FLUXCAP_ABI_VERSION
            && std::strcmp(fluxcap_version_string(), "0.1.0") == 0
            && gpu_imports_work
        ? 0
        : 1;
}
