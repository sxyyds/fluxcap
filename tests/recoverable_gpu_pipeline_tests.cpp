#include "recoverable_gpu_pipeline.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

namespace gpu = fluxcap::gpu;
namespace internal = fluxcap::gpu::internal;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require(
    const gpu::RecoverableGpuPipelineResult& result,
    const char* message) {
    require(static_cast<bool>(result), message);
}

struct HookState final {
    std::atomic<std::uint32_t> build_count{0};
    std::atomic<std::uint32_t> teardown_count{0};
    std::atomic<std::uint32_t> dimension_query_count{0};
    std::atomic<std::uint32_t> source_width{320};
    std::atomic<std::uint32_t> source_height{320};
    std::atomic<std::uint32_t> planar_build_failures{0};
    std::atomic<std::uint32_t> planar_activation_failures{0};
    std::atomic<bool> planar_supported{true};
    std::atomic<bool> fault_enabled{false};
    std::atomic<bool> fault_emitted{false};
    std::uint32_t failed_initial_builds = 0;
    std::uint32_t failed_recovery_builds = 0;
    std::uint32_t terminal_build = 0;
    gpu::RecoverableGpuPipelineStatus initial_failure_status =
        gpu::RecoverableGpuPipelineStatus::build_failed;
    HRESULT initial_failure_hresult = E_FAIL;
    gpu::RecoverableGpuPipelineStatus terminal_build_status =
        gpu::RecoverableGpuPipelineStatus::unsupported;
    HRESULT terminal_build_hresult = E_NOINTERFACE;
    gpu::RecoverableGpuPipelineStatus planar_failure_status =
        gpu::RecoverableGpuPipelineStatus::build_failed;
    HRESULT planar_failure_hresult = E_FAIL;
    gpu::RecoverableGpuPipelineStatus planar_activation_failure_status =
        gpu::RecoverableGpuPipelineStatus::build_failed;
    HRESULT planar_activation_failure_hresult = E_FAIL;
    internal::RecoverableGpuPipelineTestFaultKind fault_kind =
        internal::RecoverableGpuPipelineTestFaultKind::device_lost;
    HRESULT fault_hresult = DXGI_ERROR_DEVICE_REMOVED;
};

