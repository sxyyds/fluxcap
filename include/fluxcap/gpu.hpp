#ifndef FLUXCAP_GPU_HPP
#define FLUXCAP_GPU_HPP

#include <d3d11.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined(_WIN32) && defined(FLUXCAP_GPU_SHARED)
#  if defined(FLUXCAP_GPU_EXPORTS)
#    define FLUXCAP_GPU_API __declspec(dllexport)
#  else
#    define FLUXCAP_GPU_API __declspec(dllimport)
#  endif
#else
#  define FLUXCAP_GPU_API
#endif

#if defined(_MSC_VER) && defined(FLUXCAP_GPU_SHARED)
// All standard-library members are private pImpl/state handles. Their
// construction, move, and destruction stay in the DLL's out-of-line methods.
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

namespace fluxcap::gpu {

enum class GpuStatus : std::uint8_t {
    ok,
    invalid_argument,
    unsupported,
    out_of_memory,
    timeout,
    device_lost,
    system_error
};

struct GpuError final {
    GpuStatus status = GpuStatus::ok;
    HRESULT hresult = S_OK;
    std::array<char, 512> message{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == GpuStatus::ok;
    }
    [[nodiscard]] const char* what() const noexcept { return message.data(); }
};

enum class GpuPixelFormat : std::uint8_t {
    bgra8,
    nv12,
    p010,
    rgba16_float
};

// automatic probes the deterministic planar shader on the first submitted
// source/destination pair and permanently selects it or VideoProcessor for
// the lifetime of this transform (normally one capture epoch).
enum class GpuTransformBackend : std::uint8_t {
    automatic,
    video_processor,
    deterministic_planar
};

struct GpuTransformConfig final {
    std::uint32_t input_width = 0;
    std::uint32_t input_height = 0;
    // Zero region width/height selects the full input. A non-zero region is
    // cropped by the video processor in the same output write as conversion.
    std::uint32_t input_region_x = 0;
    std::uint32_t input_region_y = 0;
    std::uint32_t input_region_width = 0;
    std::uint32_t input_region_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    GpuPixelFormat output_format = GpuPixelFormat::bgra8;
    std::uint32_t frame_rate_numerator = 60;
    std::uint32_t frame_rate_denominator = 1;
    bool full_range_yuv = false;
    DXGI_FORMAT input_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_COLOR_SPACE_TYPE input_color_space =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    // CUSTOM preserves the input color space for FP16 RGB output, maps linear
    // scRGB FP16 to studio-range PQ/P2020 P010, and otherwise selects SDR from
    // output_format/full_range_yuv.
    DXGI_COLOR_SPACE_TYPE output_color_space = DXGI_COLOR_SPACE_CUSTOM;
    GpuTransformBackend backend = GpuTransformBackend::automatic;
    // process_into()-only users can avoid allocating an otherwise unused
    // full-frame output texture. process() is invalid in this mode.
    bool external_output_only = false;
};

class FLUXCAP_GPU_API GpuTransform final {
public:
    GpuTransform() noexcept;
    ~GpuTransform();
    GpuTransform(const GpuTransform&) = delete;
    GpuTransform& operator=(const GpuTransform&) = delete;
    GpuTransform(GpuTransform&&) noexcept;
    GpuTransform& operator=(GpuTransform&&) noexcept;
    [[nodiscard]] static GpuError create(
        ID3D11Device*, const GpuTransformConfig&, GpuTransform&) noexcept;
    [[nodiscard]] GpuError process(ID3D11Texture2D*) noexcept;
    // Writes directly into a caller-owned texture. The destination must be a
    // same-device, single-subresource DEFAULT texture matching the configured
    // output dimensions/format and carrying D3D11_BIND_RENDER_TARGET.
    [[nodiscard]] GpuError process_into(
        ID3D11Texture2D* source, ID3D11Texture2D* destination) noexcept;
    [[nodiscard]] ID3D11Texture2D* output_texture() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    // Counts explicit compatibility copies performed inside the transform.
    // This is normally zero. A fully typed BGRA8 SRGB source cannot be viewed
    // as UNORM by the deterministic shader and is not accepted directly by
    // every VideoProcessor driver, so automatic/video_processor may normalize
    // it into one reusable UNORM input before the VP submission.
    [[nodiscard]] std::uint64_t compatibility_copy_submissions() const noexcept;
    [[nodiscard]] GpuTransformConfig config() const noexcept;
    // automatic is returned until the first source/destination pair fixes the
    // backend. spatially_deterministic() is true only after the plane-RTV
    // backend has been selected (or for explicit deterministic mode).
    [[nodiscard]] GpuTransformBackend active_backend() const noexcept;
    [[nodiscard]] bool spatially_deterministic() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct GpuCropConfig final {
    std::uint32_t input_width = 0;
    std::uint32_t input_height = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::uint32_t bind_flags = D3D11_BIND_SHADER_RESOURCE;
};

class FLUXCAP_GPU_API GpuCrop final {
public:
    GpuCrop() noexcept;
    ~GpuCrop();
    GpuCrop(const GpuCrop&) = delete;
    GpuCrop& operator=(const GpuCrop&) = delete;
    GpuCrop(GpuCrop&&) noexcept;
    GpuCrop& operator=(GpuCrop&&) noexcept;
    [[nodiscard]] static GpuError create(
        ID3D11Device*, const GpuCropConfig&, GpuCrop&) noexcept;
    // The output is a single reusable texture. Callers must serialize its
    // consumption with the next process() submission.
    [[nodiscard]] GpuError process(ID3D11Texture2D*) noexcept;
    [[nodiscard]] ID3D11Texture2D* output_texture() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] GpuCropConfig config() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct SharedTextureConfig final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::uint32_t bind_flags = D3D11_BIND_SHADER_RESOURCE;
    std::uint64_t producer_key = 0;
    std::uint64_t consumer_key = 1;
};

struct SharedTextureExport final {
    std::uint32_t structure_size = sizeof(SharedTextureExport);
    std::uint32_t protocol_version = 1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t adapter_luid_low = 0;
    std::int32_t adapter_luid_high = 0;
    std::uint32_t reserved = 0;
    std::uint64_t handle_value = 0;
    std::uint64_t producer_key = 0;
    std::uint64_t consumer_key = 0;
    std::uint64_t publish_sequence = 0;
};

class FLUXCAP_GPU_API SharedTexturePublisher final {
public:
    SharedTexturePublisher() noexcept;
    ~SharedTexturePublisher();
    SharedTexturePublisher(const SharedTexturePublisher&) = delete;
    SharedTexturePublisher& operator=(const SharedTexturePublisher&) = delete;
    SharedTexturePublisher(SharedTexturePublisher&&) noexcept;
    SharedTexturePublisher& operator=(SharedTexturePublisher&&) noexcept;
    [[nodiscard]] static GpuError create(
        ID3D11Device*, const SharedTextureConfig&, SharedTexturePublisher&) noexcept;
    [[nodiscard]] GpuError publish(ID3D11Texture2D*, std::uint32_t timeout_ms) noexcept;
    [[nodiscard]] GpuError export_to_process(HANDLE, SharedTextureExport&) const noexcept;
    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] SharedTextureConfig config() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class FLUXCAP_GPU_API SharedTextureConsumer final {
public:
    SharedTextureConsumer() noexcept;
    ~SharedTextureConsumer();
    SharedTextureConsumer(const SharedTextureConsumer&) = delete;
    SharedTextureConsumer& operator=(const SharedTextureConsumer&) = delete;
    SharedTextureConsumer(SharedTextureConsumer&&) noexcept;
    SharedTextureConsumer& operator=(SharedTextureConsumer&&) noexcept;
    // metadata.handle_value must be valid in the current process. When
    // take_handle_ownership is true, open() closes that NT handle.
    [[nodiscard]] static GpuError open(
        ID3D11Device*, const SharedTextureExport&, bool take_handle_ownership,
        SharedTextureConsumer&) noexcept;
    [[nodiscard]] GpuError acquire(std::uint32_t timeout_ms) noexcept;
    [[nodiscard]] GpuError release() noexcept;
    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] bool acquired() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

inline constexpr std::uint32_t shared_frame_bus_max_slots = 8;
inline constexpr std::uint32_t shared_frame_bus_max_consumers = 8;
inline constexpr std::uint32_t shared_frame_bus_move_result_slots = 16;
inline constexpr std::uint32_t shared_frame_bus_protocol_version = 5;
inline constexpr std::uint32_t shared_frame_bus_metadata_version = 1;

inline constexpr std::uint64_t shared_frame_bus_metadata_source_timestamp = 1ull << 0;
inline constexpr std::uint64_t shared_frame_bus_metadata_qpc = 1ull << 1;
inline constexpr std::uint64_t shared_frame_bus_metadata_source_dimensions = 1ull << 2;
inline constexpr std::uint64_t shared_frame_bus_metadata_roi = 1ull << 3;
inline constexpr std::uint64_t shared_frame_bus_metadata_mailbox_generation = 1ull << 4;
inline constexpr std::uint64_t shared_frame_bus_metadata_color_space = 1ull << 5;

struct SharedFrameBusFrameMetadata final {
    std::uint32_t structure_size = sizeof(SharedFrameBusFrameMetadata);
    std::uint32_t metadata_version = shared_frame_bus_metadata_version;
    std::uint64_t valid_fields = 0;
    std::int64_t source_timestamp_100ns = 0;
    std::uint64_t timestamp_qpc = 0;
    std::uint64_t qpc_frequency = 0;
    std::uint64_t mailbox_generation = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    // ROI is expressed in source coordinates. Protocol v5 permits its size to
    // differ from SharedFrameBusFrameInfo dimensions after a fused scale.
    std::uint32_t roi_x = 0;
    std::uint32_t roi_y = 0;
    std::uint32_t roi_width = 0;
    std::uint32_t roi_height = 0;
    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_CUSTOM;
    std::uint32_t reserved_0 = 0;
    std::array<std::uint64_t, 2> reserved{};
};

static_assert(std::is_standard_layout_v<SharedFrameBusFrameMetadata>);
static_assert(std::is_trivially_copyable_v<SharedFrameBusFrameMetadata>);
static_assert(sizeof(SharedFrameBusFrameMetadata) == 96);
static_assert(alignof(SharedFrameBusFrameMetadata) == 8);
static_assert(offsetof(SharedFrameBusFrameMetadata, valid_fields) == 8);
static_assert(offsetof(SharedFrameBusFrameMetadata, source_timestamp_100ns) == 16);
static_assert(offsetof(SharedFrameBusFrameMetadata, mailbox_generation) == 40);
static_assert(offsetof(SharedFrameBusFrameMetadata, source_width) == 48);
static_assert(offsetof(SharedFrameBusFrameMetadata, color_space) == 72);
static_assert(offsetof(SharedFrameBusFrameMetadata, reserved) == 80);

struct SharedFrameBusConfig final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // CUSTOM lets the producer derive a format-appropriate default. A concrete
    // value becomes the required per-frame color contract.
    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_CUSTOM;
    // RENDER_TARGET keeps BGRA slots directly usable as VideoProcessor input
    // on drivers that reject shader-resource-only shared textures.
    std::uint32_t bind_flags = D3D11_BIND_SHADER_RESOURCE
        | D3D11_BIND_RENDER_TARGET;
    std::uint32_t slot_count = 4;
    std::uint32_t max_consumers = shared_frame_bus_max_consumers;
};

