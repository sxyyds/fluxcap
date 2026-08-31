// Resilient monitor capture showcase: Desktop Duplication with worker-side
// ACCESS_LOST recovery, idle heartbeat, session-event awareness, and a GDI
// fallback chain, all publishing into one SharedFrameBus consumed in-process.
#include <fluxcap/gpu.hpp>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

namespace gpu = fluxcap::gpu;
using Microsoft::WRL::ComPtr;

std::uint32_t monitor_dimensions(HMONITOR monitor, std::uint32_t& height) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return 0;
    const LONG width = info.rcMonitor.right - info.rcMonitor.left;
    height = static_cast<std::uint32_t>(
        info.rcMonitor.bottom - info.rcMonitor.top);
    return static_cast<std::uint32_t>(width);
}

void print_stats(const char* label, const gpu::WgcCaptureStats& stats) {
    std::cout << '[' << label << "] published=" << stats.published_frames
              << " received=" << stats.received_frames
              << " rebuilds=" << stats.session_rebuilds
              << " recovery=" << stats.recovery_attempts << '/'
              << stats.recovery_successes
              << " idle_republish=" << stats.idle_republished_frames
              << " session_events=" << stats.session_events
              << " protected=" << stats.protected_content_frames
              << " no_slot_skips=" << stats.skipped_no_buffer << '\n';
}

} // namespace

int main() {
    const HMONITOR monitor = MonitorFromWindow(
        GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (monitor == nullptr) {
        std::cerr << "no primary monitor\n";
        return 1;
    }
    std::uint32_t height = 0;
    const std::uint32_t width = monitor_dimensions(monitor, height);
    if (width == 0 || height == 0) {
        std::cerr << "primary monitor has no dimensions\n";
        return 1;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    const HRESULT device_hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &feature_level,
        &context);
    if (FAILED(device_hr)) {
        std::cerr << "D3D11CreateDevice failed: 0x" << std::hex << device_hr
                  << std::dec << '\n';
        return 1;
    }

    gpu::SharedFrameBusConfig bus_config;
    bus_config.width = width;
    bus_config.height = height;
    bus_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bus_config.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bus_config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
    bus_config.slot_count = 3;
    gpu::SharedFrameBusPublisher publisher;
    gpu::GpuError created = gpu::SharedFrameBusPublisher::create(
        device.Get(), bus_config, publisher);
    if (!created) {
        std::cerr << "bus create failed: " << created.what() << '\n';
        return 1;
    }
    gpu::SharedFrameBusRegistration registration;
    const gpu::GpuError registered = publisher.register_consumer(
        GetCurrentProcess(), registration);
    if (!registered) {
        std::cerr << "consumer registration failed: " << registered.what()
                  << '\n';
        return 1;
    }
    gpu::SharedFrameBusConsumer consumer;
    const gpu::GpuError opened = gpu::SharedFrameBusConsumer::open(
        device.Get(), registration, true, consumer);
    if (!opened) {
        std::cerr << "consumer open failed: " << opened.what() << '\n';
        return 1;
    }

    gpu::WgcCaptureOptions options;
    options.pixel_format = gpu::WgcPixelFormat::bgra8;
    options.include_cursor = false;
    options.include_cursor_metadata = true;
    options.damage_mode = gpu::WgcDamageMode::native_report_only;
    options.capture_epoch = 7;
    options.capture_epoch_nonce = 0x5e5e'ca11ull;
    // Resilience knobs (Desktop Duplication / GDI only; ignored by WGC).
    options.frame_timeout_ms = 50;
    options.access_lost_retry_limit = 0; // retry forever with backoff
    options.access_lost_retry_initial_ms = 25;
    options.access_lost_retry_max_ms = 1'000;
    options.session_retry_limit = 3;
    options.session_retry_interval_ms = 100;
    options.idle_republish_interval_ms = 100;
    options.monitor_session_events = true;
    const gpu::WgcMailboxConfig mailbox; // full frame

    std::atomic<bool> draining{true};
    std::thread drain([&]() noexcept {
        while (draining.load(std::memory_order_relaxed)) {
            gpu::SharedFrameBusFrameLease frame;
            const gpu::GpuError acquired =
                consumer.acquire_latest(100, frame);
            if (acquired.status == gpu::GpuStatus::timeout) continue;
            if (!acquired) {
                std::cerr << "acquire failed: " << acquired.what() << '\n';
                break;
            }
            const gpu::GpuError released = consumer.release(frame);
            if (!released) {
                std::cerr << "release failed: " << released.what() << '\n';
                break;
            }
        }
    });

    gpu::DesktopDuplicationCapture duplication;
    gpu::WgcResult duplication_created =
        gpu::DesktopDuplicationCapture::create_for_monitor_to_bus(
            monitor, publisher, options, mailbox, duplication);
    if (duplication_created) {
        const gpu::WgcResult started = duplication.start();
        if (!started) duplication_created = started;
    }
    if (duplication_created) {
        const auto color = duplication.desktop_color_state();
        std::cout << "desktop color: "
                  << (color.known
                          ? (color.hdr ? "HDR (PQ/BT.2020)" : "SDR")
                          : "unknown (pre-DXGI1.6)")
                  << '\n';
        for (int second = 0; second < 8; ++second) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            print_stats("dd", duplication.stats());
            if (!duplication.running()) {
                std::cerr << "capture stopped: "
                          << duplication.last_error().message << '\n';
                break;
            }
        }
        duplication.stop();
    } else {
        std::cout << "Desktop Duplication unavailable ("
                  << duplication_created.message << "), falling back to GDI\n";
        duplication.stop();
        gpu::GdiMonitorCapture gdi;
        // GDI has no OS damage metadata and rejects WGC-only contracts.
        auto gdi_options = options;
        gdi_options.damage_mode = gpu::WgcDamageMode::disabled;
        gdi_options.include_cursor_metadata = false;
        const gpu::WgcResult gdi_created =
            gpu::GdiMonitorCapture::create_for_monitor_to_bus(
                monitor, publisher, gdi_options, mailbox, gdi);
        if (!gdi_created) {
            std::cerr << "GDI fallback failed: " << gdi_created.message
                      << '\n';
            draining.store(false);
            drain.join();
            return 1;
        }
        const gpu::WgcResult gdi_started = gdi.start();
        if (!gdi_started) {
            std::cerr << "GDI start failed: " << gdi_started.message << '\n';
            draining.store(false);
            drain.join();
            return 1;
        }
        for (int second = 0; second < 8; ++second) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            print_stats("gdi", gdi.stats());
        }
        gdi.stop();
    }

    draining.store(false);
    drain.join();
    (void)consumer.close();
    (void)publisher.unregister_consumer(registration, 5'000);
    std::cout << "done\n";
    return 0;
}
