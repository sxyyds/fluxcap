#include <fluxcap/gpu.hpp>

#include "gpu_device_failure.hpp"

#include <d3d10.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace fluxcap::gpu {
namespace {

using Microsoft::WRL::ComPtr;

enum class SlotState : std::uint8_t { free, writing, ready, reading };

constexpr std::uint64_t kStateBits = 2;
constexpr std::uint64_t kStateMask = (1ull << kStateBits) - 1ull;
constexpr std::uint32_t kWaitInfinite = 0xffffffffu;
constexpr std::uint32_t kBusAcquirePollMs = 8;

constexpr std::uint64_t make_control(
    std::uint64_t generation, SlotState state) noexcept {
    return (generation << kStateBits) | static_cast<std::uint64_t>(state);
}

constexpr SlotState control_state(std::uint64_t control) noexcept {
    return static_cast<SlotState>(control & kStateMask);
}

constexpr std::uint64_t control_generation(std::uint64_t control) noexcept {
    return control >> kStateBits;
}

AsyncGpuPipelineResult result(
    AsyncGpuPipelineStatus status,
    HRESULT hr = S_OK,
    std::string message = {}) {
    if (message.empty()) message = async_gpu_pipeline_status_string(status);
    return {status, hr, std::move(message)};
}

AsyncGpuPipelineResult transform_result(const GpuError& error) {
    AsyncGpuPipelineStatus status = AsyncGpuPipelineStatus::transform_error;
    if (error.status == GpuStatus::device_lost) {
        status = AsyncGpuPipelineStatus::device_lost;
    } else if (error.status == GpuStatus::out_of_memory) {
        status = AsyncGpuPipelineStatus::out_of_memory;
    }
    return result(status, error.hresult, error.what());
}

AsyncGpuPipelineResult encoder_result(const GpuEncoderResult& error) {
    return result(
        internal::async_status_from_encoder(error.status),
        error.hresult,
        error.message);
}

AsyncGpuPipelineResult device_aware_result(
    ID3D11Device* device,
    AsyncGpuPipelineStatus fallback,
    HRESULT hresult,
    const char* message) {
    const auto classification = internal::classify_device_failure(
        device, hresult);
    return result(
        classification.device_lost
            ? AsyncGpuPipelineStatus::device_lost
            : fallback,
        classification.hresult,
        message);
}

DXGI_FORMAT transform_output_format(GpuPixelFormat format) noexcept {
    switch (format) {
    case GpuPixelFormat::bgra8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case GpuPixelFormat::nv12: return DXGI_FORMAT_NV12;
    case GpuPixelFormat::p010: return DXGI_FORMAT_P010;
    case GpuPixelFormat::rgba16_float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_COLOR_SPACE_TYPE resolved_transform_output_color_space(
    const GpuTransformConfig& config) noexcept {
    if (config.output_color_space != DXGI_COLOR_SPACE_CUSTOM) {
        return config.output_color_space;
    }
    switch (config.output_format) {
    case GpuPixelFormat::bgra8:
        return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    case GpuPixelFormat::rgba16_float:
        return config.input_color_space;
    case GpuPixelFormat::p010:
        if (config.input_color_space
            == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        }
        [[fallthrough]];
    case GpuPixelFormat::nv12:
        return config.full_range_yuv
            ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
            : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    default:
        return DXGI_COLOR_SPACE_CUSTOM;
    }
}

bool transform_uses_full_input_region(
    const GpuTransformConfig& config) noexcept {
    if (config.input_region_width == 0
        && config.input_region_height == 0) {
        return config.input_region_x == 0 && config.input_region_y == 0;
    }
    return config.input_region_x == 0
        && config.input_region_y == 0
        && config.input_region_width == config.input_width
        && config.input_region_height == config.input_height;
}

bool same_device(ID3D11Device* left, ID3D11Device* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> a;
    ComPtr<IUnknown> b;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b)))
        && a.Get() == b.Get();
}

AsyncGpuPipelineResult bus_result(const GpuError& error) {
    AsyncGpuPipelineStatus status = AsyncGpuPipelineStatus::system_error;
    switch (error.status) {
    case GpuStatus::invalid_argument:
        status = AsyncGpuPipelineStatus::invalid_state;
        break;
    case GpuStatus::timeout:
        status = AsyncGpuPipelineStatus::timeout;
        break;
    case GpuStatus::device_lost:
        status = AsyncGpuPipelineStatus::device_lost;
        break;
    case GpuStatus::out_of_memory:
        status = AsyncGpuPipelineStatus::out_of_memory;
        break;
    default:
        break;
    }
    return result(status, error.hresult, error.what());
}

} // namespace

class AsyncGpuPipeline::Impl final {
public:
    enum class SourceMode : std::uint8_t { submissions, shared_bus };

    struct Metadata final {
        std::int64_t timestamp_100ns = 0;
        std::int64_t duration_100ns = 0;
        bool force_keyframe = false;
    };

    struct Slot final {
        ComPtr<ID3D11Texture2D> copy_texture;
        WgcFrameLease lease;
        Metadata metadata{};
        bool uses_lease = false;
        std::atomic<std::uint64_t> control{
            make_control(0, SlotState::free)};
    };

    ~Impl() { close(); }

    AsyncGpuPipelineResult initialize(
        ID3D11Device* source_device,
        const AsyncGpuPipelineConfig& source_config,
        EncodedPacketCallback source_callback,
        void* source_callback_context) {
        return initialize_common(
            source_device,
            source_config,
            source_callback,
            source_callback_context,
            SourceMode::submissions,
            nullptr);
    }

    AsyncGpuPipelineResult initialize_from_shared_bus(
        ID3D11Device* source_device,
        SharedFrameBusConsumer&& source_consumer,
        const AsyncGpuPipelineConfig& source_config,
        EncodedPacketCallback source_callback,
        void* source_callback_context) {
        if (!source_consumer.initialized()) {
            return result(
                AsyncGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "shared frame bus consumer is uninitialized");
        }
        return initialize_common(
            source_device,
            source_config,
            source_callback,
            source_callback_context,
            SourceMode::shared_bus,
            &source_consumer);
    }