// Control, process, and fence handles are duplicated into the target process.
// texture_handles are legacy DXGI shared identifiers: never CloseHandle them.
// take_handle_ownership closes every duplicated NT handle on success or error.
struct SharedFrameBusRegistration final {
    std::uint32_t structure_size = sizeof(SharedFrameBusRegistration);
    std::uint32_t protocol_version = shared_frame_bus_protocol_version;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t slot_count = 0;
    std::uint32_t consumer_index = 0;
    std::uint32_t target_process_id = 0;
    std::uint32_t adapter_luid_low = 0;
    std::int32_t adapter_luid_high = 0;
    std::uint64_t consumer_token = 0;
    std::uint64_t control_mapping_handle = 0;
    std::uint64_t publisher_process_handle = 0;
    std::uint64_t ready_fence_handle = 0;
    std::uint64_t done_fence_handle = 0;
    std::array<std::uint64_t, shared_frame_bus_max_slots> texture_handles{};
};

struct SharedFrameBusFrameInfo final {
    std::uint32_t slot_index = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t sequence = 0;
};

struct SharedFrameBusStats final {
    std::uint64_t publish_attempts = 0;
    std::uint64_t published_frames = 0;
    std::uint64_t copied_publishes = 0;
    std::uint64_t direct_publishes = 0;
    std::uint64_t no_slot = 0;
    std::uint64_t reused_ready_slots = 0;
    std::uint64_t consumer_registrations = 0;
    std::uint64_t consumer_reclamations = 0;
    std::uint64_t dead_consumer_reclamations = 0;
    std::uint64_t move_results_published = 0;
    std::uint64_t move_result_overwrites = 0;
    std::uint32_t quarantined_slots = 0;
    std::uint32_t active_consumers = 0;
};

struct SharedFrameBusPublisherState;
struct SharedFrameBusConsumerState;
struct SharedFrameBusFrameSideData;
struct SharedFrameBusMoveResult;
struct WgcCursorShape;
class WgcCapture;
class DesktopDuplicationCapture;

class FLUXCAP_GPU_API SharedFrameBusWriteLease final {
public:
    SharedFrameBusWriteLease() noexcept;
    ~SharedFrameBusWriteLease();
    SharedFrameBusWriteLease(const SharedFrameBusWriteLease&) = delete;
    SharedFrameBusWriteLease& operator=(const SharedFrameBusWriteLease&) = delete;
    SharedFrameBusWriteLease(SharedFrameBusWriteLease&&) noexcept;
    SharedFrameBusWriteLease& operator=(SharedFrameBusWriteLease&&) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    [[nodiscard]] std::uint32_t slot_index() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    void reset() noexcept;
private:
    friend class SharedFrameBusPublisher;
    friend struct SharedFrameBusPublisherState;
    std::shared_ptr<SharedFrameBusPublisherState> state_;
    ID3D11Texture2D* texture_ = nullptr;
    std::uint64_t token_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint32_t slot_ = 0;
};

class FLUXCAP_GPU_API SharedFrameBusFrameLease final {
public:
    SharedFrameBusFrameLease() noexcept;
    ~SharedFrameBusFrameLease();
    SharedFrameBusFrameLease(const SharedFrameBusFrameLease&) = delete;
    SharedFrameBusFrameLease& operator=(const SharedFrameBusFrameLease&) = delete;
    SharedFrameBusFrameLease(SharedFrameBusFrameLease&&) noexcept;
    SharedFrameBusFrameLease& operator=(SharedFrameBusFrameLease&&) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    [[nodiscard]] const SharedFrameBusFrameInfo& info() const noexcept;
    [[nodiscard]] SharedFrameBusFrameMetadata metadata() const noexcept;
    [[nodiscard]] SharedFrameBusFrameSideData side_data() const noexcept;
    [[nodiscard]] GpuError release() noexcept;
    void reset() noexcept;
private:
    friend class SharedFrameBusConsumer;
    friend struct SharedFrameBusConsumerState;
    std::shared_ptr<SharedFrameBusConsumerState> state_;
    ID3D11Texture2D* texture_ = nullptr;
    SharedFrameBusFrameInfo info_{};
};