gpu::RecoverableGpuPipelineResult before_build_stage(
    void* context,
    internal::RecoverableGpuPipelineBuildStage stage,
    internal::PipelineEpochTicket,
    DXGI_FORMAT bus_format) noexcept {
    auto& state = *static_cast<HookState*>(context);
    if (stage == internal::RecoverableGpuPipelineBuildStage::device) {
        state.build_count.fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint32_t build = state.build_count.load(
        std::memory_order_relaxed);
    if (stage == internal::RecoverableGpuPipelineBuildStage::shared_bus
        && (bus_format == DXGI_FORMAT_NV12
            || bus_format == DXGI_FORMAT_P010)) {
        std::uint32_t failures = state.planar_build_failures.load(
            std::memory_order_relaxed);
        while (failures != 0
            && !state.planar_build_failures.compare_exchange_weak(
                failures, failures - 1, std::memory_order_relaxed)) {
        }
        if (failures != 0) {
            return {
                state.planar_failure_status,
                state.planar_failure_hresult,
                "injected planar topology build failure"};
        }
    }
    if (stage == internal::RecoverableGpuPipelineBuildStage::capture_start
        && (bus_format == DXGI_FORMAT_NV12
            || bus_format == DXGI_FORMAT_P010)) {
        std::uint32_t failures = state.planar_activation_failures.load(
            std::memory_order_relaxed);
        while (failures != 0
            && !state.planar_activation_failures.compare_exchange_weak(
                failures, failures - 1, std::memory_order_relaxed)) {
        }
        if (failures != 0) {
            return {
                state.planar_activation_failure_status,
                state.planar_activation_failure_hresult,
                "injected planar activation failure"};
        }
    }
    if (stage == internal::RecoverableGpuPipelineBuildStage::shared_bus
        && build <= state.failed_initial_builds) {
        return {
            state.initial_failure_status,
            state.initial_failure_hresult,
            "injected initial build failure"};
    }
    if (stage == internal::RecoverableGpuPipelineBuildStage::shared_bus
        && build == state.terminal_build) {
        return {
            state.terminal_build_status,
            state.terminal_build_hresult,
            "injected terminal replacement failure"};
    }
    if (stage == internal::RecoverableGpuPipelineBuildStage::shared_bus
        && build >= 2
        && build < 2 + state.failed_recovery_builds) {
        return {
            gpu::RecoverableGpuPipelineStatus::build_failed,
            E_FAIL,
            "injected staged build failure"};
    }
    return {};
}

bool query_source_dimensions(
    void* context,
    internal::PipelineEpochTicket,
    std::uint32_t& width,
    std::uint32_t& height) noexcept {
    auto& state = *static_cast<HookState*>(context);
    state.dimension_query_count.fetch_add(1, std::memory_order_relaxed);
    width = state.source_width.load(std::memory_order_relaxed);
    height = state.source_height.load(std::memory_order_relaxed);
    return width != 0 && height != 0;
}

bool planar_topology_supported(
    void* context,
    internal::PipelineEpochTicket,
    std::uint32_t,
    std::uint32_t) noexcept {
    const auto& state = *static_cast<HookState*>(context);
    return state.planar_supported.load(std::memory_order_relaxed);
}

bool poll_fault(
    void* context,
    internal::PipelineEpochTicket,
    internal::RecoverableGpuPipelineTestFault& output) noexcept {
    auto& state = *static_cast<HookState*>(context);
    if (!state.fault_enabled.load(std::memory_order_acquire)
        || state.fault_emitted.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    output.kind = state.fault_kind;
    output.hresult = state.fault_hresult;
    return true;
}

bool frame_published(
    void* context,
    internal::PipelineEpochTicket) noexcept {
    const auto& state = *static_cast<HookState*>(context);
    return state.build_count.load(std::memory_order_acquire) >= 2;
}

void teardown_epoch(
    void* context,
    internal::PipelineEpochTicket) noexcept {
    auto& state = *static_cast<HookState*>(context);
    state.teardown_count.fetch_add(1, std::memory_order_relaxed);
}

class ScopedHook final {
public:
    explicit ScopedHook(HookState& state) {
        hook_.context = &state;
        hook_.bypass_hardware = true;
        hook_.before_build_stage = &before_build_stage;
        hook_.query_source_dimensions = &query_source_dimensions;
        hook_.planar_topology_supported = &planar_topology_supported;
        hook_.poll_fault = &poll_fault;
        hook_.frame_published = &frame_published;
        hook_.teardown_epoch = &teardown_epoch;
        internal::set_recoverable_gpu_pipeline_test_hook(&hook_);
    }

    ~ScopedHook() {
        internal::set_recoverable_gpu_pipeline_test_hook(nullptr);
    }

private:
    internal::RecoverableGpuPipelineTestHook hook_{};
};

gpu::RecoverableGpuPipelineConfig test_config() {
    gpu::RecoverableGpuPipelineConfig config;
    config.mailbox.mode = gpu::WgcMailboxMode::centered_region;
    config.mailbox.width = 320;
    config.mailbox.height = 320;
    config.pipeline.transform.input_width = 320;
    config.pipeline.transform.input_height = 320;
    config.pipeline.transform.output_width = 320;
    config.pipeline.transform.output_height = 320;
    config.pipeline.transform.output_format = gpu::GpuPixelFormat::nv12;
    config.pipeline.encoder.width = 320;
    config.pipeline.encoder.height = 320;
    config.pipeline.encoder.input_format = DXGI_FORMAT_NV12;
    config.pipeline.queue_depth = 4;
    config.bus_mode =
        gpu::RecoverableGpuPipelineBusMode::planar_encoder_input;
    config.retry.monitor_interval_ms = 1;
    config.retry.initial_backoff_ms = 1;
    config.retry.maximum_backoff_ms = 4;
    config.retry.backoff_multiplier = 2;
    return config;
}

void packet_callback(void* context, const gpu::EncodedPacket&);

void test_hdr_planar_bypass_snapshot_contract() {
    HookState hook_state;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.capture.pixel_format = gpu::WgcPixelFormat::rgba16_float;
    config.pipeline.transform.output_format = gpu::GpuPixelFormat::p010;
    config.pipeline.encoder.input_format = DXGI_FORMAT_P010;
    config.bus_mode =
        gpu::RecoverableGpuPipelineBusMode::planar_encoder_input;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "HDR planar bypass factory failed");
    require(pipeline.start(), "HDR planar bypass epoch failed to start");
    const auto snapshot = pipeline.snapshot();
    require(snapshot.state == gpu::RecoverableGpuPipelineState::running
            && snapshot.active_bus_format == DXGI_FORMAT_P010
            && snapshot.active_bus_color_space
                == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
            && snapshot.active_external_encoder_input,
        "FP16 capture did not resolve to a P010/PQ external planar contract");
    pipeline.stop();
}

void packet_callback(void* context, const gpu::EncodedPacket&) {
    static_cast<std::atomic<std::uint32_t>*>(context)->fetch_add(
        1, std::memory_order_relaxed);
}

template <typename Predicate>
void wait_until(Predicate&& predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void test_full_frame_resize_rebuilds_with_fresh_dimensions() {
    HookState hook_state;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.mailbox = {};
    config.mailbox.mode = gpu::WgcMailboxMode::full_frame;
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "full-frame resize factory failed");
    require(pipeline.start(), "full-frame resize initial epoch failed");
    const auto initial = pipeline.snapshot();
    require(initial.active_source_width == 320
            && initial.active_source_height == 320
            && initial.active_bus_format == DXGI_FORMAT_NV12,
        "initial full-frame contract is inconsistent");

    hook_state.source_width.store(640, std::memory_order_relaxed);
    hook_state.source_height.store(480, std::memory_order_relaxed);
    wait_until([&] {
        const auto current = pipeline.snapshot();
        return current.state == gpu::RecoverableGpuPipelineState::running
            && current.epoch == initial.epoch + 1
            && current.active_source_width == 640
            && current.active_source_height == 480;
    }, "full-frame resize did not install a fresh dimension contract");

    const auto resized = pipeline.snapshot();
    require(resized.recovery_attempts == 1
            && resized.recovery_successes == 1
            && resized.rebuild_attempts == 2,
        "full-frame resize used the wrong recovery path");
    require(resized.active_bus_format == DXGI_FORMAT_NV12
            && resized.active_external_encoder_input
            && resized.active_source_width == 640
            && resized.active_source_height == 480
            && resized.active_bus_width == 320
            && resized.active_bus_height == 320,
        "resized automatic graph did not retain scaled planar topology");
    require(!resized.automatic_planar_disabled
            && resized.automatic_planar_fallbacks == 0,
        "scaled planar resize unexpectedly disabled automatic planar mode");
    pipeline.stop();
}

void test_fixed_roi_unavailability_does_not_rebuild() {
    HookState hook_state;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "fixed-ROI factory failed");
    require(pipeline.start(), "fixed-ROI initial epoch failed");
    const auto initial = pipeline.snapshot();
    const auto baseline_queries = hook_state.dimension_query_count.load(
        std::memory_order_relaxed);

    hook_state.source_width.store(200, std::memory_order_relaxed);
    hook_state.source_height.store(200, std::memory_order_relaxed);
    wait_until([&] {
        return hook_state.dimension_query_count.load(
            std::memory_order_relaxed) >= baseline_queries + 8;
    }, "fixed-ROI source shrink was not observed by the monitor");

    const auto unavailable = pipeline.snapshot();
    require(unavailable.state == gpu::RecoverableGpuPipelineState::running
            && unavailable.epoch == initial.epoch
            && unavailable.rebuild_attempts == initial.rebuild_attempts
            && unavailable.recovery_attempts == 0,
        "temporary fixed-ROI unavailability rebuilt the graph");
    pipeline.stop();
}

