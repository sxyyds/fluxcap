#ifndef FLUXCAP_GPU_WIN32_CURSOR_SHAPE_HPP
#define FLUXCAP_GPU_WIN32_CURSOR_SHAPE_HPP

#include <fluxcap/gpu.hpp>

namespace fluxcap::gpu::internal {

// Extracts an unscaled cursor shape from a Win32 HCURSOR via
// CopyIcon/GetIconInfo/GetDIBits. Shared by the Desktop Duplication, the
// multi-monitor controller, and the GDI fallback. Returns false without
// touching the output when the shape cannot be represented under the bus
// cursor contract.
[[nodiscard]] bool extract_win32_cursor_shape(
    HCURSOR source,
    WgcCursorShape& output);

} // namespace fluxcap::gpu::internal
#endif