// Fixed-format, fixed-adapter, latest-wins GPU broadcast ring. publish() does
// one CopyResource into a reusable slot. begin_publish()/commit() lets an
// upstream GPU stage write that slot directly and removes even that copy.
class FLUXCAP_GPU_API SharedFrameBusPublisher final {
public:
    SharedFrameBusPublisher() noexcept;
    ~SharedFrameBusPublisher();
    SharedFrameBusPublisher(const SharedFrameBusPublisher&) = delete;
    SharedFrameBusPublisher& operator=(const SharedFrameBusPublisher&) = delete;
    SharedFrameBusPublisher(SharedFrameBusPublisher&&) noexcept;
    SharedFrameBusPublisher& operator=(SharedFrameBusPublisher&&) noexcept;
    [[nodiscard]] static GpuError create(
        ID3D11Device*, const SharedFrameBusConfig&, SharedFrameBusPublisher&) noexcept;
    [[nodiscard]] GpuError register_consumer(
        HANDLE target_process, SharedFrameBusRegistration&) noexcept;
    // Invalidates new acquisitions immediately. The index is released only
    // after in-flight acquire calls leave and its done fence covers all readers.
    [[nodiscard]] GpuError unregister_consumer(
        const SharedFrameBusRegistration&, std::uint32_t timeout_ms = 0) noexcept;
    [[nodiscard]] GpuError publish(
        ID3D11Texture2D*, std::uint32_t timeout_ms = 0) noexcept;
    [[nodiscard]] GpuError publish(
        ID3D11Texture2D*, const SharedFrameBusFrameMetadata&,
        std::uint32_t timeout_ms = 0) noexcept;
    [[nodiscard]] GpuError publish(
        ID3D11Texture2D*, const SharedFrameBusFrameMetadata&,
        const SharedFrameBusFrameSideData&,
        std::uint32_t timeout_ms = 0) noexcept;
    [[nodiscard]] GpuError begin_publish(
        std::uint32_t timeout_ms, SharedFrameBusWriteLease&) noexcept;
    // Direct writes must already be queued on this device's immediate context.
    // Execute deferred command lists on it before commit().
    [[nodiscard]] GpuError commit(SharedFrameBusWriteLease&&) noexcept;
    [[nodiscard]] GpuError commit(
        SharedFrameBusWriteLease&&,
        const SharedFrameBusFrameMetadata&) noexcept;
    [[nodiscard]] GpuError commit(
        SharedFrameBusWriteLease&&,
        const SharedFrameBusFrameMetadata&,
        const SharedFrameBusFrameSideData&) noexcept;
    [[nodiscard]] GpuError publish_cursor_shape(
        const WgcCursorShape&) noexcept;
    [[nodiscard]] GpuError publish_move_result(
        const SharedFrameBusMoveResult&) noexcept;
    [[nodiscard]] SharedFrameBusConfig config() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] SharedFrameBusStats stats() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    friend class WgcCapture;
    friend class DesktopDuplicationCapture;
    friend class DesktopDuplicationController;
    friend class GdiMonitorCapture;
    std::shared_ptr<SharedFrameBusPublisherState> state_;
};

class FLUXCAP_GPU_API SharedFrameBusConsumer final {
public:
    SharedFrameBusConsumer() noexcept;
    ~SharedFrameBusConsumer();
    SharedFrameBusConsumer(const SharedFrameBusConsumer&) = delete;
    SharedFrameBusConsumer& operator=(const SharedFrameBusConsumer&) = delete;
    SharedFrameBusConsumer(SharedFrameBusConsumer&&) noexcept;
    SharedFrameBusConsumer& operator=(SharedFrameBusConsumer&&) noexcept;
    [[nodiscard]] static GpuError open(
        ID3D11Device*, const SharedFrameBusRegistration&,
        bool take_handle_ownership, SharedFrameBusConsumer&) noexcept;
    // Queue every GPU use of lease.texture() on this device's immediate
    // context before release(). Execute deferred command lists first so the
    // done-fence signal is ordered after all reads.
    [[nodiscard]] GpuError acquire_latest(
        std::uint32_t timeout_ms, SharedFrameBusFrameLease&) noexcept;
    [[nodiscard]] GpuError release(SharedFrameBusFrameLease&) noexcept;
    [[nodiscard]] GpuError cursor_shape(
        std::uint64_t shape_sequence, WgcCursorShape&) const noexcept;
    [[nodiscard]] GpuError try_get_move_result(
        std::uint64_t epoch,
        std::uint64_t epoch_nonce,
        std::uint64_t sequence,
        SharedFrameBusMoveResult&) const noexcept;
    [[nodiscard]] GpuError close() noexcept;
    [[nodiscard]] std::uint32_t consumer_index() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    std::shared_ptr<SharedFrameBusConsumerState> state_;
};

enum class WgcStatus : std::uint8_t {
    ok,
    invalid_argument,
    invalid_state,
    not_supported,
    timeout,
    target_closed,
    no_buffer,
    out_of_memory,
    d3d_error,
    capture_error,
    region_unavailable
};

struct WgcResult final {
    WgcStatus status = WgcStatus::ok;
    HRESULT hresult = S_OK;
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == WgcStatus::ok;
    }
};

enum class WgcPixelFormat : std::uint8_t {
    bgra8,
    rgba16_float
};

enum class WgcDamageMode : std::uint8_t {
    disabled,
    native_report_only,
    native_with_inferred_moves
};

struct WgcCaptureOptions final {
    std::uint32_t buffer_count = 3;
    bool include_cursor = true;
    bool require_border = false;
    bool include_secondary_windows = false;
    std::uint32_t min_update_interval_us = 0;
    WgcPixelFormat pixel_format = WgcPixelFormat::bgra8;
    WgcDamageMode damage_mode = WgcDamageMode::native_report_only;
    // Independent cursor metadata disables WGC's baked-in cursor pixels.
    bool include_cursor_metadata = false;
    std::uint32_t cursor_shape_refresh_interval_ms = 16;
    // Applies only when a direct bus requires RGB->NV12/P010 conversion.
    // automatic fixes the selected backend on the first frame of each epoch.
    GpuTransformBackend planar_transform_backend =
        GpuTransformBackend::automatic;
    std::uint64_t capture_epoch = 1;
    std::uint64_t capture_epoch_nonce = 1;
    // ---- Desktop Duplication / GDI resilience knobs (ignored by WGC) ----
    // AcquireNextFrame / poll timeout per iteration.
    std::uint32_t frame_timeout_ms = 50;
    // Worker-side DXGI_ERROR_ACCESS_LOST session rebuild attempts. Zero
    // retries indefinitely with bounded backoff, matching OBS behaviour.
    std::uint32_t access_lost_retry_limit = 0;
    std::uint32_t access_lost_retry_initial_ms = 25;
    std::uint32_t access_lost_retry_max_ms = 1'000;
    // DuplicateOutput retries while another process holds the session limit
    // (DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, common around exclusive
    // fullscreen transitions).
    std::uint32_t session_retry_limit = 3;
    std::uint32_t session_retry_interval_ms = 100;
    // Re-publish the last committed frame while the desktop is static so
    // downstream encoders keep receiving frames. Zero disables.
    std::uint32_t idle_republish_interval_ms = 0;
    // Resolve pixel_format from the output's live color space
    // (IDXGIOutput6::GetDesc1) instead of trusting the declared format.
    bool auto_detect_color = false;
    // Accept B8G8R8A8 duplication when the driver rejects the requested
    // R16G16B16A16_FLOAT (fail-closed by default).
    bool allow_format_fallback = false;
    // React to lock/RDP/power session notifications with a full-frame
    // refresh and cursor re-probe.
    bool monitor_session_events = false;
    // GDI backend only: poll cadence in milliseconds.
    std::uint32_t gdi_poll_interval_ms = 33;
};

// The mailbox region controls FluxCap's copy into its private texture ring or
// a direct SharedFrameBus slot. WGC still supplies a full capture surface.
enum class WgcMailboxMode : std::uint8_t {
    full_frame,
    absolute_region,
    centered_region
};