void test_automatic_planar_capability_and_build_fallbacks() {
    {
        HookState hook_state;
        hook_state.planar_supported.store(false, std::memory_order_relaxed);
        ScopedHook hook(hook_state);
        std::atomic<std::uint32_t> packets{0};
        auto config = test_config();
        config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;

        gpu::RecoverableGpuPipeline pipeline;
        require(
            gpu::RecoverableGpuPipeline::create_for_window(
                GetDesktopWindow(), config, &packet_callback, &packets,
                pipeline),
            "automatic capability fallback factory failed");
        require(pipeline.start(),
            "automatic capability fallback did not start");
        const auto snapshot = pipeline.snapshot();
        require(snapshot.epoch == 1
                && snapshot.rebuild_attempts == 1
                && snapshot.rebuild_failures == 0,
            "capability fallback consumed an epoch retry");
        require(snapshot.active_bus_format == DXGI_FORMAT_B8G8R8A8_UNORM
                && snapshot.automatic_planar_disabled
                && snapshot.automatic_planar_fallbacks == 1,
            "unsupported planar capability did not become sticky native fallback");
        pipeline.stop();
    }

    {
        HookState hook_state;
        hook_state.planar_build_failures.store(1, std::memory_order_relaxed);
        ScopedHook hook(hook_state);
        std::atomic<std::uint32_t> packets{0};
        auto config = test_config();
        config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;

        gpu::RecoverableGpuPipeline pipeline;
        require(
            gpu::RecoverableGpuPipeline::create_for_window(
                GetDesktopWindow(), config, &packet_callback, &packets,
                pipeline),
            "automatic build fallback factory failed");
        require(pipeline.start(), "automatic planar build did not fallback");
        const auto snapshot = pipeline.snapshot();
        require(snapshot.epoch == 1
                && snapshot.rebuild_attempts == 1
                && snapshot.rebuild_failures == 0,
            "planar candidate fallback consumed retry policy debt");
        require(snapshot.active_bus_format == DXGI_FORMAT_B8G8R8A8_UNORM
                && snapshot.automatic_planar_disabled
                && snapshot.automatic_planar_fallbacks == 1,
            "planar build failure did not install sticky native fallback");
        require(hook_state.planar_build_failures.load(
                std::memory_order_relaxed) == 0,
            "planar build failure injection was not exercised");
        pipeline.stop();
    }


    {
        HookState hook_state;
        hook_state.planar_activation_failures.store(
            1, std::memory_order_relaxed);
        hook_state.planar_activation_failure_status =
            gpu::RecoverableGpuPipelineStatus::unsupported;
        hook_state.planar_activation_failure_hresult = E_NOINTERFACE;
        ScopedHook hook(hook_state);
        std::atomic<std::uint32_t> packets{0};
        auto config = test_config();
        config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;
        config.retry.max_retry_count = 1;

        gpu::RecoverableGpuPipeline pipeline;
        require(
            gpu::RecoverableGpuPipeline::create_for_window(
                GetDesktopWindow(), config, &packet_callback, &packets,
                pipeline),
            "automatic activation fallback factory failed");
        require(pipeline.start(),
            "automatic planar activation did not recover natively");
        const auto snapshot = pipeline.snapshot();
        require(snapshot.epoch == 2
                && snapshot.rebuild_attempts == 2
                && snapshot.rebuild_failures == 1,
            "planar activation fallback did not use a fresh epoch");
        require(snapshot.active_bus_format == DXGI_FORMAT_B8G8R8A8_UNORM
                && snapshot.automatic_planar_disabled
                && snapshot.automatic_planar_fallbacks == 1,
            "planar activation failure was retried on planar topology");
        pipeline.stop();
    }
}

