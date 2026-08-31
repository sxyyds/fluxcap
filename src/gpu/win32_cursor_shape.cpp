#include "win32_cursor_shape.hpp"
#include "desktop_duplication_capture.hpp"

namespace fluxcap::gpu::internal {

bool extract_win32_cursor_shape(HCURSOR source, WgcCursorShape& output) {
    output = {};
    if (source == nullptr) return false;
    HICON cursor = CopyIcon(source);
    if (cursor == nullptr) return false;
    ICONINFO icon{};
    if (!GetIconInfo(cursor, &icon)) {
        DestroyIcon(cursor);
        return false;
    }
    bool success = false;
    HDC screen = GetDC(nullptr);
    const auto cleanup = [&]() noexcept {
        if (screen != nullptr) ReleaseDC(nullptr, screen);
        if (icon.hbmColor != nullptr) DeleteObject(icon.hbmColor);
        if (icon.hbmMask != nullptr) DeleteObject(icon.hbmMask);
        DestroyIcon(cursor);
    };
    try {
        do {
            if (screen == nullptr || icon.hbmMask == nullptr) break;
            BITMAP mask{};
            if (GetObjectW(icon.hbmMask, sizeof(mask), &mask) != sizeof(mask)
                || mask.bmWidth <= 0 || mask.bmHeight <= 0) {
                break;
            }
            output.hotspot_x = icon.xHotspot;
            output.hotspot_y = icon.yHotspot;
            if (icon.hbmColor != nullptr) {
                BITMAP color{};
                if (GetObjectW(icon.hbmColor, sizeof(color), &color)
                        != sizeof(color)
                    || color.bmWidth <= 0 || color.bmHeight <= 0
                    || color.bmWidth > 256 || color.bmHeight > 256
                    || icon.xHotspot
                        >= static_cast<DWORD>(color.bmWidth)
                    || icon.yHotspot
                        >= static_cast<DWORD>(color.bmHeight)) {
                    break;
                }
                output.kind = WgcCursorShapeKind::color_bgra8;
                output.width = static_cast<std::uint32_t>(color.bmWidth);
                output.height = static_cast<std::uint32_t>(color.bmHeight);
                output.stride_bytes = output.width * 4u;
                output.data.resize(
                    static_cast<std::size_t>(output.stride_bytes)
                        * output.height);
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth = color.bmWidth;
                bitmap.bmiHeader.biHeight = -color.bmHeight;
                bitmap.bmiHeader.biPlanes = 1;
                bitmap.bmiHeader.biBitCount = 32;
                bitmap.bmiHeader.biCompression = BI_RGB;
                if (GetDIBits(
                        screen,
                        icon.hbmColor,
                        0,
                        output.height,
                        output.data.data(),
                        &bitmap,
                        DIB_RGB_COLORS) != static_cast<int>(output.height)) {
                    break;
                }
            } else {
                if ((mask.bmHeight & 1) != 0
                    || mask.bmWidth > 256 || mask.bmHeight > 512) {
                    break;
                }
                output.kind = WgcCursorShapeKind::monochrome_and_xor;
                output.width = static_cast<std::uint32_t>(mask.bmWidth);
                output.height = static_cast<std::uint32_t>(mask.bmHeight / 2);
                if (icon.xHotspot >= output.width
                    || icon.yHotspot >= output.height) {
                    break;
                }
                output.stride_bytes = ((output.width + 31u) / 32u) * 4u;
                const std::uint32_t rows = output.height * 2u;
                output.data.resize(
                    static_cast<std::size_t>(output.stride_bytes) * rows);
                struct MonoBitmapInfo final {
                    BITMAPINFOHEADER header{};
                    RGBQUAD colors[2]{};
                } bitmap;
                bitmap.header.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.header.biWidth = mask.bmWidth;
                bitmap.header.biHeight = -mask.bmHeight;
                bitmap.header.biPlanes = 1;
                bitmap.header.biBitCount = 1;
                bitmap.header.biCompression = BI_RGB;
                bitmap.colors[1] = {255, 255, 255, 0};
                if (GetDIBits(
                        screen,
                        icon.hbmMask,
                        0,
                        rows,
                        output.data.data(),
                        reinterpret_cast<BITMAPINFO*>(&bitmap),
                        DIB_RGB_COLORS) != static_cast<int>(rows)) {
                    break;
                }
            }
            success = !output.data.empty()
                && output.data.size()
                    <= shared_frame_bus_max_cursor_shape_bytes;
            if (success) {
                output.sequence =
                    desktop_duplication_cursor_shape_key(output);
            }
        } while (false);
    } catch (...) {
        cleanup();
        output = {};
        throw;
    }
    cleanup();
    if (!success) output = {};
    return success;
}

} // namespace fluxcap::gpu::internal