struct WgcMailboxConfig final {
    WgcMailboxMode mode = WgcMailboxMode::full_frame;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct WgcMailboxFrameInfo final {
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t generation = 0;
};

struct WgcMailboxState final {
    WgcMailboxConfig config{};
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint64_t generation = 0;
    bool region_available = false;
};

inline constexpr std::uint32_t wgc_max_dirty_rects = 64;
inline constexpr std::uint32_t wgc_max_move_rects = 16;

struct WgcRect final {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct WgcMoveRect final {
    std::int32_t source_x = 0;
    std::int32_t source_y = 0;
    WgcRect destination{};
};

inline constexpr std::uint32_t wgc_damage_valid = 1u << 0;
inline constexpr std::uint32_t wgc_damage_native = 1u << 1;
inline constexpr std::uint32_t wgc_damage_full_frame = 1u << 2;
inline constexpr std::uint32_t wgc_damage_overflow = 1u << 3;
inline constexpr std::uint32_t wgc_damage_discontinuity = 1u << 4;
inline constexpr std::uint32_t wgc_damage_native_move_unavailable = 1u << 5;
inline constexpr std::uint32_t wgc_damage_inferred_move = 1u << 6;
inline constexpr std::uint32_t wgc_damage_move_pending = 1u << 7;
// Native move metadata was available for this frame. move_count may be zero.
inline constexpr std::uint32_t wgc_damage_native_move_available = 1u << 8;

struct WgcFrameDamage final {
    std::uint64_t base_sequence = 0;
    std::uint32_t dirty_count = 0;
    std::uint32_t move_count = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
    std::array<WgcRect, wgc_max_dirty_rects> dirty_rects{};
    std::array<WgcMoveRect, wgc_max_move_rects> move_rects{};
};

inline constexpr std::uint32_t shared_frame_bus_move_result_version = 1;
inline constexpr std::uint32_t shared_frame_bus_move_result_valid = 1u << 0;
inline constexpr std::uint32_t shared_frame_bus_move_result_inferred = 1u << 1;
inline constexpr std::uint32_t shared_frame_bus_move_result_fail_closed = 1u << 2;
inline constexpr std::uint32_t shared_frame_bus_move_result_capacity_exceeded =
    1u << 3;

// Protocol-v4 asynchronous damage amendment. It is deliberately separate
// from the texture sidecar: inference may finish after the frame is committed,
// so consumers must query the exact epoch/nonce/sequence instead of attaching
// a late result to whichever frame happens to arrive next.
struct SharedFrameBusMoveResult final {
    std::uint32_t structure_size = sizeof(SharedFrameBusMoveResult);
    std::uint32_t result_version = shared_frame_bus_move_result_version;
    std::uint64_t epoch = 0;
    std::uint64_t epoch_nonce = 0;
    std::uint64_t sequence = 0;
    std::uint64_t base_sequence = 0;
    std::uint32_t move_count = 0;
    std::uint32_t flags = 0;
    std::array<std::uint64_t, 2> reserved{};
    std::array<WgcMoveRect, wgc_max_move_rects> move_rects{};
};

static_assert(std::is_standard_layout_v<SharedFrameBusMoveResult>);
static_assert(std::is_trivially_copyable_v<SharedFrameBusMoveResult>);
static_assert(sizeof(SharedFrameBusMoveResult) == 448);

enum class WgcCursorShapeKind : std::uint8_t {
    none,
    color_bgra8,
    monochrome_and_xor,
    // DXGI masked-color pointers use the alpha byte to select replace versus
    // XOR semantics and cannot be flattened to an ordinary color cursor.
    masked_color_bgra8
};

inline constexpr std::uint32_t wgc_cursor_visible = 1u << 0;
inline constexpr std::uint32_t wgc_cursor_position_valid = 1u << 1;
inline constexpr std::uint32_t wgc_cursor_position_estimated = 1u << 2;
inline constexpr std::uint32_t wgc_cursor_shape_pending = 1u << 3;

// Best-effort WGC session properties surface their application failures
// through WgcCaptureStats::property_failure_mask instead of failing the
// capture: older systems simply lack the property, while a set failure on a
// system that advertises it deserves caller visibility.
inline constexpr std::uint32_t wgc_property_failure_border = 1u << 0;
inline constexpr std::uint32_t wgc_property_failure_secondary_windows = 1u << 1;
inline constexpr std::uint32_t wgc_property_failure_min_update_interval = 1u << 2;
inline constexpr std::uint32_t wgc_property_failure_cursor = 1u << 3;
inline constexpr std::uint32_t wgc_property_failure_dirty_region = 1u << 4;

struct WgcCursorInfo final {
    std::uint64_t sample_qpc = 0;
    std::uint64_t shape_sequence = 0;
    // screen_* and frame_* both name the cursor hot spot, never the shape's
    // top-left. frame_* is expressed in the committed texture coordinates.
    std::int32_t screen_x = 0;
    std::int32_t screen_y = 0;
    std::int32_t frame_x = 0;
    std::int32_t frame_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t hotspot_x = 0;
    std::uint32_t hotspot_y = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};

struct WgcCursorShape final {
    std::uint64_t sequence = 0;
    WgcCursorShapeKind kind = WgcCursorShapeKind::none;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t hotspot_x = 0;
    std::uint32_t hotspot_y = 0;
    std::uint32_t stride_bytes = 0;
    // BGRA rows for color_bgra8. For monochrome_and_xor this stores a
    // top-down AND plane followed by a top-down XOR plane, both 1-bpp.
    std::vector<std::uint8_t> data;
};

inline constexpr std::uint32_t shared_frame_bus_side_data_version = 1;
inline constexpr std::uint32_t shared_frame_bus_max_cursor_shape_bytes =
    256u * 256u * 4u;

// Protocol-v3 sidecar committed atomically with the texture sequence. Older
// v1/v2 registrations expose an empty sidecar.
struct SharedFrameBusFrameSideData final {
    std::uint32_t structure_size = sizeof(SharedFrameBusFrameSideData);
    std::uint32_t side_data_version = shared_frame_bus_side_data_version;
    std::uint64_t epoch = 0;
    std::uint64_t epoch_nonce = 0;
    WgcFrameDamage damage{};
    WgcCursorInfo cursor{};
    std::array<std::uint64_t, 1> reserved{};
};

static_assert(std::is_standard_layout_v<SharedFrameBusFrameSideData>);
static_assert(std::is_trivially_copyable_v<SharedFrameBusFrameSideData>);

inline constexpr std::uint32_t wgc_frame_discontinuity = 1u << 0;

struct WgcFrameInfo final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_qpc = 0;
    std::uint64_t qpc_frequency = 0;
    std::int64_t source_timestamp_100ns = 0;
    DXGI_COLOR_SPACE_TYPE color_space =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::uint64_t epoch = 1;
    std::uint64_t epoch_nonce = 1;
    std::uint32_t flags = 0;
};

struct WgcCaptureStats final {
    std::uint64_t received_frames = 0;
    std::uint64_t published_frames = 0;
    std::uint64_t overwritten_frames = 0;
    std::uint64_t skipped_no_buffer = 0;
    std::uint64_t dropped_at_source = 0;
    std::uint64_t size_changes = 0;
    std::uint64_t ingress_copy_submissions = 0;
    std::uint64_t ingress_transform_submissions = 0;
    std::uint64_t native_damage_frames = 0;
    std::uint64_t full_damage_frames = 0;
    std::uint64_t move_inference_submissions = 0;
    std::uint64_t move_results_published = 0;
    std::uint64_t move_inference_fail_closed = 0;
    std::uint64_t move_inference_no_readback = 0;
    std::uint64_t cursor_metadata_frames = 0;
    std::uint64_t cursor_shape_updates = 0;
    std::uint64_t recovery_attempts = 0;
    std::uint64_t recovery_successes = 0;
    std::uint64_t epoch = 1;
    std::uint64_t epoch_nonce = 1;
    // Desktop Duplication / GDI resilience counters (zero for WGC).
    std::uint64_t session_rebuilds = 0;
    std::uint64_t protected_content_frames = 0;
    std::uint64_t idle_republished_frames = 0;
    std::uint64_t session_events = 0;
    std::uint64_t format_fallbacks = 0;
    // WGC best-effort property application failures (mask of the
    // wgc_property_failure_* bits) and the total failure count.
    std::uint32_t property_failure_mask = 0;
    std::uint32_t property_failures = 0;
};

struct WgcCaptureState;

class FLUXCAP_GPU_API WgcFrameLease final {
public:
    WgcFrameLease() noexcept = default;
    ~WgcFrameLease();
    WgcFrameLease(const WgcFrameLease&) = delete;
    WgcFrameLease& operator=(const WgcFrameLease&) = delete;
    WgcFrameLease(WgcFrameLease&&) noexcept;
    WgcFrameLease& operator=(WgcFrameLease&&) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    [[nodiscard]] const WgcFrameInfo& info() const noexcept;
    [[nodiscard]] WgcMailboxFrameInfo mailbox_info() const noexcept;
    [[nodiscard]] const WgcFrameDamage& damage() const noexcept;
    [[nodiscard]] const WgcCursorInfo& cursor_info() const noexcept;
    void reset() noexcept;
private:
    friend class WgcCapture;
    friend struct WgcCaptureState;
    WgcFrameLease(
        std::shared_ptr<WgcCaptureState>, std::uint32_t, std::uint64_t,
        ID3D11Texture2D*, WgcFrameInfo, WgcFrameDamage,
        WgcCursorInfo) noexcept;
    std::shared_ptr<WgcCaptureState> state_;
    std::uint32_t slot_ = 0;
    std::uint64_t token_ = 0;
    ID3D11Texture2D* texture_ = nullptr;
    WgcFrameInfo info_{};
    WgcFrameDamage damage_{};
    WgcCursorInfo cursor_{};
};

class FLUXCAP_GPU_API WgcCapture final {
public:
    WgcCapture() noexcept = default;
    ~WgcCapture();
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;
    WgcCapture(WgcCapture&&) noexcept;
    WgcCapture& operator=(WgcCapture&&) noexcept;
    // The calling thread must have a COM/WinRT apartment initialized.
    [[nodiscard]] static WgcResult create_for_window(
        HWND, ID3D11Device*, const WgcCaptureOptions&, WgcCapture&) noexcept;
    [[nodiscard]] static WgcResult create_for_window(
        HWND, ID3D11Device*, const WgcCaptureOptions&,
        const WgcMailboxConfig&, WgcCapture&) noexcept;
    [[nodiscard]] static WgcResult create_for_monitor(
        HMONITOR, ID3D11Device*, const WgcCaptureOptions&, WgcCapture&) noexcept;
    [[nodiscard]] static WgcResult create_for_monitor(
        HMONITOR, ID3D11Device*, const WgcCaptureOptions&,
        const WgcMailboxConfig&, WgcCapture&) noexcept;
    // Bus-only capture exclusively reserves the retained publisher state and
    // writes the resolved ROI directly into its slots. The bus format may
    // match options.pixel_format (one ingress copy), or be NV12/P010 (one
    // crop/color-conversion write). Its device becomes the WGC device.
    // No private WGC output ring is allocated.
    [[nodiscard]] static WgcResult create_for_window_to_bus(
        HWND, SharedFrameBusPublisher&, const WgcCaptureOptions&,
        const WgcMailboxConfig&, WgcCapture&) noexcept;
    [[nodiscard]] static WgcResult create_for_monitor_to_bus(
        HMONITOR, SharedFrameBusPublisher&, const WgcCaptureOptions&,
        const WgcMailboxConfig&, WgcCapture&) noexcept;
    [[nodiscard]] WgcResult start() noexcept;
    void stop() noexcept;
    [[nodiscard]] WgcResult acquire_latest(std::uint32_t, WgcFrameLease&) noexcept;
    // Unleased frames from the previous generation are discarded. Existing
    // leases remain valid and expose their original mailbox_info().
    [[nodiscard]] WgcResult set_mailbox_config(
        const WgcMailboxConfig&) noexcept;
    [[nodiscard]] WgcMailboxState mailbox_state() const noexcept;
    [[nodiscard]] WgcResult cursor_shape(
        std::uint64_t shape_sequence, WgcCursorShape&) const noexcept;
    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool target_closed() const noexcept;
    [[nodiscard]] WgcCaptureStats stats() const noexcept;
    [[nodiscard]] WgcResult last_error() const noexcept;
private:
    explicit WgcCapture(std::shared_ptr<WgcCaptureState>) noexcept;
    std::shared_ptr<WgcCaptureState> state_;
};

struct DesktopDuplicationCaptureState;

// Live desktop color state resolved from IDXGIOutput6::GetDesc1. `known` is
// false on pre-DXGI1_6 systems or before the first session.
struct DesktopDuplicationColorState final {
    bool known = false;
    bool hdr = false;
    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_CUSTOM;
};

// Monitor-only native DXGI capture. The desktop image is written directly into
// a SharedFrameBus slot (one required GPU copy, or one RGB->planar transform),
// while dirty/move rectangles and pointer position/shape remain independent
// side data. include_cursor composites the cursor into the bus frame via a
// GPU pass (one extra full-surface copy); include_cursor_metadata keeps the
// independent side data. ACCESS_LOST is recovered in the worker by rebuilding
// the duplication session with bounded backoff. The bus device must belong
// to the monitor's adapter.
class FLUXCAP_GPU_API DesktopDuplicationCapture final {
public:
    DesktopDuplicationCapture() noexcept = default;
    ~DesktopDuplicationCapture();
    DesktopDuplicationCapture(const DesktopDuplicationCapture&) = delete;
    DesktopDuplicationCapture& operator=(
        const DesktopDuplicationCapture&) = delete;
    DesktopDuplicationCapture(DesktopDuplicationCapture&&) noexcept;
    DesktopDuplicationCapture& operator=(
        DesktopDuplicationCapture&&) noexcept;
    [[nodiscard]] static WgcResult create_for_monitor_to_bus(
        HMONITOR, SharedFrameBusPublisher&, const WgcCaptureOptions&,
        const WgcMailboxConfig&, DesktopDuplicationCapture&) noexcept;
    [[nodiscard]] WgcResult start() noexcept;
    void stop() noexcept;
    [[nodiscard]] WgcMailboxState mailbox_state() const noexcept;
    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool target_closed() const noexcept;
    [[nodiscard]] WgcCaptureStats stats() const noexcept;
    // Successfully published frames backed by a real desktop presentation
    // (DXGI_OUTDUPL_FRAME_INFO::LastPresentTime > 0). Pointer-only updates do
    // not increment this counter.
    [[nodiscard]] std::uint64_t desktop_present_frames() const noexcept;
    [[nodiscard]] DesktopDuplicationColorState desktop_color_state()
        const noexcept;
    [[nodiscard]] WgcResult last_error() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    explicit DesktopDuplicationCapture(
        std::shared_ptr<DesktopDuplicationCaptureState>) noexcept;
    std::shared_ptr<DesktopDuplicationCaptureState> state_;
};

struct DesktopDuplicationControllerState;

// Aggregates every desktop-attached output on the bus adapter into a single
// virtual-desktop texture published to the SharedFrameBus, mirroring OBS's
// DxgiDuplicatorController. Damage rectangles are remapped into virtual
// desktop coordinates. Rotated outputs and topology changes (added/removed
// monitors, changed virtual bounds) are not supported: the session stops with
// a recoverable error so the owning epoch can be rebuilt. The mailbox must be
// full_frame; the cursor follows the same metadata contract as
// DesktopDuplicationCapture.
class FLUXCAP_GPU_API DesktopDuplicationController final {
public:
    DesktopDuplicationController() noexcept = default;
    ~DesktopDuplicationController();
    DesktopDuplicationController(const DesktopDuplicationController&) = delete;
    DesktopDuplicationController& operator=(
        const DesktopDuplicationController&) = delete;
    DesktopDuplicationController(DesktopDuplicationController&&) noexcept;
    DesktopDuplicationController& operator=(
        DesktopDuplicationController&&) noexcept;
    [[nodiscard]] static WgcResult create_for_bus(
        SharedFrameBusPublisher&, const WgcCaptureOptions&,
        DesktopDuplicationController&) noexcept;
    [[nodiscard]] WgcResult start() noexcept;
    void stop() noexcept;
    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool target_closed() const noexcept;
    [[nodiscard]] WgcCaptureStats stats() const noexcept;
    [[nodiscard]] std::uint32_t monitor_count() const noexcept;
    [[nodiscard]] DesktopDuplicationColorState desktop_color_state()
        const noexcept;
    [[nodiscard]] WgcResult last_error() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    explicit DesktopDuplicationController(
        std::shared_ptr<DesktopDuplicationControllerState>) noexcept;
    std::shared_ptr<DesktopDuplicationControllerState> state_;
};

struct GdiMonitorCaptureState;

// CPU GDI fallback that blits the monitor into a SharedFrameBus slot for
// systems where Desktop Duplication and WGC are unavailable. Every frame is
// full damage without native metadata and the bus must be BGRA8 SDR with slot
// dimensions equal to the resolved mailbox. include_cursor draws the cursor
// with DrawIcon; include_cursor_metadata follows the shared side-data
// contract. Poll cadence is options.gdi_poll_interval_ms.
class FLUXCAP_GPU_API GdiMonitorCapture final {
public:
    GdiMonitorCapture() noexcept = default;
    ~GdiMonitorCapture();
    GdiMonitorCapture(const GdiMonitorCapture&) = delete;
    GdiMonitorCapture& operator=(const GdiMonitorCapture&) = delete;
    GdiMonitorCapture(GdiMonitorCapture&&) noexcept;
    GdiMonitorCapture& operator=(GdiMonitorCapture&&) noexcept;
    [[nodiscard]] static WgcResult create_for_monitor_to_bus(
        HMONITOR, SharedFrameBusPublisher&, const WgcCaptureOptions&,
        const WgcMailboxConfig&, GdiMonitorCapture&) noexcept;
    [[nodiscard]] WgcResult start() noexcept;
    void stop() noexcept;
    [[nodiscard]] WgcMailboxState mailbox_state() const noexcept;
    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] WgcCaptureStats stats() const noexcept;
    [[nodiscard]] WgcResult last_error() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    explicit GdiMonitorCapture(
        std::shared_ptr<GdiMonitorCaptureState>) noexcept;
    std::shared_ptr<GdiMonitorCaptureState> state_;
};