void test_replacement_planar_activation_falls_back_on_unsupported() {
    HookState hook_state;
    hook_state.fault_kind =
        internal::RecoverableGpuPipelineTestFaultKind::device_lost;
    hook_state.fault_hresult = DXGI_ERROR_DEVICE_REMOVED;
    hook_state.planar_activation_failure_status =
        gpu::RecoverableGpuPipelineStatus::unsupported;
    hook_state.planar_activation_failure_hresult = E_NOINTERFACE;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "replacement activation fallback factory failed");
    require(pipeline.start(),
        "replacement activation fallback initial epoch failed");
    const auto initial = pipeline.snapshot();
    require(initial.active_bus_format == DXGI_FORMAT_NV12,
        "replacement activation fallback did not start planar");

    hook_state.planar_activation_failures.store(1, std::memory_order_relaxed);
    hook_state.fault_enabled.store(true, std::memory_order_release);
    wait_until([&] {
        const auto current = pipeline.snapshot();
        return current.state == gpu::RecoverableGpuPipelineState::running
            && current.epoch == initial.epoch + 2
            && current.recovery_successes == 1
            && !current.first_recovered_frame_pending;
    }, "unsupported replacement activation did not fallback natively");

    const auto recovered = pipeline.snapshot();
    require(recovered.epoch_nonce != initial.epoch_nonce
            && recovered.rebuild_attempts == 3
            && recovered.rebuild_failures == 1
            && recovered.active_bus_format == DXGI_FORMAT_B8G8R8A8_UNORM
            && recovered.automatic_planar_disabled
            && recovered.automatic_planar_fallbacks == 1,
        "unsupported replacement activation did not preserve recovery topology");
    pipeline.stop();
}

void test_runtime_planar_failure_is_sticky_native() {
    HookState hook_state;
    hook_state.fault_kind =
        internal::RecoverableGpuPipelineTestFaultKind::capture_error;
    hook_state.fault_hresult = E_FAIL;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "runtime planar fallback factory failed");
    require(pipeline.start(), "runtime planar fallback initial epoch failed");
    const auto initial = pipeline.snapshot();
    require(initial.active_bus_format == DXGI_FORMAT_NV12,
        "runtime fallback test did not start on planar topology");

    hook_state.fault_enabled.store(true, std::memory_order_release);
    wait_until([&] {
        const auto current = pipeline.snapshot();
        return current.state == gpu::RecoverableGpuPipelineState::running
            && current.epoch == initial.epoch + 1
            && current.recovery_successes == 1
            && !current.first_recovered_frame_pending;
    }, "runtime planar failure did not recover");

    const auto recovered = pipeline.snapshot();
    require(recovered.active_bus_format == DXGI_FORMAT_B8G8R8A8_UNORM
            && recovered.automatic_planar_disabled
            && recovered.automatic_planar_fallbacks == 1,
        "runtime planar failure was retried on planar topology");
    pipeline.stop();
}

void test_duplication_access_lost_rebuilds_without_planar_fallback() {
    HookState hook_state;
    hook_state.fault_kind =
        internal::RecoverableGpuPipelineTestFaultKind::capture_error;
    hook_state.fault_hresult = DXGI_ERROR_ACCESS_LOST;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "access-lost recovery factory failed");
    require(pipeline.start(), "access-lost initial epoch failed");
    const auto initial = pipeline.snapshot();
    require(initial.active_bus_format == DXGI_FORMAT_NV12,
        "access-lost test did not start planar");

    hook_state.fault_enabled.store(true, std::memory_order_release);
    wait_until([&] {
        const auto current = pipeline.snapshot();
        return current.state == gpu::RecoverableGpuPipelineState::running
            && current.epoch == initial.epoch + 1
            && current.recovery_successes == 1
            && !current.first_recovered_frame_pending;
    }, "Desktop Duplication access loss did not rebuild the epoch");
    const auto recovered = pipeline.snapshot();
    require(recovered.active_bus_format == DXGI_FORMAT_NV12
            && !recovered.automatic_planar_disabled
            && recovered.automatic_planar_fallbacks == 0
            && recovered.rebuild_attempts == 2
            && recovered.rebuild_failures == 0,
        "access loss was misclassified as a planar capability failure");
    pipeline.stop();
}

