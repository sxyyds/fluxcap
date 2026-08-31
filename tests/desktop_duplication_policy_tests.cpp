#include "gpu/desktop_duplication_policy.hpp"

#include <cstdio>

using namespace fluxcap::gpu;

namespace {

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::printf("FAIL %s\n", name);
        failures += 1;
    }
}

void test_backoff() {
    namespace policy = internal;
    check(
        policy::desktop_duplication_backoff_delay_ms(1, 25, 1'000) == 25,
        "backoff first attempt is the initial delay");
    check(
        policy::desktop_duplication_backoff_delay_ms(2, 25, 1'000) == 50,
        "backoff doubles");
    check(
        policy::desktop_duplication_backoff_delay_ms(6, 25, 1'000) == 800,
        "backoff keeps doubling");
    check(
        policy::desktop_duplication_backoff_delay_ms(7, 25, 1'000) == 1'000,
        "backoff clamps to max");
    check(
        policy::desktop_duplication_backoff_delay_ms(50, 25, 1'000) == 1'000,
        "backoff stays clamped for large attempts");
    check(
        policy::desktop_duplication_backoff_delay_ms(3, 25, 0) == 0,
        "backoff with zero max is zero");
    check(
        policy::desktop_duplication_backoff_delay_ms(0, 25, 1'000) == 25,
        "backoff treats zero attempt as one");
}

void test_color_resolution() {
    namespace policy = internal;
    check(
        policy::desktop_duplication_resolve_pixel_format(
            false, true, WgcPixelFormat::bgra8)
            == WgcPixelFormat::bgra8,
        "declared format wins without auto detect");
    check(
        policy::desktop_duplication_resolve_pixel_format(
            true, true, WgcPixelFormat::bgra8)
            == WgcPixelFormat::rgba16_float,
        "HDR desktop upgrades to FP16");
    check(
        policy::desktop_duplication_resolve_pixel_format(
            true, false, WgcPixelFormat::rgba16_float)
            == WgcPixelFormat::bgra8,
        "SDR desktop downgrades to BGRA8");
    check(
        policy::desktop_duplication_hdr_color_space(
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020),
        "PQ/BT.2020 is HDR");
    check(
        !policy::desktop_duplication_hdr_color_space(
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),
        "sRGB is not HDR");
    check(
        !policy::desktop_duplication_hdr_color_space(
            DXGI_COLOR_SPACE_CUSTOM),
        "CUSTOM is not HDR");
}

void test_format_negotiation() {
    namespace policy = internal;
    DXGI_FORMAT formats[2]{};
    check(
        policy::desktop_duplication_format_negotiation(
            DXGI_FORMAT_R16G16B16A16_FLOAT, true, formats)
            == 2,
        "FP16 negotiates with a BGRA8 fallback");
    check(
        formats[0] == DXGI_FORMAT_R16G16B16A16_FLOAT
            && formats[1] == DXGI_FORMAT_B8G8R8A8_UNORM,
        "primary format comes first");
    check(
        policy::desktop_duplication_format_negotiation(
            DXGI_FORMAT_R16G16B16A16_FLOAT, false, formats)
            == 1,
        "fallback requires opt-in");
    check(
        policy::desktop_duplication_format_negotiation(
            DXGI_FORMAT_B8G8R8A8_UNORM, true, formats)
            == 1,
        "BGRA8 needs no fallback");
    check(
        policy::desktop_duplication_format_fallback_accepted(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            true),
        "BGRA8 accepted for FP16 request");
    check(
        !policy::desktop_duplication_format_fallback_accepted(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            false),
        "fallback gated by option");
    check(
        !policy::desktop_duplication_format_fallback_accepted(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            true),
        "format match needs no fallback");
    check(
        !policy::desktop_duplication_format_fallback_accepted(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            true),
        "BGRA8 primary never falls back");
}

void test_heartbeat() {
    namespace policy = internal;
    check(
        !policy::desktop_duplication_heartbeat_due(
            0, 100, 1'000, 16),
        "no heartbeat before the first publish");
    check(
        !policy::desktop_duplication_heartbeat_due(
            100, 100, 1'000, 16),
        "no heartbeat while fresh");
    check(
        !policy::desktop_duplication_heartbeat_due(
            100, 115, 1'000, 16),
        "heartbeat waits for the full interval");
    check(
        policy::desktop_duplication_heartbeat_due(
            100, 116, 1'000, 16),
        "heartbeat fires at the interval");
    check(
        policy::desktop_duplication_heartbeat_due(
            100, 1'000, 1'000, 16),
        "heartbeat fires long after the interval");
    check(
        !policy::desktop_duplication_heartbeat_due(
            100, 1'000, 1'000, 0),
        "disabled heartbeat never fires");
    check(
        !policy::desktop_duplication_heartbeat_due(
            100, 100, 0, 16),
        "zero frequency never fires");
}

void test_offset_rect() {
    namespace policy = internal;
    WgcRect rect{};
    check(
        policy::desktop_duplication_offset_rect(
            {10, 20, 30, 40}, -1920, 0, rect)
            && rect.x == -1910 && rect.y == 20
            && rect.width == 30 && rect.height == 40,
        "offset applies to the origin only");
    check(
        !policy::desktop_duplication_offset_rect(
            {1, 0, 1, 1},
            std::numeric_limits<std::int32_t>::max(),
            0,
            rect),
        "offset overflow fails closed");
}

} // namespace

int main() {
    check(
        internal::desktop_duplication_policy_contract(),
        "compile-time policy contract");
    test_backoff();
    test_color_resolution();
    test_format_negotiation();
    test_heartbeat();
    test_offset_rect();
    if (failures == 0) {
        std::printf("desktop_duplication_policy_tests: all passed\n");
        return 0;
    }
    std::printf("desktop_duplication_policy_tests: %d failure(s)\n", failures);
    return 1;
}