FLUXCAP_GPU_API const char* wgc_status_string(WgcStatus) noexcept;

enum class VideoCodec : std::uint8_t { h264, hevc, av1 };

struct GpuHdr10StaticMetadata final {
    bool enabled = false;
    // Chromaticity coordinates use the CTA/DXGI 0..50000 representation.
    std::uint16_t red_primary_x = 35'400;
    std::uint16_t red_primary_y = 14'600;
    std::uint16_t green_primary_x = 8'500;
    std::uint16_t green_primary_y = 39'850;
    std::uint16_t blue_primary_x = 6'550;
    std::uint16_t blue_primary_y = 2'300;
    std::uint16_t white_point_x = 15'635;
    std::uint16_t white_point_y = 16'450;
    // Maximum is in nits; minimum is in units of 0.0001 nit.
    std::uint32_t max_mastering_luminance = 1'000;
    std::uint32_t min_mastering_luminance = 1;
    std::uint16_t max_content_light_level = 1'000;
    std::uint16_t max_frame_average_light_level = 400;
};

enum class GpuEncoderStatus : std::uint8_t {
    ok,
    invalid_argument,
    invalid_state,
    not_supported,
    unsupported_input_format,
    device_mismatch,
    timeout,
    callback_failed,
    out_of_memory,
    media_foundation_error,
    d3d_error,
    device_lost
};