void test_empty_full_frame_dimensions_wait_without_retry_debt() {
    HookState hook_state;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.mailbox = {};
    config.mailbox.mode = gpu::WgcMailboxMode::full_frame;
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::capture_native;
    config.retry.max_retry_count = 0;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "empty-dimension wait factory failed");
    require(pipeline.start(), "empty-dimension wait initial epoch failed");
    const auto initial = pipeline.snapshot();

    hook_state.source_width.store(0, std::memory_order_relaxed);
    hook_state.source_height.store(0, std::memory_order_relaxed);
    const auto baseline_queries = hook_state.dimension_query_count.load(
        std::memory_order_relaxed);
    hook_state.fault_enabled.store(true, std::memory_order_release);
    wait_until([&] {
        return hook_state.dimension_query_count.load(
            std::memory_order_relaxed) >= baseline_queries + 8
            && pipeline.snapshot().state
                == gpu::RecoverableGpuPipelineState::building;
    }, "0x0 source dimensions did not remain in transient build wait");
    const auto waiting = pipeline.snapshot();
    require(waiting.epoch == initial.epoch + 1
            && waiting.rebuild_attempts == 2
            && waiting.rebuild_failures == 0
            && waiting.consecutive_retry_count == 0,
        "0x0 source dimensions consumed recovery retry debt");

    hook_state.source_width.store(640, std::memory_order_relaxed);
    hook_state.source_height.store(480, std::memory_order_relaxed);
    wait_until([&] {
        const auto current = pipeline.snapshot();
        return current.state == gpu::RecoverableGpuPipelineState::running
            && current.epoch == initial.epoch + 1
            && current.active_source_width == 640
            && current.active_source_height == 480;
    }, "dimension wait did not resume the same replacement epoch");
    const auto recovered = pipeline.snapshot();
    require(recovered.rebuild_attempts == 2
            && recovered.rebuild_failures == 0,
        "dimension wait resumed through an unnecessary build retry");
    pipeline.stop();
}

void test_nontransient_initial_failures_do_not_retry() {
    const auto run = [](gpu::RecoverableGpuPipelineStatus status, HRESULT hr) {
        HookState hook_state;
        hook_state.failed_initial_builds = 100;
        hook_state.initial_failure_status = status;
        hook_state.initial_failure_hresult = hr;
        ScopedHook hook(hook_state);
        std::atomic<std::uint32_t> packets{0};
        auto config = test_config();
        config.bus_mode = gpu::RecoverableGpuPipelineBusMode::capture_native;
        config.retry.max_retry_count = 8;

        gpu::RecoverableGpuPipeline pipeline;
        require(
            gpu::RecoverableGpuPipeline::create_for_window(
                GetDesktopWindow(), config, &packet_callback, &packets,
                pipeline),
            "nontransient initial-failure factory failed");
        const auto started = pipeline.start();
        require(started.status == status,
            "nontransient initial failure changed status");
        const auto failed = pipeline.snapshot();
        require(failed.state == gpu::RecoverableGpuPipelineState::failed
                && failed.epoch == 1
                && failed.rebuild_attempts == 1
                && failed.rebuild_failures == 1
                && failed.recovery_attempts == 0
                && failed.consecutive_retry_count == 0,
            "nontransient initial failure entered retry policy");
        pipeline.stop();
    };

    run(gpu::RecoverableGpuPipelineStatus::invalid_argument, E_INVALIDARG);
    run(gpu::RecoverableGpuPipelineStatus::unsupported, E_NOINTERFACE);
    run(
        gpu::RecoverableGpuPipelineStatus::target_closed,
        HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
}

void test_critical_planar_failures_are_not_fallbacks() {
    HookState hook_state;
    hook_state.planar_build_failures.store(1, std::memory_order_relaxed);
    hook_state.planar_failure_status =
        gpu::RecoverableGpuPipelineStatus::device_lost;
    hook_state.planar_failure_hresult = DXGI_ERROR_DEVICE_REMOVED;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;
    config.retry.max_retry_count = 0;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "critical planar failure factory failed");
    const auto started = pipeline.start();
    require(started.status
            == gpu::RecoverableGpuPipelineStatus::retry_exhausted,
        "critical planar failure bypassed the recovery policy");
    const auto failed = pipeline.snapshot();
    require(!failed.automatic_planar_disabled
            && failed.automatic_planar_fallbacks == 0
            && failed.rebuild_attempts == 1,
        "device loss was incorrectly swallowed by planar fallback");
    pipeline.stop();
}

