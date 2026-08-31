#ifndef FLUXCAP_GPU_DESKTOP_DUPLICATION_CONTROLLER_HPP
#define FLUXCAP_GPU_DESKTOP_DUPLICATION_CONTROLLER_HPP

#include "desktop_duplication_policy.hpp"
#include "session_event_monitor.hpp"
#include "shared_frame_bus.hpp"
#include "side_data_geometry.hpp"

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fluxcap::gpu {

using Microsoft::WRL::ComPtr;

WgcResult make_controller_result(
    WgcStatus status, HRESULT hresult = S_OK, std::string message = {});

struct ControllerMonitorSession final {
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    RECT desktop_coordinates{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool has_content = false;
};

struct DesktopDuplicationControllerState final {
    ~DesktopDuplicationControllerState() { stop(); }

    WgcResult initialize(
        const std::shared_ptr<SharedFrameBusPublisherState>& source_bus,
        const WgcCaptureOptions& source_options);
    WgcResult create_sessions(const std::stop_token* token);
    WgcResult start();
    void stop() noexcept;
    WgcResult process_iteration(std::stop_token& token);
    WgcResult publish_compose(bool any_frame);
    WgcResult rebuild_topology(std::stop_token& token);
    WgcResult build_cursor(WgcCursorInfo& cursor, WgcCursorShape& pending);
    WgcResult publish_heartbeat();

    WgcCaptureOptions options{};
    SharedFrameBusConfig bus_config{};
    std::shared_ptr<SharedFrameBusPublisherState> bus_state;
    std::uint64_t bus_producer_token = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> compose_texture;
    GpuTransform transform;
    std::vector<ControllerMonitorSession> monitors;
    RECT virtual_bounds{};
    DXGI_COLOR_SPACE_TYPE bus_color = DXGI_COLOR_SPACE_CUSTOM;
    WgcPixelFormat resolved_pixel_format = WgcPixelFormat::bgra8;
    DesktopDuplicationColorState color_state{};
    WgcCursorShape current_cursor_shape{};
    WgcCursorShape composite_shape{};
    HCURSOR last_win32_cursor = nullptr;
    std::uint64_t qpc_frequency = 0;
    std::uint64_t epoch = 1;
    std::uint64_t epoch_nonce = 1;
    std::uint64_t previous_bus_sequence = 0;
    std::uint64_t last_publish_qpc = 0;
    SharedFrameBusFrameMetadata heartbeat_metadata{};
    bool heartbeat_metadata_valid = false;
    bool first_session = true;
    bool bus_planar = false;
    bool first_frame = true;
    WgcFrameDamage pending_damage{};
    bool pending_damage_overflow = false;
    internal::SessionEventMonitor session_events;

    std::mutex lifecycle_mutex;
    std::mutex error_mutex;
    std::jthread worker;
    WgcResult last_failure{};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> running{false};
    std::atomic<bool> target_closed_flag{false};
    std::atomic<std::uint64_t> received_frames{0};
    std::atomic<std::uint64_t> published_frames{0};
    std::atomic<std::uint64_t> skipped_no_buffer{0};
    std::atomic<std::uint64_t> dropped_at_source{0};
    std::atomic<std::uint64_t> ingress_copy_submissions{0};
    std::atomic<std::uint64_t> ingress_transform_submissions{0};
    std::atomic<std::uint64_t> native_damage_frames{0};
    std::atomic<std::uint64_t> full_damage_frames{0};
    std::atomic<std::uint64_t> cursor_metadata_frames{0};
    std::atomic<std::uint64_t> cursor_shape_updates{0};
    std::atomic<std::uint64_t> session_rebuilds{0};
    std::atomic<std::uint64_t> recovery_attempts{0};
    std::atomic<std::uint64_t> recovery_successes{0};
    std::atomic<std::uint64_t> protected_content_frames{0};
    std::atomic<std::uint64_t> idle_republished_frames{0};
    std::atomic<std::uint64_t> session_event_notifications{0};
    std::atomic<std::uint64_t> format_fallbacks{0};
    std::atomic<std::uint64_t> topology_changes{0};

    void worker_main(std::stop_token token) noexcept;
    void record_error(WgcResult error) noexcept;
    bool wait_retry_delay(const std::stop_token* token, std::uint32_t ms);
    bool full_damage(WgcFrameDamage& damage) const;
};

} // namespace fluxcap::gpu

#endif