struct GpuEncoderResult final {
    GpuEncoderStatus status = GpuEncoderStatus::ok;
    HRESULT hresult = S_OK;
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == GpuEncoderStatus::ok;
    }
};

// Evidence is cumulative but scoped. L1/L2 can be established at runtime for
// successful external submissions. L3/L4 are offline qualification results
// and must never be inferred from these runtime fields.
enum class GpuCopyEvidenceLevel : std::uint8_t {
    none = 0,
    l1_no_fluxcap_explicit_copy = 1,
    l2_external_surface_identity_and_encoder_bind = 2,
    l3_etw_no_observed_full_frame_copy = 3,
    l4_vendor_qualified = 4
};

struct GpuEncoderConfig final {
    VideoCodec codec = VideoCodec::h264;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_rate_numerator = 60;
    std::uint32_t frame_rate_denominator = 1;
    std::uint32_t bitrate = 8'000'000;
    std::uint32_t gop_size = 120;
    DXGI_FORMAT input_format = DXGI_FORMAT_NV12;
    // Colorimetry of the encoder input and compressed output. CUSTOM resolves
    // to SDR BT.709 for backward compatibility.
    DXGI_COLOR_SPACE_TYPE input_color_space = DXGI_COLOR_SPACE_CUSTOM;
    // Codec-specific profile value. Zero selects Main/Main10 from input_format.
    std::uint32_t profile = 0;
    // Static HDR10 mastering-display and content-light metadata. Valid only
    // for P010 PQ/BT.2020 HEVC or AV1 input. Media-type acceptance is a gate;
    // the produced bitstream remains the final source of truth.
    GpuHdr10StaticMetadata hdr10{};
    bool require_bitstream_color_metadata = false;
    bool require_bitstream_hdr10_metadata = false;
    // Require NV12/P010 input textures to carry D3D11_BIND_VIDEO_ENCODER.
    // This removes one common reason for an MFT/driver staging copy, but does
    // not make implementation-internal copies observable.
    bool require_video_encoder_input_bind = false;
    bool low_latency = true;
    std::uint32_t input_pool_size = 4;
    std::uint32_t output_buffer_bytes = 0;
    std::uint32_t event_timeout_ms = 5'000;
};

struct GpuEncoderSupport final {
    bool supported = false;
    bool d3d11_aware = false;
    bool asynchronous = false;
    // True when an exact NV12/P010 media type was accepted by a D3D11-aware
    // MFT and is eligible for caller-owned tracked-surface submission. A
    // specific surface can still be rejected by ProcessInput at runtime.
    bool external_planar_input = false;
    std::uint32_t matching_transform_count = 0;
    // The IMF DXGI buffer can be round-tripped to the exact caller texture
    // and subresource before ProcessInput. This proves FluxCap did not insert
    // a staging surface, but cannot reveal copies inside the MFT or driver.
    bool external_surface_identity_verifiable = false;
    bool mft_internal_copy_observable = false;
    bool hdr10_static_metadata_media_type = false;
    bool video_encoder_input_bind_supported = false;
    // Highest level that a future successful external submission could prove
    // locally. This is capability, not evidence that a submission occurred.
    GpuCopyEvidenceLevel runtime_copy_evidence_capability =
        GpuCopyEvidenceLevel::none;
};

struct GpuEncoderStats final {
    std::uint64_t copied_submissions = 0;
    // Includes encoder-owned direct leases and external tracked textures.
    std::uint64_t direct_submissions = 0;
    // Subset of direct_submissions backed by caller-owned textures.
    std::uint64_t external_submissions = 0;
    std::uint64_t external_identity_verified_submissions = 0;
    std::uint64_t external_video_encoder_bound_submissions = 0;
    std::uint64_t external_release_callbacks = 0;
    // Highest level proven for every successful external submission in this
    // encoder instance. none means no external submission has completed.
    GpuCopyEvidenceLevel external_submission_copy_evidence =
        GpuCopyEvidenceLevel::none;
};

inline constexpr std::size_t gpu_encoder_mft_friendly_name_bytes = 256;

// Identity of the exact IMFActivate selected by initialize(). Empty/GUID_NULL
// means that no encoder is currently initialized or the activation did not
// expose the corresponding standard Media Foundation attribute.
struct GpuEncoderMftIdentity final {
    GUID clsid = GUID_NULL;
    std::array<char, gpu_encoder_mft_friendly_name_bytes> friendly_name{};
};

static_assert(std::is_standard_layout_v<GpuEncoderMftIdentity>);
static_assert(std::is_trivially_copyable_v<GpuEncoderMftIdentity>);

struct EncodedPacket final {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    VideoCodec codec = VideoCodec::h264;
    std::int64_t timestamp_100ns = 0;
    std::int64_t duration_100ns = 0;
    bool keyframe = false;
    // H.264/HEVC normally use Annex-B. AV1 hardware MFT output is OBU data.
    bool annex_b = true;
};

struct EncodedVideoMetadata final {
    bool video_signal_type_present = false;
    bool color_description_present = false;
    bool full_range = false;
    std::uint8_t color_primaries = 0;
    std::uint8_t transfer_characteristics = 0;
    std::uint8_t matrix_coefficients = 0;
    bool mastering_display_present = false;
    bool content_light_level_present = false;
    GpuHdr10StaticMetadata hdr10{};
};