void test_access_lost_planar_build_retries_without_fallback() {
    HookState hook_state;
    hook_state.planar_build_failures.store(1, std::memory_order_relaxed);
    hook_state.planar_failure_status =
        gpu::RecoverableGpuPipelineStatus::build_failed;
    hook_state.planar_failure_hresult = DXGI_ERROR_ACCESS_LOST;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;
    config.retry.max_retry_count = 1;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "access-lost planar build factory failed");
    require(pipeline.start(),
        "access-lost planar build did not follow retry policy");
    const auto recovered = pipeline.snapshot();
    require(recovered.state == gpu::RecoverableGpuPipelineState::running
            && recovered.epoch == 2
            && recovered.rebuild_attempts == 2
            && recovered.rebuild_failures == 1
            && recovered.active_bus_format == DXGI_FORMAT_NV12
            && !recovered.automatic_planar_disabled
            && recovered.automatic_planar_fallbacks == 0,
        "access-lost planar build was misclassified as a capability failure");
    pipeline.stop();
}

void test_initial_build_failures_follow_retry_policy() {
    HookState hook_state;
    hook_state.failed_initial_builds = 2;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::capture_native;
    config.retry.max_retry_count = 2;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "initial retry factory failed");
    require(pipeline.start(), "transient initial failures were not retried");
    const auto recovered = pipeline.snapshot();
    require(recovered.state == gpu::RecoverableGpuPipelineState::running
            && recovered.epoch == 3
            && recovered.rebuild_attempts == 3
            && recovered.rebuild_failures == 2,
        "initial build retry did not use fresh epochs");
    require(recovered.recovery_attempts == 1
            && recovered.recovery_successes == 1
            && recovered.consecutive_retry_count == 0,
        "successful initial retry retained inconsistent recovery state");
    pipeline.stop();

    HookState exhausted_state;
    exhausted_state.failed_initial_builds = 100;
    ScopedHook exhausted_hook(exhausted_state);
    gpu::RecoverableGpuPipeline exhausted;
    config.retry.max_retry_count = 1;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, exhausted),
        "initial retry-exhaustion factory failed");
    const auto started = exhausted.start();
    require(started.status
            == gpu::RecoverableGpuPipelineStatus::retry_exhausted,
        "initial retry exhaustion returned the wrong status");
    const auto failed = exhausted.snapshot();
    require(failed.state == gpu::RecoverableGpuPipelineState::failed
            && failed.epoch == 2
            && failed.rebuild_attempts == 2
            && failed.rebuild_failures == 2
            && failed.consecutive_retry_count == 2,
        "initial retry exhaustion ignored its configured budget");
    exhausted.stop();
}

void test_nontransient_replacement_stops_retry_episode() {
    HookState hook_state;
    hook_state.failed_initial_builds = 1;
    hook_state.terminal_build = 2;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::capture_native;
    config.retry.max_retry_count = 8;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "terminal replacement factory failed");
    const auto started = pipeline.start();
    require(started.status == gpu::RecoverableGpuPipelineStatus::unsupported
            && started.hresult == E_NOINTERFACE
            && started.message == "injected terminal replacement failure",
        "terminal replacement failure was rewritten or retried");
    const auto failed = pipeline.snapshot();
    require(failed.state == gpu::RecoverableGpuPipelineState::failed
            && failed.epoch == 2
            && failed.rebuild_attempts == 2
            && failed.rebuild_failures == 2
            && failed.recovery_attempts == 1
            && failed.recovery_successes == 0
            && failed.consecutive_retry_count == 2,
        "terminal replacement failure consumed extra retry attempts");
    const auto last_error = pipeline.last_error();
    require(last_error.status
                == gpu::RecoverableGpuPipelineStatus::unsupported
            && last_error.hresult == E_NOINTERFACE
            && last_error.message == "injected terminal replacement failure",
        "terminal replacement failure lost its original status");
    pipeline.stop();
}

void test_rebuild_failures_are_abandoned_before_success() {
    HookState hook_state;
    hook_state.failed_recovery_builds = 2;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.retry.max_retry_count = 3;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "recoverable window pipeline factory failed");
    require(pipeline.initialized(), "factory did not initialize the wrapper");
    require(pipeline.start(), "initial deterministic epoch failed to start");
    const auto initial = pipeline.snapshot();
    require(initial.state == gpu::RecoverableGpuPipelineState::running,
        "initial epoch is not running");
    require(initial.epoch != 0 && initial.epoch_nonce != 0,
        "initial epoch ticket is empty");
    require(initial.active_bus_format == DXGI_FORMAT_NV12,
        "planar bus selection changed");

    hook_state.fault_enabled.store(true, std::memory_order_release);
    wait_until([&] {
        const auto current = pipeline.snapshot();
        return current.state == gpu::RecoverableGpuPipelineState::running
            && current.recovery_successes == 1
            && !current.first_recovered_frame_pending;
    }, "replacement epoch did not recover after abandoned builds");

    const auto recovered = pipeline.snapshot();
    require(recovered.epoch == initial.epoch + 3,
        "each abandoned build did not advance to a fresh epoch");
    require(recovered.epoch_nonce != initial.epoch_nonce,
        "replacement epoch reused the initial nonce");
    require(recovered.recovery_attempts == 1
            && recovered.recovery_successes == 1,
        "recovery counters are inconsistent");
    require(recovered.rebuild_attempts == 4
            && recovered.rebuild_failures == 2,
        "staged build counters are inconsistent");
    require(recovered.consecutive_retry_count == 0,
        "successful recovery retained retry debt");
    require(recovered.capture_running && recovered.encoder_accepting,
        "replacement test graph was not activated");
    require(hook_state.teardown_count.load(std::memory_order_relaxed) >= 1,
        "failed device epoch was not fully torn down");
    require(pipeline.last_error(),
        "successful recovery did not clear the current error");
    require(packets.load(std::memory_order_relaxed) == 0,
        "hardware-bypass test unexpectedly emitted a packet");

    pipeline.stop();
    require(pipeline.snapshot().state
            == gpu::RecoverableGpuPipelineState::stopped,
        "stop did not join the control thread");
    require(hook_state.teardown_count.load(std::memory_order_relaxed) == 2,
        "active replacement graph was not torn down exactly once");
}