    AsyncGpuPipelineResult initialize_common(
        ID3D11Device* source_device,
        const AsyncGpuPipelineConfig& source_config,
        EncodedPacketCallback source_callback,
        void* source_callback_context,
        SourceMode source_mode,
        SharedFrameBusConsumer* source_consumer) {
        AsyncGpuPipelineConfig normalized = source_config;
        normalized.transform.output_color_space =
            resolved_transform_output_color_space(normalized.transform);
        const DXGI_FORMAT pipeline_format =
            transform_output_format(normalized.transform.output_format);
        const bool external_planar = source_mode == SourceMode::shared_bus
            && (normalized.transform.input_format == DXGI_FORMAT_NV12
                || normalized.transform.input_format == DXGI_FORMAT_P010)
            && normalized.transform.input_format
                == normalized.encoder.input_format
            && normalized.transform.input_width
                == normalized.transform.output_width
            && normalized.transform.input_height
                == normalized.transform.output_height
            && transform_uses_full_input_region(normalized.transform)
            && normalized.transform.input_color_space
                == normalized.transform.output_color_space;
        const DXGI_COLOR_SPACE_TYPE encoder_input_color = external_planar
            ? normalized.transform.input_color_space
            : normalized.transform.output_color_space;
        if (normalized.encoder.input_color_space == DXGI_COLOR_SPACE_CUSTOM) {
            normalized.encoder.input_color_space = encoder_input_color;
        }
        if (source_device == nullptr
            || source_callback == nullptr
            || source_config.queue_depth < 2
            || source_config.queue_depth > 32
            || source_config.transform.input_width == 0
            || source_config.transform.input_height == 0
            || source_config.transform.output_width == 0
            || source_config.transform.output_height == 0
            || source_config.transform.output_width != source_config.encoder.width
            || source_config.transform.output_height != source_config.encoder.height
            || pipeline_format == DXGI_FORMAT_UNKNOWN
            || pipeline_format != normalized.encoder.input_format
            || encoder_input_color == DXGI_COLOR_SPACE_CUSTOM
            || encoder_input_color == DXGI_COLOR_SPACE_RESERVED
            || normalized.encoder.input_color_space != encoder_input_color
            || source_config.encoder.frame_rate_numerator == 0
            || source_config.encoder.frame_rate_denominator == 0) {
            return result(
                AsyncGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "invalid asynchronous GPU pipeline configuration");
        }

        device_ = source_device;
        device_->GetImmediateContext(&context_);
        if (context_ == nullptr) {
            return result(
                AsyncGpuPipelineStatus::system_error,
                E_FAIL,
                "D3D11 device has no immediate context");
        }
        const AsyncGpuPipelineResult initial_device_state = device_aware_result(
            device_.Get(),
            AsyncGpuPipelineStatus::ok,
            S_OK,
            "pipeline device was already removed");
        if (!initial_device_state) return initial_device_state;
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(context_.As(&multithread))) {
            multithread->SetMultithreadProtected(TRUE);
        }

        config_ = normalized;
        callback_ = source_callback;
        callback_context_ = source_callback_context;
        source_mode_ = source_mode;
        external_planar_bus_mode_ = external_planar;
        if (source_mode_ == SourceMode::shared_bus) {
            bus_consumer_ = std::move(*source_consumer);
        } else {
            slots_.reserve(config_.queue_depth);
            for (std::uint32_t index = 0; index < config_.queue_depth; ++index) {
                auto slot = std::make_unique<Slot>();
                D3D11_TEXTURE2D_DESC description{};
                description.Width = config_.transform.input_width;
                description.Height = config_.transform.input_height;
                description.MipLevels = 1;
                description.ArraySize = 1;
                description.Format = config_.transform.input_format;
                description.SampleDesc.Count = 1;
                description.Usage = D3D11_USAGE_DEFAULT;
                description.BindFlags = D3D11_BIND_SHADER_RESOURCE
                    | D3D11_BIND_RENDER_TARGET;
                const HRESULT hr = device_->CreateTexture2D(
                    &description, nullptr, &slot->copy_texture);
                if (FAILED(hr)) {
                    return device_aware_result(
                        device_.Get(),
                        AsyncGpuPipelineStatus::out_of_memory,
                        hr,
                        "failed to allocate an asynchronous input ring texture");
                }
                slots_.push_back(std::move(slot));
            }
        }