// Merges metadata found in one encoded packet into output. H.264/HEVC require
// Annex-B packets; AV1 uses low-overhead OBU data. Call this for every packet
// because sequence headers and static metadata units can be emitted separately.
[[nodiscard]] FLUXCAP_GPU_API GpuError inspect_encoded_packet_metadata(
    const EncodedPacket&, EncodedVideoMetadata&) noexcept;
[[nodiscard]] FLUXCAP_GPU_API GpuError validate_encoded_video_metadata(
    const EncodedVideoMetadata&, DXGI_COLOR_SPACE_TYPE,
    const GpuHdr10StaticMetadata& expected_hdr10 = {}) noexcept;

using EncodedPacketCallback = void (*)(void*, const EncodedPacket&);

struct GpuEncoderInputLeaseState;

// A tracked encoder input surface. Destroying or resetting an unsubmitted
// lease returns its pool slot, including after the originating encoder closes.
class FLUXCAP_GPU_API GpuEncoderInputLease final {
public:
    GpuEncoderInputLease() noexcept;
    ~GpuEncoderInputLease();
    GpuEncoderInputLease(const GpuEncoderInputLease&) = delete;
    GpuEncoderInputLease& operator=(const GpuEncoderInputLease&) = delete;
    GpuEncoderInputLease(GpuEncoderInputLease&&) noexcept;
    GpuEncoderInputLease& operator=(GpuEncoderInputLease&&) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    void reset() noexcept;
private:
    friend class GpuEncoder;
    std::shared_ptr<GpuEncoderInputLeaseState> state_;
};

class FLUXCAP_GPU_API GpuEncoder final {
public:
    GpuEncoder();
    ~GpuEncoder();
    GpuEncoder(const GpuEncoder&) = delete;
    GpuEncoder& operator=(const GpuEncoder&) = delete;
    GpuEncoder(GpuEncoder&&) noexcept;
    GpuEncoder& operator=(GpuEncoder&&) noexcept;
    [[nodiscard]] static GpuEncoderResult probe(
        ID3D11Device*, const GpuEncoderConfig&, GpuEncoderSupport&) noexcept;
    [[nodiscard]] GpuEncoderResult initialize(
        ID3D11Device*, const GpuEncoderConfig&, EncodedPacketCallback, void* = nullptr) noexcept;
    [[nodiscard]] GpuEncoderResult acquire_input(
        GpuEncoderInputLease&) noexcept;
    // Consumes a lease acquired from this encoder. GPU writes to lease.texture()
    // must be queued on the encoder device's immediate context before calling.
    [[nodiscard]] GpuEncoderResult submit_input(
        GpuEncoderInputLease&&, std::int64_t, std::int64_t = 0,
        bool force_keyframe = false) noexcept;
    // Consumes lifetime_token. An accepted NV12/P010 surface remains retained
    // until the hardware MFT's tracked-sample callback fires. On rejection the
    // token is released before this call returns.
    [[nodiscard]] GpuEncoderResult submit_external_texture(
        ID3D11Texture2D*, std::shared_ptr<void> lifetime_token,
        std::int64_t, std::int64_t = 0, bool force_keyframe = false,
        std::uint32_t subresource = 0) noexcept;
    [[nodiscard]] GpuEncoderResult encode_texture(
        ID3D11Texture2D*, std::int64_t, std::int64_t = 0,
        bool force_keyframe = false, std::uint32_t subresource = 0) noexcept;
    [[nodiscard]] GpuEncoderResult drain() noexcept;
    [[nodiscard]] GpuEncoderResult flush() noexcept;
    void close() noexcept;
    [[nodiscard]] GpuEncoderStats stats() const noexcept;
    [[nodiscard]] GpuEncoderMftIdentity mft_identity() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

FLUXCAP_GPU_API const char* gpu_encoder_status_string(GpuEncoderStatus) noexcept;

enum class AsyncGpuPipelineStatus : std::uint8_t {
    ok,
    invalid_argument,
    invalid_state,
    no_buffer,
    timeout,
    transform_error,
    encoder_error,
    device_lost,
    out_of_memory,
    system_error
};

struct AsyncGpuPipelineResult final {
    AsyncGpuPipelineStatus status = AsyncGpuPipelineStatus::ok;
    HRESULT hresult = S_OK;
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == AsyncGpuPipelineStatus::ok;
    }
};

struct AsyncGpuPipelineConfig final {
    GpuTransformConfig transform{};
    GpuEncoderConfig encoder{};
    std::uint32_t queue_depth = 6;
};

struct AsyncGpuPipelineStats final {
    std::uint64_t submission_attempts = 0;
    std::uint64_t accepted_frames = 0;
    std::uint64_t overwritten_frames = 0;
    std::uint64_t rejected_frames = 0;
    std::uint64_t processed_frames = 0;
    std::uint64_t failed_frames = 0;
    std::uint64_t encoded_packets = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t worker_failures = 0;
    std::uint64_t last_submitted_sequence = 0;
    std::uint64_t last_processed_sequence = 0;
    std::uint32_t maximum_ready_depth = 0;
    std::uint64_t encoder_copied_submissions = 0;
    std::uint64_t encoder_direct_submissions = 0;
    std::uint64_t encoder_external_submissions = 0;
    std::uint64_t encoder_external_identity_verified_submissions = 0;
    std::uint64_t encoder_external_video_encoder_bound_submissions = 0;
    std::uint64_t input_copy_submissions = 0;
    std::uint64_t transform_submissions = 0;
    std::uint64_t bus_epoch_changes = 0;
    std::uint64_t forced_keyframes = 0;
};

// Bounded latest-wins pipeline. submit_frame() transfers a WGC lease without
// another texture copy; submit_texture() copies into a preallocated BGRA ring.
// The worker performs transform and hardware encoding without blocking capture.
// Packet callbacks run on that worker and must not re-enter, drain, close, or
// destroy the pipeline. Copy packet bytes before returning from the callback.
class FLUXCAP_GPU_API AsyncGpuPipeline final {
public:
    AsyncGpuPipeline() noexcept;
    ~AsyncGpuPipeline();
    AsyncGpuPipeline(const AsyncGpuPipeline&) = delete;
    AsyncGpuPipeline& operator=(const AsyncGpuPipeline&) = delete;
    AsyncGpuPipeline(AsyncGpuPipeline&&) noexcept;
    AsyncGpuPipeline& operator=(AsyncGpuPipeline&&) noexcept;
    [[nodiscard]] static AsyncGpuPipelineResult create(
        ID3D11Device*, const AsyncGpuPipelineConfig&,
        EncodedPacketCallback, void*, AsyncGpuPipeline&) noexcept;
    // Transfers the consumer to the worker. The worker acquires the latest
    // bus texture directly, transforms into a tracked encoder input surface,
    // releases the bus slot after queuing that read, and submits that surface.
    [[nodiscard]] static AsyncGpuPipelineResult create_from_shared_bus(
        ID3D11Device*, SharedFrameBusConsumer&&,
        const AsyncGpuPipelineConfig&, EncodedPacketCallback, void*,
        AsyncGpuPipeline&) noexcept;
    [[nodiscard]] AsyncGpuPipelineResult submit_frame(
        WgcFrameLease&&, std::int64_t timestamp_100ns,
        std::int64_t duration_100ns = 0, bool force_keyframe = false) noexcept;
    [[nodiscard]] AsyncGpuPipelineResult submit_texture(
        ID3D11Texture2D*, std::int64_t timestamp_100ns,
        std::int64_t duration_100ns = 0, bool force_keyframe = false) noexcept;
    // drain() is terminal: it stops accepting frames, processes the newest
    // queued work, drains the encoder, and waits for the worker to finish.
    [[nodiscard]] AsyncGpuPipelineResult drain(std::uint32_t timeout_ms) noexcept;
    void close() noexcept;
    [[nodiscard]] AsyncGpuPipelineStats stats() const noexcept;
    [[nodiscard]] AsyncGpuPipelineResult last_error() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
private:
    class Impl;
    std::atomic<std::shared_ptr<Impl>> impl_{};
};

FLUXCAP_GPU_API const char* async_gpu_pipeline_status_string(
    AsyncGpuPipelineStatus) noexcept;