void test_retry_exhaustion_is_terminal() {
    HookState hook_state;
    hook_state.failed_recovery_builds = 100;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    config.retry.max_retry_count = 1;

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "retry-exhaustion factory failed");
    require(pipeline.start(), "retry-exhaustion initial epoch failed");
    const auto initial = pipeline.snapshot();
    hook_state.fault_enabled.store(true, std::memory_order_release);
    wait_until([&] {
        return pipeline.snapshot().state
            == gpu::RecoverableGpuPipelineState::failed;
    }, "retry exhaustion did not become terminal");

    const auto failed = pipeline.snapshot();
    require(failed.epoch == initial.epoch + 2,
        "retry exhaustion did not abandon both failed build epochs");
    require(failed.recovery_attempts == 1
            && failed.recovery_successes == 0,
        "failed recovery counters are inconsistent");
    require(failed.rebuild_attempts == 3
            && failed.rebuild_failures == 2,
        "retry limit performed the wrong number of attempts");
    require(failed.consecutive_retry_count == 2,
        "terminal retry count is incorrect");
    require(pipeline.last_error().status
            == gpu::RecoverableGpuPipelineStatus::retry_exhausted,
        "terminal error did not report retry exhaustion");
    require(!failed.capture_running && !failed.encoder_accepting,
        "terminal pipeline retained an active graph");
    pipeline.stop();
}

void test_target_closed_does_not_rebuild() {
    HookState hook_state;
    hook_state.fault_kind =
        internal::RecoverableGpuPipelineTestFaultKind::target_closed;
    hook_state.fault_hresult = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();

    gpu::RecoverableGpuPipeline pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, pipeline),
        "target-closed factory failed");
    require(pipeline.start(), "target-closed initial epoch failed");
    hook_state.fault_enabled.store(true, std::memory_order_release);
    wait_until([&] {
        return pipeline.snapshot().state
            == gpu::RecoverableGpuPipelineState::failed;
    }, "target closure did not stop the graph");

    const auto snapshot = pipeline.snapshot();
    require(snapshot.recovery_attempts == 0
            && snapshot.rebuild_attempts == 1,
        "target closure incorrectly triggered device recovery");
    require(pipeline.last_error().status
            == gpu::RecoverableGpuPipelineStatus::target_closed,
        "target closure reported the wrong terminal status");
    require(hook_state.teardown_count.load(std::memory_order_relaxed) == 1,
        "closed-target graph was not torn down");
    pipeline.stop();
}

