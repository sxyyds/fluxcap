#ifndef FLUXCAP_GPU_GDI_CAPTURE_HPP
#define FLUXCAP_GPU_GDI_CAPTURE_HPP

#include "shared_frame_bus.hpp"
#include "side_data_geometry.hpp"

#include <d3d11_1.h>
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

WgcResult make_gdi_result(
    WgcStatus status, HRESULT hresult = S_OK, std::string message = {});

struct GdiMonitorCaptureState final {
    ~GdiMonitorCaptureState() { stop(); }

    WgcResult initialize(
        HMONITOR source_monitor,
        const std::shared_ptr<SharedFrameBusPublisherState>& source_bus,
        const WgcCaptureOptions& source_options,
        const WgcMailboxConfig& source_mailbox);
    WgcResult start();
    void stop() noexcept;
    void worker_main(std::stop_token token) noexcept;
    WgcResult capture_once();
    WgcResult build_cursor(WgcCursorInfo& cursor, WgcCursorShape& pending);
    void record_error(WgcResult error) noexcept;

    HMONITOR monitor = nullptr;
    WgcCaptureOptions options{};
    WgcMailboxConfig mailbox_config{};
    WgcMailboxFrameInfo mailbox{};
    SharedFrameBusConfig bus_config{};
    std::shared_ptr<SharedFrameBusPublisherState> bus_state;
    std::uint64_t bus_producer_token = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging_texture;
    RECT capture_rect{};
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    std::uint64_t qpc_frequency = 0;
    std::uint64_t epoch = 1;
    std::uint64_t epoch_nonce = 1;
    WgcCursorShape current_cursor_shape{};
    HCURSOR last_win32_cursor = nullptr;

    // GDI resources are created lazily on the worker thread and released in
    // stop(); they are not ComPtr-managed.
    HDC screen_dc = nullptr;
    HDC memory_dc = nullptr;
    HBITMAP dib = nullptr;
    HBITMAP previous_bmp = nullptr;
    void* dib_bits = nullptr;

    std::mutex lifecycle_mutex;
    std::mutex error_mutex;
    std::jthread worker;
    WgcResult last_failure{};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> received_frames{0};
    std::atomic<std::uint64_t> published_frames{0};
    std::atomic<std::uint64_t> skipped_no_buffer{0};
    std::atomic<std::uint64_t> ingress_copy_submissions{0};
    std::atomic<std::uint64_t> full_damage_frames{0};
    std::atomic<std::uint64_t> cursor_metadata_frames{0};
    std::atomic<std::uint64_t> cursor_shape_updates{0};
};

} // namespace fluxcap::gpu

#endif