enum class RecoverableGpuPipelineStatus : std::uint8_t {
    ok,
    invalid_argument,
    invalid_state,
    unsupported,
    target_closed,
    build_failed,
    device_lost,
    retry_exhausted,
    out_of_memory,
    system_error
};

enum class RecoverableGpuPipelineState : std::uint8_t {
    stopped,
    starting,
    running,
    quiescing,
    retired,
    building,
    ready,
    stopping,
    failed
};

struct RecoverableGpuPipelineResult final {
    RecoverableGpuPipelineStatus status = RecoverableGpuPipelineStatus::ok;
    HRESULT hresult = S_OK;
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == RecoverableGpuPipelineStatus::ok;
    }
};

// capture_native writes WGC's BGRA8 or scRGB FP16 surface directly into the
// bus. planar_encoder_input performs crop/color conversion once while writing
// NV12/P010 bus slots; eligible slots are then tracked directly by the MFT.
enum class RecoverableGpuPipelineBusMode : std::uint8_t {
    automatic,
    capture_native,
    planar_encoder_input
};

enum class RecoverableMonitorCaptureBackend : std::uint8_t {
    // Prefer Desktop Duplication when its cursor contract can be honored and
    // fall back to WGC when native duplication is unavailable.
    automatic,
    windows_graphics_capture,
    desktop_duplication
};

struct RecoverableGpuPipelineRetryPolicy final {
    // Number of additional attempts after the first failed graph build,
    // including startup. A value of zero still performs one immediate build
    // after a running graph fails.
    std::uint32_t max_retry_count = 8;
    std::uint32_t initial_backoff_ms = 25;
    std::uint32_t maximum_backoff_ms = 1'000;
    std::uint32_t backoff_multiplier = 2;
    std::uint32_t monitor_interval_ms = 10;
};

struct RecoverableGpuPipelineConfig final {
    WgcCaptureOptions capture{};
    WgcMailboxConfig mailbox{};
    AsyncGpuPipelineConfig pipeline{};
    RecoverableGpuPipelineBusMode bus_mode =
        RecoverableGpuPipelineBusMode::automatic;
    RecoverableMonitorCaptureBackend monitor_backend =
        RecoverableMonitorCaptureBackend::automatic;
    std::uint32_t bus_slot_count = 4;
    RecoverableGpuPipelineRetryPolicy retry{};
};

struct RecoverableGpuPipelineSnapshot final {
    RecoverableGpuPipelineState state =
        RecoverableGpuPipelineState::stopped;
    std::uint64_t epoch = 0;
    std::uint64_t epoch_nonce = 0;
    std::uint64_t recovery_attempts = 0;
    std::uint64_t recovery_successes = 0;
    std::uint64_t rebuild_attempts = 0;
    std::uint64_t rebuild_failures = 0;
    std::uint32_t consecutive_retry_count = 0;
    HRESULT last_device_removed_reason = S_OK;
    DXGI_FORMAT active_bus_format = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE active_bus_color_space = DXGI_COLOR_SPACE_CUSTOM;
    bool active_external_encoder_input = false;
    RecoverableMonitorCaptureBackend active_monitor_backend =
        RecoverableMonitorCaptureBackend::windows_graphics_capture;
    bool first_recovered_frame_pending = false;
    bool capture_running = false;
    bool encoder_accepting = false;
    WgcCaptureStats capture{};
    SharedFrameBusStats bus{};
    AsyncGpuPipelineStats pipeline{};
    // Automatic mode permanently selects capture-native after a planar
    // capability, build, activation, or runtime conversion failure.
    std::uint64_t automatic_planar_fallbacks = 0;
    // Source ROI dimensions and final bus texture dimensions. They differ when
    // capture ingress performs a single crop/scale/color conversion.
    std::uint32_t active_source_width = 0;
    std::uint32_t active_source_height = 0;
    std::uint32_t active_bus_width = 0;
    std::uint32_t active_bus_height = 0;
    bool automatic_planar_disabled = false;
};

// Owns one complete local GPU epoch. Every recovery creates a fresh D3D11
// device, SharedFrameBus, WGC session, local consumer, transform, and encoder.
// Packet callbacks run on the encoder worker, never on WGC's FrameArrived
// thread. Calling stop() from the packet callback is supported and asynchronous.
class FLUXCAP_GPU_API RecoverableGpuPipeline final {
public:
    RecoverableGpuPipeline() noexcept;
    ~RecoverableGpuPipeline();
    RecoverableGpuPipeline(const RecoverableGpuPipeline&) = delete;
    RecoverableGpuPipeline& operator=(const RecoverableGpuPipeline&) = delete;
    RecoverableGpuPipeline(RecoverableGpuPipeline&&) noexcept;
    RecoverableGpuPipeline& operator=(RecoverableGpuPipeline&&) noexcept;
    [[nodiscard]] static RecoverableGpuPipelineResult create_for_window(
        HWND, const RecoverableGpuPipelineConfig&, EncodedPacketCallback,
        void*, RecoverableGpuPipeline&) noexcept;
    [[nodiscard]] static RecoverableGpuPipelineResult create_for_monitor(
        HMONITOR, const RecoverableGpuPipelineConfig&, EncodedPacketCallback,
        void*, RecoverableGpuPipeline&) noexcept;
    // start() applies the retry policy and waits until an initial epoch is
    // fully constructed and capture has started. Later recovery continues on
    // an internal control thread.
    [[nodiscard]] RecoverableGpuPipelineResult start() noexcept;
    void stop() noexcept;
    [[nodiscard]] RecoverableGpuPipelineSnapshot snapshot() const noexcept;
    [[nodiscard]] RecoverableGpuPipelineResult last_error() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool running() const noexcept;
private:
    class Impl;
    std::atomic<std::shared_ptr<Impl>> impl_{};
};

FLUXCAP_GPU_API const char* recoverable_gpu_pipeline_status_string(
    RecoverableGpuPipelineStatus) noexcept;
FLUXCAP_GPU_API const char* recoverable_gpu_pipeline_state_string(
    RecoverableGpuPipelineState) noexcept;

enum class ImageFormat : std::uint8_t { png, jpeg };
enum class ImageEncoderStatus : std::uint8_t {
    ok,
    invalid_argument,
    invalid_state,
    unsupported_texture,
    device_mismatch,
    out_of_memory,
    d3d_error,
    wic_error,
    io_error
};

struct ImageEncoderResult final {
    ImageEncoderStatus status = ImageEncoderStatus::ok;
    HRESULT hresult = S_OK;
    std::size_t bytes_written = 0;
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == ImageEncoderStatus::ok;
    }
};

struct ImageEncodeOptions final {
    ImageFormat format = ImageFormat::png;
    float jpeg_quality = 0.92F;
    bool preserve_png_alpha = true;
    double dpi_x = 96.0;
    double dpi_y = 96.0;
    std::uint32_t subresource = 0;
};

class FLUXCAP_GPU_API WicImageEncoder final {
public:
    WicImageEncoder();
    ~WicImageEncoder();
    WicImageEncoder(const WicImageEncoder&) = delete;
    WicImageEncoder& operator=(const WicImageEncoder&) = delete;
    WicImageEncoder(WicImageEncoder&&) noexcept;
    WicImageEncoder& operator=(WicImageEncoder&&) noexcept;
    [[nodiscard]] ImageEncoderResult initialize(ID3D11Device*) noexcept;
    [[nodiscard]] ImageEncoderResult encode_texture_to_file_sync(
        ID3D11Texture2D*, std::wstring_view,
        const ImageEncodeOptions& = {}) noexcept;
    [[nodiscard]] ImageEncoderResult encode_texture_to_memory_sync(
        ID3D11Texture2D*, std::vector<std::uint8_t>&,
        const ImageEncodeOptions& = {}) noexcept;
    void close() noexcept;
    [[nodiscard]] bool initialized() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

FLUXCAP_GPU_API const char* image_encoder_status_string(ImageEncoderStatus) noexcept;

} // namespace fluxcap::gpu

#if defined(_MSC_VER) && defined(FLUXCAP_GPU_SHARED)
#  pragma warning(pop)
#endif

#endif