void test_monitor_factory_and_planar_validation() {
    HookState hook_state;
    ScopedHook hook(hook_state);
    std::atomic<std::uint32_t> packets{0};
    auto config = test_config();
    const HMONITOR monitor = MonitorFromWindow(
        GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    gpu::RecoverableGpuPipeline monitor_pipeline;
    require(
        gpu::RecoverableGpuPipeline::create_for_monitor(
            monitor, config, &packet_callback, &packets, monitor_pipeline),
        "recoverable monitor factory failed");
    require(monitor_pipeline.snapshot().state
            == gpu::RecoverableGpuPipelineState::stopped,
        "factory started monitor capture eagerly");

    auto duplication_config = config;
    duplication_config.monitor_backend =
        gpu::RecoverableMonitorCaptureBackend::desktop_duplication;
    // require_border is WGC-only and must keep failing closed for the
    // explicit Desktop Duplication backend.
    duplication_config.capture.require_border = true;
    gpu::RecoverableGpuPipeline invalid_duplication_border;
    const auto invalid_border =
        gpu::RecoverableGpuPipeline::create_for_monitor(
            monitor,
            duplication_config,
            &packet_callback,
            &packets,
            invalid_duplication_border);
    require(invalid_border.status
            == gpu::RecoverableGpuPipelineStatus::invalid_argument,
        "explicit Desktop Duplication accepted WGC-only border options");
    duplication_config.capture.require_border = false;
    // include_cursor is satisfied by Desktop Duplication GPU cursor
    // compositing and is no longer a rejection reason.
    gpu::RecoverableGpuPipeline invalid_window_backend;
    const auto invalid_window =
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(),
            duplication_config,
            &packet_callback,
            &packets,
            invalid_window_backend);
    require(invalid_window.status
            == gpu::RecoverableGpuPipelineStatus::invalid_argument,
        "window capture accepted the monitor-only duplication backend");

    config.pipeline.transform.output_width = 640;
    config.pipeline.encoder.width = 640;
    gpu::RecoverableGpuPipeline scaled;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(), config, &packet_callback, &packets, scaled),
        "scaled planar zero-copy factory rejected a fused transform");
    require(scaled.start(), "scaled planar test epoch failed to start");
    const auto scaled_snapshot = scaled.snapshot();
    require(scaled_snapshot.active_bus_format == DXGI_FORMAT_NV12
            && scaled_snapshot.active_external_encoder_input
            && scaled_snapshot.active_source_width == 320
            && scaled_snapshot.active_source_height == 320
            && scaled_snapshot.active_bus_width == 640
            && scaled_snapshot.active_bus_height == 320,
        "scaled planar epoch did not separate source and bus dimensions");
    scaled.stop();

    config.capture.include_cursor = false;
    config.capture.include_cursor_metadata = true;
    gpu::RecoverableGpuPipeline scaled_cursor;
    const auto accepted_cursor =
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(),
            config,
            &packet_callback,
            &packets,
            scaled_cursor);
    require(accepted_cursor,
        "scaled planar mode rejected mapped independent cursor metadata");
    require(scaled_cursor.start(),
        "scaled cursor-metadata epoch failed to start");
    require(scaled_cursor.snapshot().active_external_encoder_input,
        "scaled cursor metadata disabled the planar external-input path");
    scaled_cursor.stop();

    config.pipeline.transform.output_width = 480;
    config.pipeline.encoder.width = 480;
    config.bus_mode = gpu::RecoverableGpuPipelineBusMode::automatic;
    gpu::RecoverableGpuPipeline fractional_cursor;
    require(
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(),
            config,
            &packet_callback,
            &packets,
            fractional_cursor),
        "fractional cursor scaling factory failed");
    require(fractional_cursor.start(),
        "fractional cursor fallback epoch failed to start");
    const auto fractional_cursor_snapshot = fractional_cursor.snapshot();
    require(fractional_cursor_snapshot.active_bus_format
            == DXGI_FORMAT_B8G8R8A8_UNORM
            && !fractional_cursor_snapshot.active_external_encoder_input,
        "fractional cursor shape incorrectly entered the planar bus");
    fractional_cursor.stop();

    config.bus_mode =
        gpu::RecoverableGpuPipelineBusMode::planar_encoder_input;
    gpu::RecoverableGpuPipeline invalid_fractional_cursor;
    const auto rejected_fractional_cursor =
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(),
            config,
            &packet_callback,
            &packets,
            invalid_fractional_cursor);
    require(rejected_fractional_cursor.status
            == gpu::RecoverableGpuPipelineStatus::invalid_argument,
        "explicit planar mode accepted a position-dependent cursor shape");

    config.capture.include_cursor_metadata = false;
    config.capture.damage_mode =
        gpu::WgcDamageMode::native_with_inferred_moves;
    config.pipeline.transform.output_width = 640;
    config.pipeline.encoder.width = 640;
    gpu::RecoverableGpuPipeline scaled_inference;
    const auto accepted_inference =
        gpu::RecoverableGpuPipeline::create_for_window(
            GetDesktopWindow(),
            config,
            &packet_callback,
            &packets,
            scaled_inference);
    require(accepted_inference,
        "scaled planar mode rejected exactly mapped move inference");
    require(scaled_inference.start(),
        "scaled inferred-move epoch failed to start");
    require(scaled_inference.snapshot().active_external_encoder_input,
        "scaled move inference disabled the planar external-input path");
    scaled_inference.stop();
}

} // namespace

int main() {
    try {
        test_hdr_planar_bypass_snapshot_contract();
        test_full_frame_resize_rebuilds_with_fresh_dimensions();
        test_fixed_roi_unavailability_does_not_rebuild();
        test_automatic_planar_capability_and_build_fallbacks();
        test_replacement_planar_activation_falls_back_on_unsupported();
        test_runtime_planar_failure_is_sticky_native();
        test_duplication_access_lost_rebuilds_without_planar_fallback();
        test_empty_full_frame_dimensions_wait_without_retry_debt();
        test_nontransient_initial_failures_do_not_retry();
        test_critical_planar_failures_are_not_fallbacks();
        test_access_lost_planar_build_retries_without_fallback();
        test_initial_build_failures_follow_retry_policy();
        test_nontransient_replacement_stops_retry_episode();
        test_rebuild_failures_are_abandoned_before_success();
        test_retry_exhaustion_is_terminal();
        test_target_closed_does_not_rebuild();
        test_monitor_factory_and_planar_validation();
        std::cout << "recoverable GPU pipeline tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        internal::set_recoverable_gpu_pipeline_test_hook(nullptr);
        std::cerr << "recoverable GPU pipeline test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
