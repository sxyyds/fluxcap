#ifndef FLUXCAP_GPU_DESKTOP_DUPLICATION_POLICY_HPP
#define FLUXCAP_GPU_DESKTOP_DUPLICATION_POLICY_HPP

#include <fluxcap/gpu.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace fluxcap::gpu::internal {

// Bounded exponential backoff for duplication session rebuilds. Attempt is
// 1-based; the delay never exceeds max_ms.
inline constexpr std::uint32_t desktop_duplication_backoff_delay_ms(
    std::uint32_t attempt,
    std::uint32_t initial_ms,
    std::uint32_t max_ms) noexcept {
    if (initial_ms == 0) return 0;
    if (attempt == 0) attempt = 1;
    if (attempt > 32) return max_ms;
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(initial_ms) << (attempt - 1);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(scaled, max_ms));
}

// Resolves the pixel format for a new duplication session. auto_detect uses
// the live desktop color space: HDR (PQ/BT.2020) selects scRGB FP16, anything
// else selects SDR BGRA8. Without auto_detect the declared format wins.
inline constexpr WgcPixelFormat desktop_duplication_resolve_pixel_format(
    bool auto_detect,
    bool desktop_hdr,
    WgcPixelFormat requested) noexcept {
    if (!auto_detect) return requested;
    return desktop_hdr ? WgcPixelFormat::rgba16_float : WgcPixelFormat::bgra8;
}

inline constexpr bool desktop_duplication_hdr_color_space(
    DXGI_COLOR_SPACE_TYPE color_space) noexcept {
    return color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

// Fills the DuplicateOutput1 negotiation array. The primary format always
// comes first; a fallback is appended only when the primary is FP16 and the
// caller opted into format negotiation. Returns the element count.
inline constexpr std::uint32_t desktop_duplication_format_negotiation(
    DXGI_FORMAT primary,
    bool allow_fallback,
    DXGI_FORMAT (&formats)[2]) noexcept {
    formats[0] = primary;
    if (!allow_fallback || primary == DXGI_FORMAT_B8G8R8A8_UNORM) return 1;
    formats[1] = DXGI_FORMAT_B8G8R8A8_UNORM;
    return 2;
}

inline constexpr bool desktop_duplication_format_fallback_accepted(
    DXGI_FORMAT actual,
    DXGI_FORMAT primary,
    bool allow_fallback) noexcept {
    return actual == DXGI_FORMAT_B8G8R8A8_UNORM
        && primary == DXGI_FORMAT_R16G16B16A16_FLOAT && allow_fallback;
}

// Heartbeat scheduling: true when the desktop has been silent for at least
// idle_interval_ms since the last committed publish.
inline constexpr bool desktop_duplication_heartbeat_due(
    std::uint64_t last_publish_qpc,
    std::uint64_t now_qpc,
    std::uint64_t qpc_frequency,
    std::uint32_t idle_interval_ms) noexcept {
    if (idle_interval_ms == 0 || last_publish_qpc == 0
        || qpc_frequency == 0 || now_qpc <= last_publish_qpc) {
        return false;
    }
    const std::uint64_t elapsed = now_qpc - last_publish_qpc;
    const std::uint64_t required =
        (static_cast<std::uint64_t>(idle_interval_ms) * qpc_frequency) / 1'000u;
    return elapsed >= required;
}

// Translates a monitor-local rectangle into virtual desktop coordinates.
inline constexpr bool desktop_duplication_offset_rect(
    const WgcRect& local,
    std::int32_t offset_x,
    std::int32_t offset_y,
    WgcRect& virtual_rect) noexcept {
    const std::int64_t x = static_cast<std::int64_t>(local.x) + offset_x;
    const std::int64_t y = static_cast<std::int64_t>(local.y) + offset_y;
    if (x < std::numeric_limits<std::int32_t>::min()
        || x > std::numeric_limits<std::int32_t>::max()
        || y < std::numeric_limits<std::int32_t>::min()
        || y > std::numeric_limits<std::int32_t>::max()) {
        virtual_rect = {};
        return false;
    }
    virtual_rect = {
        static_cast<std::int32_t>(x),
        static_cast<std::int32_t>(y),
        local.width,
        local.height};
    return true;
}

inline constexpr bool desktop_duplication_policy_contract() noexcept {
    if (desktop_duplication_backoff_delay_ms(1, 25, 1'000) != 25
        || desktop_duplication_backoff_delay_ms(2, 25, 1'000) != 50
        || desktop_duplication_backoff_delay_ms(7, 25, 1'000) != 1'000
        || desktop_duplication_backoff_delay_ms(40, 25, 1'000) != 1'000
        || desktop_duplication_backoff_delay_ms(3, 25, 0) != 0
        || desktop_duplication_backoff_delay_ms(3, 0, 1'000) != 0) {
        return false;
    }
    if (!desktop_duplication_hdr_color_space(
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        || desktop_duplication_hdr_color_space(
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)) {
        return false;
    }
    if (desktop_duplication_resolve_pixel_format(
            false, true, WgcPixelFormat::bgra8)
            != WgcPixelFormat::bgra8
        || desktop_duplication_resolve_pixel_format(
            true, true, WgcPixelFormat::bgra8)
            != WgcPixelFormat::rgba16_float
        || desktop_duplication_resolve_pixel_format(
            true, false, WgcPixelFormat::rgba16_float)
            != WgcPixelFormat::bgra8) {
        return false;
    }
    DXGI_FORMAT formats[2]{};
    if (desktop_duplication_format_negotiation(
            DXGI_FORMAT_R16G16B16A16_FLOAT, true, formats) != 2
        || formats[0] != DXGI_FORMAT_R16G16B16A16_FLOAT
        || formats[1] != DXGI_FORMAT_B8G8R8A8_UNORM
        || desktop_duplication_format_negotiation(
            DXGI_FORMAT_R16G16B16A16_FLOAT, false, formats) != 1
        || desktop_duplication_format_negotiation(
            DXGI_FORMAT_B8G8R8A8_UNORM, true, formats) != 1) {
        return false;
    }
    if (!desktop_duplication_format_fallback_accepted(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            true)
        || desktop_duplication_format_fallback_accepted(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            false)
        || desktop_duplication_format_fallback_accepted(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            true)) {
        return false;
    }
    if (desktop_duplication_heartbeat_due(0, 100, 1'000, 16)
        || desktop_duplication_heartbeat_due(100, 100, 1'000, 16)
        || !desktop_duplication_heartbeat_due(100, 116, 1'000, 16)
        || !desktop_duplication_heartbeat_due(100, 200, 1'000, 16)
        || desktop_duplication_heartbeat_due(100, 200, 1'000, 0)) {
        return false;
    }
    WgcRect offset{};
    if (!desktop_duplication_offset_rect({10, 20, 30, 40}, -1920, 0, offset)
        || offset.x != -1910 || offset.y != 20
        || offset.width != 30 || offset.height != 40
        || desktop_duplication_offset_rect(
            {1, 0, 1, 1},
            std::numeric_limits<std::int32_t>::max(),
            0,
            offset)) {
        return false;
    }
    return true;
}

static_assert(desktop_duplication_policy_contract());

} // namespace fluxcap::gpu::internal
#endif