        worker_ = std::thread([this] { worker_main(); });
        std::unique_lock state_lock(state_mutex_);
        state_cv_.wait(state_lock, [this] { return startup_complete_; });
        const AsyncGpuPipelineResult startup = startup_result_;
        state_lock.unlock();
        if (!startup) {
            close();
            return startup;
        }
        initialized_.store(true, std::memory_order_release);
        accepting_.store(true, std::memory_order_release);
        return result(AsyncGpuPipelineStatus::ok);
    }

    AsyncGpuPipelineResult submit_texture(
        ID3D11Texture2D* texture,
        std::int64_t timestamp_100ns,
        std::int64_t duration_100ns,
        bool force_keyframe) {
        submission_attempts_.fetch_add(1, std::memory_order_relaxed);
        if (source_mode_ != SourceMode::submissions) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::invalid_state,
                E_UNEXPECTED,
                "shared-bus pipeline does not accept submitted textures");
        }
        if (texture == nullptr || duration_100ns < 0) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(AsyncGpuPipelineStatus::invalid_argument, E_INVALIDARG);
        }
        if (!accepting_.load(std::memory_order_acquire)) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::invalid_state,
                E_UNEXPECTED,
                "pipeline is not accepting frames");
        }

        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        ComPtr<ID3D11Device> texture_device;
        texture->GetDevice(&texture_device);
        if (!same_device(device_.Get(), texture_device.Get())
            || description.Width != config_.transform.input_width
            || description.Height != config_.transform.input_height
            || description.MipLevels != 1
            || description.ArraySize != 1
            || (description.Format != config_.transform.input_format
                && !(config_.transform.input_format
                         == DXGI_FORMAT_B8G8R8A8_UNORM
                    && description.Format
                        == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
            || description.SampleDesc.Count != 1
            || description.SampleDesc.Quality != 0) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "submitted texture does not match the pipeline input");
        }

        std::lock_guard submit_lock(submit_mutex_);
        if (!accepting_.load(std::memory_order_acquire)) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(AsyncGpuPipelineStatus::invalid_state, E_UNEXPECTED);
        }
        const std::uint64_t sequence = next_sequence();
        if (sequence == 0) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::invalid_state,
                E_FAIL,
                "pipeline sequence space is exhausted");
        }
        Slot* slot = acquire_write_slot();
        if (slot == nullptr) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::no_buffer,
                HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES));
        }
        context_->CopyResource(slot->copy_texture.Get(), texture);
        input_copy_submissions_.fetch_add(1, std::memory_order_relaxed);
        const auto copied = internal::classify_device_failure(
            device_.Get(), S_OK);
        if (copied.device_lost) {
            slot->control.store(
                make_control(sequence, SlotState::free),
                std::memory_order_release);
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            const auto failure = result(
                AsyncGpuPipelineStatus::device_lost,
                copied.hresult,
                "device was removed during asynchronous input copy");
            record_submission_failure(failure);
            return failure;
        }
        slot->lease.reset();
        slot->uses_lease = false;
        slot->metadata = {timestamp_100ns, duration_100ns, force_keyframe};
        publish_slot(*slot, sequence);
        return result(AsyncGpuPipelineStatus::ok);
    }

    AsyncGpuPipelineResult submit_frame(
        WgcFrameLease&& frame,
        std::int64_t timestamp_100ns,
        std::int64_t duration_100ns,
        bool force_keyframe) {
        submission_attempts_.fetch_add(1, std::memory_order_relaxed);
        if (source_mode_ != SourceMode::submissions) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::invalid_state,
                E_UNEXPECTED,
                "shared-bus pipeline does not accept submitted frames");
        }
        if (!frame || frame.texture() == nullptr || duration_100ns < 0) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(AsyncGpuPipelineStatus::invalid_argument, E_INVALIDARG);
        }
        if (!accepting_.load(std::memory_order_acquire)) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(AsyncGpuPipelineStatus::invalid_state, E_UNEXPECTED);
        }
        ComPtr<ID3D11Device> frame_device;
        frame.texture()->GetDevice(&frame_device);
        if (!same_device(device_.Get(), frame_device.Get())
            || frame.info().width != config_.transform.input_width
            || frame.info().height != config_.transform.input_height
            || (frame.info().format != config_.transform.input_format
                && !(config_.transform.input_format
                        == DXGI_FORMAT_B8G8R8A8_UNORM
                    && frame.info().format
                        == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "submitted WGC lease does not match the pipeline input");
        }

        std::lock_guard submit_lock(submit_mutex_);
        if (!accepting_.load(std::memory_order_acquire)) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(AsyncGpuPipelineStatus::invalid_state, E_UNEXPECTED);
        }
        const std::uint64_t sequence = next_sequence();
        if (sequence == 0) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(AsyncGpuPipelineStatus::invalid_state, E_FAIL);
        }
        Slot* slot = acquire_write_slot();
        if (slot == nullptr) {
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::no_buffer,
                HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES));
        }
        slot->lease = std::move(frame);
        slot->uses_lease = true;
        slot->metadata = {timestamp_100ns, duration_100ns, force_keyframe};
        publish_slot(*slot, sequence);
        return result(AsyncGpuPipelineStatus::ok);
    }

    AsyncGpuPipelineResult drain(std::uint32_t timeout_ms) {
        if (!initialized_.load(std::memory_order_acquire)) {
            return result(AsyncGpuPipelineStatus::invalid_state, E_UNEXPECTED);
        }
        if (worker_.get_id() == std::this_thread::get_id()) {
            return result(
                AsyncGpuPipelineStatus::invalid_state,
                E_UNEXPECTED,
                "drain cannot be called from the packet callback");
        }
        std::unique_lock lifecycle_lock(lifecycle_mutex_);
        {
            std::lock_guard submit_lock(submit_mutex_);
            accepting_.store(false, std::memory_order_release);
            std::lock_guard work_lock(work_mutex_);
            drain_requested_.store(true, std::memory_order_release);
        }
        work_cv_.notify_all();

        std::unique_lock state_lock(state_mutex_);
        bool completed = false;
        if (timeout_ms == kWaitInfinite) {
            state_cv_.wait(state_lock, [this] { return worker_finished_; });
            completed = true;
        } else {
            completed = state_cv_.wait_for(
                state_lock,
                std::chrono::milliseconds(timeout_ms),
                [this] { return worker_finished_; });
        }
        if (!completed) {
            return result(
                AsyncGpuPipelineStatus::timeout,
                HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        }
        const AsyncGpuPipelineResult worker_result = last_error_;
        state_lock.unlock();
        if (worker_.joinable()) worker_.join();
        return worker_result;
    }

    void close() noexcept {
        try {
            std::unique_lock lifecycle_lock(lifecycle_mutex_);
            {
                std::lock_guard submit_lock(submit_mutex_);
                accepting_.store(false, std::memory_order_release);
                std::lock_guard work_lock(work_mutex_);
                stop_requested_.store(true, std::memory_order_release);
            }
            work_cv_.notify_all();
            if (worker_.joinable()
                && worker_.get_id() != std::this_thread::get_id()) {
                worker_.join();
            }
            if (bus_consumer_.initialized()) {
                (void)bus_consumer_.close();
            }
            for (auto& slot : slots_) {
                slot->lease.reset();
                slot->uses_lease = false;
                const auto control = slot->control.load(std::memory_order_relaxed);
                slot->control.store(
                    make_control(control_generation(control), SlotState::free),
                    std::memory_order_release);
            }
            ready_depth_.store(0, std::memory_order_relaxed);
            initialized_.store(false, std::memory_order_release);
        } catch (...) {
        }
    }

    AsyncGpuPipelineStats stats() const noexcept {
        return {
            submission_attempts_.load(std::memory_order_relaxed),
            accepted_frames_.load(std::memory_order_relaxed),
            overwritten_frames_.load(std::memory_order_relaxed),
            rejected_frames_.load(std::memory_order_relaxed),
            processed_frames_.load(std::memory_order_relaxed),
            failed_frames_.load(std::memory_order_relaxed),
            encoded_packets_.load(std::memory_order_relaxed),
            encoded_bytes_.load(std::memory_order_relaxed),
            worker_failures_.load(std::memory_order_relaxed),
            last_submitted_sequence_.load(std::memory_order_relaxed),
            last_processed_sequence_.load(std::memory_order_relaxed),
            maximum_ready_depth_.load(std::memory_order_relaxed),
            encoder_copied_submissions_.load(std::memory_order_relaxed),
            encoder_direct_submissions_.load(std::memory_order_relaxed),
            encoder_external_submissions_.load(std::memory_order_relaxed),
            encoder_external_identity_verified_submissions_.load(
                std::memory_order_relaxed),
            encoder_external_video_encoder_bound_submissions_.load(
                std::memory_order_relaxed),
            input_copy_submissions_.load(std::memory_order_relaxed),
            transform_submissions_.load(std::memory_order_relaxed),
            bus_epoch_changes_.load(std::memory_order_relaxed),
            forced_keyframes_.load(std::memory_order_relaxed)};
    }

    AsyncGpuPipelineResult last_error() const {
        std::lock_guard state_lock(state_mutex_);
        return last_error_;
    }

    bool initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

    bool accepting() const noexcept {
        return accepting_.load(std::memory_order_acquire);
    }

private:
    std::uint64_t next_sequence() noexcept {
        if (next_sequence_ == (std::numeric_limits<std::uint64_t>::max() >> kStateBits)) {
            return 0;
        }
        return ++next_sequence_;
    }

    Slot* acquire_write_slot() noexcept {
        for (;;) {
            for (auto& slot : slots_) {
                auto control = slot->control.load(std::memory_order_acquire);
                if (control_state(control) == SlotState::free
                    && slot->control.compare_exchange_strong(
                        control,
                        make_control(control_generation(control), SlotState::writing),
                        std::memory_order_acq_rel)) {
                    return slot.get();
                }
            }

            Slot* oldest = nullptr;
            std::uint64_t oldest_control = 0;
            std::uint64_t oldest_sequence = std::numeric_limits<std::uint64_t>::max();
            for (auto& slot : slots_) {
                const auto control = slot->control.load(std::memory_order_acquire);
                if (control_state(control) == SlotState::ready
                    && control_generation(control) < oldest_sequence) {
                    oldest = slot.get();
                    oldest_control = control;
                    oldest_sequence = control_generation(control);
                }
            }
            if (oldest == nullptr) {
                // The sole worker may have changed ready -> writing -> free
                // between the two scans. With queue_depth >= 2 and submitters
                // serialized, a writable slot must become observable shortly.
                YieldProcessor();
                continue;
            }
            if (oldest->control.compare_exchange_strong(
                    oldest_control,
                    make_control(oldest_sequence, SlotState::writing),
                    std::memory_order_acq_rel)) {
                ready_depth_.fetch_sub(1, std::memory_order_acq_rel);
                oldest->lease.reset();
                oldest->uses_lease = false;
                overwritten_frames_.fetch_add(1, std::memory_order_relaxed);
                return oldest;
            }
        }
    }

    void publish_slot(Slot& slot, std::uint64_t sequence) noexcept {
        std::lock_guard work_lock(work_mutex_);
        accepted_frames_.fetch_add(1, std::memory_order_relaxed);
        last_submitted_sequence_.store(sequence, std::memory_order_relaxed);
        const std::uint32_t depth = ready_depth_.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        auto maximum = maximum_ready_depth_.load(std::memory_order_relaxed);
        while (maximum < depth
            && !maximum_ready_depth_.compare_exchange_weak(
                maximum, depth, std::memory_order_relaxed)) {
        }
        // Metadata is complete and the ready-depth reservation is visible
        // before the slot itself becomes observable to the worker.
        slot.control.store(
            make_control(sequence, SlotState::ready),
            std::memory_order_release);
        work_cv_.notify_one();
    }

    Slot* acquire_latest_ready(std::uint64_t& sequence) noexcept {
        for (;;) {
            Slot* newest = nullptr;
            std::uint64_t newest_control = 0;
            std::uint64_t newest_sequence = 0;
            for (auto& slot : slots_) {
                const auto control = slot->control.load(std::memory_order_acquire);
                if (control_state(control) == SlotState::ready
                    && control_generation(control) >= newest_sequence) {
                    newest = slot.get();
                    newest_control = control;
                    newest_sequence = control_generation(control);
                }
            }
            if (newest == nullptr) return nullptr;
            if (!newest->control.compare_exchange_strong(
                    newest_control,
                    make_control(newest_sequence, SlotState::reading),
                    std::memory_order_acq_rel)) {
                continue;
            }
            ready_depth_.fetch_sub(1, std::memory_order_acq_rel);
            sequence = newest_sequence;

            for (auto& slot : slots_) {
                if (slot.get() == newest) continue;
                auto control = slot->control.load(std::memory_order_acquire);
                while (control_state(control) == SlotState::ready
                    && control_generation(control) < newest_sequence) {
                    if (slot->control.compare_exchange_weak(
                            control,
                            make_control(
                                control_generation(control), SlotState::writing),
                            std::memory_order_acq_rel)) {
                        ready_depth_.fetch_sub(1, std::memory_order_acq_rel);
                        slot->lease.reset();
                        slot->uses_lease = false;
                        slot->control.store(
                            make_control(
                                control_generation(control), SlotState::free),
                            std::memory_order_release);
                        overwritten_frames_.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
            return newest;
        }
    }

    void release_read_slot(Slot& slot, std::uint64_t sequence) noexcept {
        slot.lease.reset();
        slot.uses_lease = false;
        slot.control.store(
            make_control(sequence, SlotState::free),
            std::memory_order_release);
    }

    void record_encoder_submissions(
        const GpuEncoderStats& before,
        const GpuEncoderStats& after) noexcept {
        if (after.copied_submissions >= before.copied_submissions) {
            encoder_copied_submissions_.fetch_add(
                after.copied_submissions - before.copied_submissions,
                std::memory_order_relaxed);
        }
        if (after.direct_submissions >= before.direct_submissions) {
            encoder_direct_submissions_.fetch_add(
                after.direct_submissions - before.direct_submissions,
                std::memory_order_relaxed);
        }
        if (after.external_submissions >= before.external_submissions) {
            encoder_external_submissions_.fetch_add(
                after.external_submissions - before.external_submissions,
                std::memory_order_relaxed);
        }
        if (after.external_identity_verified_submissions
            >= before.external_identity_verified_submissions) {
            encoder_external_identity_verified_submissions_.fetch_add(
                after.external_identity_verified_submissions
                    - before.external_identity_verified_submissions,
                std::memory_order_relaxed);
        }
        if (after.external_video_encoder_bound_submissions
            >= before.external_video_encoder_bound_submissions) {
            encoder_external_video_encoder_bound_submissions_.fetch_add(
                after.external_video_encoder_bound_submissions
                    - before.external_video_encoder_bound_submissions,
                std::memory_order_relaxed);
        }
    }

    AsyncGpuPipelineResult process_one(
        GpuTransform& transform,
        GpuEncoder& encoder,
        Slot& slot,
        std::uint64_t sequence) {
        ID3D11Texture2D* input = slot.uses_lease
            ? slot.lease.texture()
            : slot.copy_texture.Get();
        const Metadata metadata = slot.metadata;
        if (input == nullptr) {
            release_read_slot(slot, sequence);
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            return result(
                AsyncGpuPipelineStatus::system_error,
                E_POINTER,
                "asynchronous input slot has no texture");
        }

        GpuEncoderInputLease encoder_input;
        const GpuEncoderResult acquired = encoder.acquire_input(encoder_input);
        if (!acquired) {
            release_read_slot(slot, sequence);
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            return encoder_result(acquired);
        }

        const GpuError transformed = transform.process_into(
            input,
            encoder_input.texture());
        // VideoProcessorBlt has queued its source read. Releasing the WGC
        // lease now cannot recycle the surface ahead of that GPU work.
        release_read_slot(slot, sequence);
        if (!transformed) {
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            return transform_result(transformed);
        }
        transform_submissions_.fetch_add(1, std::memory_order_relaxed);

        const GpuEncoderStats encoder_before = encoder.stats();
        const GpuEncoderResult encoded = encoder.submit_input(
            std::move(encoder_input),
            metadata.timestamp_100ns,
            metadata.duration_100ns,
            metadata.force_keyframe);
        record_encoder_submissions(encoder_before, encoder.stats());
        if (!encoded) {
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            return encoder_result(encoded);
        }
        processed_frames_.fetch_add(1, std::memory_order_relaxed);
        last_processed_sequence_.store(sequence, std::memory_order_relaxed);
        return result(AsyncGpuPipelineStatus::ok);
    }

    std::int64_t bus_frame_duration_100ns() const noexcept {
        const std::uint64_t numerator = config_.encoder.frame_rate_numerator;
        const std::uint64_t denominator = config_.encoder.frame_rate_denominator;
        const std::uint64_t rounded =
            (10'000'000ull * denominator + numerator / 2ull) / numerator;
        return static_cast<std::int64_t>(std::max<std::uint64_t>(1, rounded));
    }

    std::int64_t bus_timestamp_100ns(
        const SharedFrameBusFrameMetadata& metadata,
        std::int64_t duration_100ns) noexcept {
        std::int64_t timestamp = 0;
        if ((metadata.valid_fields
                & shared_frame_bus_metadata_source_timestamp) != 0
            && metadata.source_timestamp_100ns >= 0) {
            timestamp = metadata.source_timestamp_100ns;
        } else if (last_bus_timestamp_100ns_ >= 0) {
            timestamp = last_bus_timestamp_100ns_;
        }
        if (last_bus_timestamp_100ns_ >= 0
            && timestamp <= last_bus_timestamp_100ns_) {
            const auto maximum = std::numeric_limits<std::int64_t>::max();
            timestamp = last_bus_timestamp_100ns_ > maximum - duration_100ns
                ? maximum
                : last_bus_timestamp_100ns_ + duration_100ns;
        }
        last_bus_timestamp_100ns_ = timestamp;
        return timestamp;
    }

    AsyncGpuPipelineResult process_bus_frame(
        GpuTransform& transform,
        GpuEncoder& encoder,
        std::uint32_t timeout_ms) {
        submission_attempts_.fetch_add(1, std::memory_order_relaxed);
        SharedFrameBusFrameLease lease;
        const GpuError acquired = bus_consumer_.acquire_latest(timeout_ms, lease);
        if (!acquired) return bus_result(acquired);

        const SharedFrameBusFrameInfo info = lease.info();
        const SharedFrameBusFrameMetadata metadata = lease.metadata();
        const SharedFrameBusFrameSideData side_data = lease.side_data();
        ID3D11Texture2D* input = lease.texture();
        D3D11_TEXTURE2D_DESC description{};
        ComPtr<ID3D11Device> input_device;
        if (input != nullptr) {
            input->GetDesc(&description);
            input->GetDevice(&input_device);
        }
        if (input == nullptr
            || !same_device(device_.Get(), input_device.Get())
            || info.width != config_.transform.input_width
            || info.height != config_.transform.input_height
            || description.Width != config_.transform.input_width
            || description.Height != config_.transform.input_height
            || (description.Format != config_.transform.input_format
                && !(config_.transform.input_format
                        == DXGI_FORMAT_B8G8R8A8_UNORM
                    && description.Format
                        == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
            || description.MipLevels != 1
            || description.ArraySize != 1
            || description.SampleDesc.Count != 1) {
            const GpuError released = bus_consumer_.release(lease);
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            if (!released) return bus_result(released);
            return result(
                AsyncGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "shared frame bus texture does not match the pipeline input");
        }
        if ((metadata.valid_fields
                & shared_frame_bus_metadata_color_space) != 0
            && metadata.color_space
                != config_.transform.input_color_space) {
            const GpuError released = bus_consumer_.release(lease);
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            if (!released) return bus_result(released);
            return result(
                AsyncGpuPipelineStatus::invalid_argument,
                E_INVALIDARG,
                "shared frame bus color space does not match the pipeline input");
        }

        accepted_frames_.fetch_add(1, std::memory_order_relaxed);
        last_submitted_sequence_.store(info.sequence, std::memory_order_relaxed);
        maximum_ready_depth_.store(1, std::memory_order_relaxed);
        if (last_bus_sequence_ != 0 && info.sequence > last_bus_sequence_ + 1) {
            overwritten_frames_.fetch_add(
                info.sequence - last_bus_sequence_ - 1,
                std::memory_order_relaxed);
        }
        last_bus_sequence_ = info.sequence;
        bool force_keyframe =
            (side_data.damage.flags & wgc_damage_discontinuity) != 0;
        if (side_data.epoch != 0
            && side_data.epoch_nonce != 0
            && (side_data.epoch != last_bus_epoch_
                || side_data.epoch_nonce != last_bus_epoch_nonce_)) {
            if (last_bus_epoch_ != 0) {
                bus_epoch_changes_.fetch_add(1, std::memory_order_relaxed);
            }
            last_bus_epoch_ = side_data.epoch;
            last_bus_epoch_nonce_ = side_data.epoch_nonce;
            force_keyframe = true;
        }

        const std::int64_t duration_100ns = bus_frame_duration_100ns();
        const std::int64_t timestamp_100ns =
            bus_timestamp_100ns(metadata, duration_100ns);
        if (external_planar_bus_mode_) {
            auto lifetime = std::make_shared<SharedFrameBusFrameLease>(
                std::move(lease));
            const GpuEncoderStats encoder_before = encoder.stats();
            const GpuEncoderResult encoded = encoder.submit_external_texture(
                input,
                std::move(lifetime),
                timestamp_100ns,
                duration_100ns,
                force_keyframe);
            record_encoder_submissions(encoder_before, encoder.stats());
            if (!encoded) {
                failed_frames_.fetch_add(1, std::memory_order_relaxed);
                return encoder_result(encoded);
            }
            if (force_keyframe) {
                forced_keyframes_.fetch_add(1, std::memory_order_relaxed);
            }
            processed_frames_.fetch_add(1, std::memory_order_relaxed);
            last_processed_sequence_.store(
                info.sequence, std::memory_order_relaxed);
            return result(AsyncGpuPipelineStatus::ok);
        }

        GpuEncoderInputLease encoder_input;
        const GpuEncoderResult encoder_acquired = encoder.acquire_input(encoder_input);
        if (!encoder_acquired) {
            const GpuError released = bus_consumer_.release(lease);
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            if (!released) return bus_result(released);
            return encoder_result(encoder_acquired);
        }

        const GpuError transformed = transform.process_into(
            input,
            encoder_input.texture());
        // The done-fence signal is queued after the transform's GPU read.
        const GpuError released = bus_consumer_.release(lease);
        if (!released) {
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            return bus_result(released);
        }
        if (!transformed) {
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            return transform_result(transformed);
        }
        transform_submissions_.fetch_add(1, std::memory_order_relaxed);

        const GpuEncoderStats encoder_before = encoder.stats();
        const GpuEncoderResult encoded = encoder.submit_input(
            std::move(encoder_input),
            timestamp_100ns,
            duration_100ns,
            force_keyframe);
        record_encoder_submissions(encoder_before, encoder.stats());
        if (!encoded) {
            failed_frames_.fetch_add(1, std::memory_order_relaxed);
            return encoder_result(encoded);
        }
        if (force_keyframe) {
            forced_keyframes_.fetch_add(1, std::memory_order_relaxed);
        }
        processed_frames_.fetch_add(1, std::memory_order_relaxed);
        last_processed_sequence_.store(info.sequence, std::memory_order_relaxed);
        return result(AsyncGpuPipelineStatus::ok);
    }

    static void packet_callback(void* context, const EncodedPacket& packet) {
        auto& self = *static_cast<Impl*>(context);
        self.encoded_packets_.fetch_add(1, std::memory_order_relaxed);
        self.encoded_bytes_.fetch_add(packet.size, std::memory_order_relaxed);
        self.callback_(self.callback_context_, packet);
    }

    void publish_startup(const AsyncGpuPipelineResult& startup) noexcept {
        try {
            std::lock_guard state_lock(state_mutex_);
            if (!startup_complete_) {
                startup_result_ = startup;
                startup_complete_ = true;
            }
            state_cv_.notify_all();
        } catch (...) {
        }
    }

    // Called with submit_mutex_ held. Publish the terminal error before waking
    // the worker so a recovery supervisor cannot observe accepting=false with
    // a stale successful last_error().
    void record_submission_failure(
        const AsyncGpuPipelineResult& failure) noexcept {
        try {
            std::lock_guard state_lock(state_mutex_);
            submission_failure_ = failure;
            submission_failure_set_ = true;
            last_error_ = failure;
            state_cv_.notify_all();
        } catch (...) {
        }
        accepting_.store(false, std::memory_order_release);
        stop_requested_.store(true, std::memory_order_release);
        work_cv_.notify_all();
    }

    void finish_worker(const AsyncGpuPipelineResult& final_result) noexcept {
        {
            std::lock_guard submit_lock(submit_mutex_);
            accepting_.store(false, std::memory_order_release);
            // A submit that entered before the worker observed its failure may
            // have published while finish_worker waited for submit_mutex_.
            // Retire it immediately so no texture lease remains stranded.
            for (auto& slot : slots_) {
                auto control = slot->control.load(std::memory_order_acquire);
                while (control_state(control) == SlotState::ready) {
                    const std::uint64_t generation = control_generation(control);
                    if (slot->control.compare_exchange_weak(
                            control,
                            make_control(generation, SlotState::writing),
                            std::memory_order_acq_rel)) {
                        ready_depth_.fetch_sub(1, std::memory_order_acq_rel);
                        slot->lease.reset();
                        slot->uses_lease = false;
                        slot->control.store(
                            make_control(generation, SlotState::free),
                            std::memory_order_release);
                        overwritten_frames_.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        }
        if (!final_result) {
            worker_failures_.fetch_add(1, std::memory_order_relaxed);
        }
        try {
            std::lock_guard state_lock(state_mutex_);
            last_error_ = final_result;
            worker_finished_ = true;
            state_cv_.notify_all();
        } catch (...) {
        }
    }

    void worker_main() noexcept {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(apartment);
        try {
            if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
                const auto failure = result(
                    AsyncGpuPipelineStatus::system_error,
                    apartment,
                    "pipeline worker COM initialization failed");
                publish_startup(failure);
                finish_worker(failure);
                return;
            }

            GpuTransform transform;
            if (!external_planar_bus_mode_) {
                GpuTransformConfig transform_config = config_.transform;
                transform_config.external_output_only = true;
                const GpuError transform_created = GpuTransform::create(
                    device_.Get(), transform_config, transform);
                if (!transform_created) {
                    const auto failure = transform_result(transform_created);
                    publish_startup(failure);
                    finish_worker(failure);
                    if (uninitialize) CoUninitialize();
                    return;
                }
            }

            GpuEncoder encoder;
            const GpuEncoderResult encoder_created = encoder.initialize(
                device_.Get(),
                config_.encoder,
                &Impl::packet_callback,
                this);
            if (!encoder_created) {
                const auto failure = encoder_result(encoder_created);
                publish_startup(failure);
                finish_worker(failure);
                if (uninitialize) CoUninitialize();
                return;
            }
            publish_startup(result(AsyncGpuPipelineStatus::ok));

            AsyncGpuPipelineResult final_result = result(AsyncGpuPipelineStatus::ok);
            if (source_mode_ == SourceMode::shared_bus) {
                for (;;) {
                    if (stop_requested_.load(std::memory_order_acquire)) break;
                    const bool draining =
                        drain_requested_.load(std::memory_order_acquire);
                    final_result = process_bus_frame(
                        transform,
                        encoder,
                        draining ? 0 : kBusAcquirePollMs);
                    if (final_result.status == AsyncGpuPipelineStatus::timeout) {
                        if (!draining) continue;
                        const GpuEncoderResult drained = encoder.drain();
                        final_result = drained
                            ? result(AsyncGpuPipelineStatus::ok)
                            : encoder_result(drained);
                        break;
                    }
                    if (!final_result) break;
                }
            } else {
                for (;;) {
                    if (stop_requested_.load(std::memory_order_acquire)) break;
                    std::uint64_t sequence = 0;
                    Slot* slot = ready_depth_.load(std::memory_order_acquire) != 0
                        ? acquire_latest_ready(sequence)
                        : nullptr;
                    if (slot != nullptr) {
                        final_result = process_one(transform, encoder, *slot, sequence);
                        if (!final_result) break;
                        continue;
                    }
                    if (drain_requested_.load(std::memory_order_acquire)) {
                        const GpuEncoderResult drained = encoder.drain();
                        if (!drained) final_result = encoder_result(drained);
                        break;
                    }
                    std::unique_lock work_lock(work_mutex_);
                    work_cv_.wait(work_lock, [this] {
                        return ready_depth_.load(std::memory_order_acquire) != 0
                            || drain_requested_.load(std::memory_order_acquire)
                            || stop_requested_.load(std::memory_order_acquire);
                    });
                }
            }
            {
                std::lock_guard state_lock(state_mutex_);
                if (submission_failure_set_) {
                    final_result = submission_failure_;
                }
            }
            encoder.close();
            if (bus_consumer_.initialized()) {
                const GpuError closed = bus_consumer_.close();
                if (!closed && final_result) final_result = bus_result(closed);
            }
            finish_worker(final_result);
        } catch (const std::bad_alloc&) {
            const auto failure = result(
                AsyncGpuPipelineStatus::out_of_memory,
                E_OUTOFMEMORY,
                "pipeline worker allocation failed");
            publish_startup(failure);
            finish_worker(failure);
        } catch (...) {
            const auto failure = device_aware_result(
                device_.Get(),
                AsyncGpuPipelineStatus::system_error,
                E_FAIL,
                "unknown asynchronous GPU pipeline failure");
            publish_startup(failure);
            finish_worker(failure);
        }
        if (uninitialize) CoUninitialize();
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    AsyncGpuPipelineConfig config_{};
    EncodedPacketCallback callback_ = nullptr;
    void* callback_context_ = nullptr;
    SourceMode source_mode_ = SourceMode::submissions;
    bool external_planar_bus_mode_ = false;
    SharedFrameBusConsumer bus_consumer_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::thread worker_;

    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    AsyncGpuPipelineResult startup_result_{};
    AsyncGpuPipelineResult last_error_{};
    AsyncGpuPipelineResult submission_failure_{};
    bool startup_complete_ = false;
    bool worker_finished_ = false;
    bool submission_failure_set_ = false;
    std::mutex submit_mutex_;
    std::mutex work_mutex_;
    std::condition_variable work_cv_;
    std::mutex lifecycle_mutex_;
    std::uint64_t next_sequence_ = 0;
    std::uint64_t last_bus_sequence_ = 0;
    std::uint64_t last_bus_epoch_ = 0;
    std::uint64_t last_bus_epoch_nonce_ = 0;
    std::int64_t last_bus_timestamp_100ns_ = -1;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> drain_requested_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint32_t> ready_depth_{0};
    std::atomic<std::uint64_t> submission_attempts_{0};
    std::atomic<std::uint64_t> accepted_frames_{0};
    std::atomic<std::uint64_t> overwritten_frames_{0};
    std::atomic<std::uint64_t> rejected_frames_{0};
    std::atomic<std::uint64_t> processed_frames_{0};
    std::atomic<std::uint64_t> failed_frames_{0};
    std::atomic<std::uint64_t> encoded_packets_{0};
    std::atomic<std::uint64_t> encoded_bytes_{0};
    std::atomic<std::uint64_t> worker_failures_{0};
    std::atomic<std::uint64_t> last_submitted_sequence_{0};
    std::atomic<std::uint64_t> last_processed_sequence_{0};
    std::atomic<std::uint32_t> maximum_ready_depth_{0};
    std::atomic<std::uint64_t> encoder_copied_submissions_{0};
    std::atomic<std::uint64_t> encoder_direct_submissions_{0};
    std::atomic<std::uint64_t> encoder_external_submissions_{0};
    std::atomic<std::uint64_t>
        encoder_external_identity_verified_submissions_{0};
    std::atomic<std::uint64_t>
        encoder_external_video_encoder_bound_submissions_{0};
    std::atomic<std::uint64_t> input_copy_submissions_{0};
    std::atomic<std::uint64_t> transform_submissions_{0};
    std::atomic<std::uint64_t> bus_epoch_changes_{0};
    std::atomic<std::uint64_t> forced_keyframes_{0};
};

AsyncGpuPipeline::AsyncGpuPipeline() noexcept = default;
AsyncGpuPipeline::~AsyncGpuPipeline() { close(); }

AsyncGpuPipeline::AsyncGpuPipeline(AsyncGpuPipeline&& other) noexcept {
    impl_.store(
        other.impl_.exchange({}, std::memory_order_acq_rel),
        std::memory_order_release);
}

AsyncGpuPipeline& AsyncGpuPipeline::operator=(AsyncGpuPipeline&& other) noexcept {
    if (this != &other) {
        close();
        impl_.store(
            other.impl_.exchange({}, std::memory_order_acq_rel),
            std::memory_order_release);
    }
    return *this;
}

AsyncGpuPipelineResult AsyncGpuPipeline::create(
    ID3D11Device* device,
    const AsyncGpuPipelineConfig& config,
    EncodedPacketCallback callback,
    void* callback_context,
    AsyncGpuPipeline& output) noexcept {
    try {
        auto implementation = std::make_shared<Impl>();
        const auto initialized = implementation->initialize(
            device, config, callback, callback_context);
        if (!initialized) return initialized;
        output.close();
        output.impl_.store(std::move(implementation), std::memory_order_release);
        return result(AsyncGpuPipelineStatus::ok);
    } catch (const std::bad_alloc&) {
        return result(
            AsyncGpuPipelineStatus::out_of_memory,
            E_OUTOFMEMORY);
    } catch (...) {
        return device_aware_result(
            device,
            AsyncGpuPipelineStatus::system_error,
            E_FAIL,
            "unknown asynchronous GPU pipeline creation failure");
    }
}

AsyncGpuPipelineResult AsyncGpuPipeline::create_from_shared_bus(
    ID3D11Device* device,
    SharedFrameBusConsumer&& consumer,
    const AsyncGpuPipelineConfig& config,
    EncodedPacketCallback callback,
    void* callback_context,
    AsyncGpuPipeline& output) noexcept {
    try {
        auto implementation = std::make_shared<Impl>();
        const auto initialized = implementation->initialize_from_shared_bus(
            device,
            std::move(consumer),
            config,
            callback,
            callback_context);
        if (!initialized) return initialized;
        output.close();
        output.impl_.store(std::move(implementation), std::memory_order_release);
        return result(AsyncGpuPipelineStatus::ok);
    } catch (const std::bad_alloc&) {
        return result(
            AsyncGpuPipelineStatus::out_of_memory,
            E_OUTOFMEMORY);
    } catch (...) {
        return device_aware_result(
            device,
            AsyncGpuPipelineStatus::system_error,
            E_FAIL,
            "unknown shared-bus GPU pipeline creation failure");
    }
}

AsyncGpuPipelineResult AsyncGpuPipeline::submit_frame(
    WgcFrameLease&& frame,
    std::int64_t timestamp_100ns,
    std::int64_t duration_100ns,
    bool force_keyframe) noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    if (!implementation) return result(AsyncGpuPipelineStatus::invalid_state, E_UNEXPECTED);
    try {
        return implementation->submit_frame(
            std::move(frame), timestamp_100ns, duration_100ns, force_keyframe);
    } catch (const std::bad_alloc&) {
        return result(AsyncGpuPipelineStatus::out_of_memory, E_OUTOFMEMORY);
    } catch (...) {
        return result(AsyncGpuPipelineStatus::system_error, E_FAIL);
    }
}

AsyncGpuPipelineResult AsyncGpuPipeline::submit_texture(
    ID3D11Texture2D* texture,
    std::int64_t timestamp_100ns,
    std::int64_t duration_100ns,
    bool force_keyframe) noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    if (!implementation) return result(AsyncGpuPipelineStatus::invalid_state, E_UNEXPECTED);
    try {
        return implementation->submit_texture(
            texture, timestamp_100ns, duration_100ns, force_keyframe);
    } catch (const std::bad_alloc&) {
        return result(AsyncGpuPipelineStatus::out_of_memory, E_OUTOFMEMORY);
    } catch (...) {
        ComPtr<ID3D11Device> texture_device;
        if (texture != nullptr) texture->GetDevice(&texture_device);
        return device_aware_result(
            texture_device.Get(),
            AsyncGpuPipelineStatus::system_error,
            E_FAIL,
            "unknown asynchronous texture submission failure");
    }
}

AsyncGpuPipelineResult AsyncGpuPipeline::drain(std::uint32_t timeout_ms) noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    if (!implementation) return result(AsyncGpuPipelineStatus::invalid_state, E_UNEXPECTED);
    try {
        return implementation->drain(timeout_ms);
    } catch (const std::bad_alloc&) {
        return result(AsyncGpuPipelineStatus::out_of_memory, E_OUTOFMEMORY);
    } catch (...) {
        return result(AsyncGpuPipelineStatus::system_error, E_FAIL);
    }
}

void AsyncGpuPipeline::close() noexcept {
    auto implementation = impl_.exchange({}, std::memory_order_acq_rel);
    if (implementation) implementation->close();
}

AsyncGpuPipelineStats AsyncGpuPipeline::stats() const noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    return implementation ? implementation->stats() : AsyncGpuPipelineStats{};
}

AsyncGpuPipelineResult AsyncGpuPipeline::last_error() const noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    if (!implementation) return result(AsyncGpuPipelineStatus::invalid_state, E_UNEXPECTED);
    try {
        return implementation->last_error();
    } catch (...) {
        return result(AsyncGpuPipelineStatus::system_error, E_FAIL);
    }
}

bool AsyncGpuPipeline::initialized() const noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    return implementation && implementation->initialized();
}

bool AsyncGpuPipeline::accepting() const noexcept {
    auto implementation = impl_.load(std::memory_order_acquire);
    return implementation && implementation->accepting();
}

const char* async_gpu_pipeline_status_string(
    AsyncGpuPipelineStatus status) noexcept {
    switch (status) {
    case AsyncGpuPipelineStatus::ok: return "ok";
    case AsyncGpuPipelineStatus::invalid_argument: return "invalid argument";
    case AsyncGpuPipelineStatus::invalid_state: return "invalid state";
    case AsyncGpuPipelineStatus::no_buffer: return "no buffer";
    case AsyncGpuPipelineStatus::timeout: return "timeout";
    case AsyncGpuPipelineStatus::transform_error: return "GPU transform error";
    case AsyncGpuPipelineStatus::encoder_error: return "GPU encoder error";
    case AsyncGpuPipelineStatus::device_lost: return "device lost";
    case AsyncGpuPipelineStatus::out_of_memory: return "out of memory";
    case AsyncGpuPipelineStatus::system_error: return "system error";
    default: return "unknown asynchronous pipeline status";
    }
}

} // namespace fluxcap::gpu
