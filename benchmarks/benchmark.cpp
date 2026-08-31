#include <fluxcap/fluxcap.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kDefaultFrames = 300;
constexpr std::size_t kDefaultWarmup = 30;
constexpr std::size_t kMaximumSamples = 10'000'000;

struct Options {
    std::size_t frames = kDefaultFrames;
    std::size_t warmup = kDefaultWarmup;
    bool detect_dirty = true;
    bool show_help = false;
};

struct CaptureRegion {
    int x;
    int y;
    int width;
    int height;
};

struct Summary {
    double minimum_ms;
    double p50_ms;
    double p95_ms;
    double p99_ms;
    double mean_ms;
    double frames_per_second;
};

class ScopedDpiAwareness final {
public:
    ScopedDpiAwareness() noexcept
        : previous_(SetThreadDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}

    ~ScopedDpiAwareness() {
        if (previous_ != nullptr) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }

    ScopedDpiAwareness(const ScopedDpiAwareness&) = delete;
    ScopedDpiAwareness& operator=(const ScopedDpiAwareness&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_;
};

void print_usage() {
    std::cout << "Usage: fluxcap_bench [--frames N] [--warmup N] [--no-dirty]\n";
}

std::size_t parse_count(std::wstring_view text, std::wstring_view option) {
    if (text.empty()) {
        throw std::invalid_argument("missing numeric option value");
    }

    std::size_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            throw std::invalid_argument("option values must be non-negative integers");
        }
        const auto digit = static_cast<std::size_t>(character - L'0');
        if (value > (kMaximumSamples - digit) / 10u) {
            throw std::invalid_argument("option value is too large");
        }
        value = value * 10u + digit;
    }

    if (option == L"--frames" && value == 0) {
        throw std::invalid_argument("--frames must be greater than zero");
    }
    return value;
}

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.show_help = true;
        } else if (argument == L"--no-dirty") {
            options.detect_dirty = false;
        } else if (argument == L"--frames" || argument == L"--warmup") {
            if (++index >= argc) {
                throw std::invalid_argument("missing numeric option value");
            }
            const auto value = parse_count(argv[index], argument);
            if (argument == L"--frames") {
                options.frames = value;
            } else {
                options.warmup = value;
            }
        } else if (argument.starts_with(L"--frames=")) {
            options.frames = parse_count(argument.substr(9), L"--frames");
        } else if (argument.starts_with(L"--warmup=")) {
            options.warmup = parse_count(argument.substr(9), L"--warmup");
        } else {
            throw std::invalid_argument("unknown command-line option");
        }
    }
    return options;
}

void consume_flux_frame(const fluxcap::Frame& frame, std::uint64_t& checksum) {
    if (!frame || frame.data() == nullptr || frame.width() == 0 || frame.height() == 0) {
        throw std::runtime_error("FluxCap returned an empty frame");
    }

    std::uint32_t pixel = 0;
    std::memcpy(&pixel, frame.data(), sizeof(pixel));
    checksum += pixel;
}

CaptureRegion region_from_frame(const fluxcap::Frame& frame) {
    const auto desktop = frame.desktop_region();
    if (desktop.width <= 0 || desktop.height <= 0) {
        throw std::runtime_error("FluxCap returned an invalid desktop region");
    }
    return {desktop.x, desktop.y, desktop.width, desktop.height};
}

double capture_gdi_rebuilt(
    const CaptureRegion& region,
    std::uint64_t& checksum) {
    const auto begin = Clock::now();

    HDC screen_dc = GetDC(nullptr);
    HDC memory_dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous_bitmap = nullptr;
    void* pixels = nullptr;
    DWORD failure = ERROR_SUCCESS;
    bool copied = false;

    if (screen_dc == nullptr) {
        failure = GetLastError();
    } else {
        memory_dc = CreateCompatibleDC(screen_dc);
        if (memory_dc == nullptr) {
            failure = GetLastError();
        }
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = region.width;
    bitmap_info.bmiHeader.biHeight = -region.height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    if (memory_dc != nullptr) {
        bitmap = CreateDIBSection(
            screen_dc,
            &bitmap_info,
            DIB_RGB_COLORS,
            &pixels,
            nullptr,
            0);
        if (bitmap == nullptr || pixels == nullptr) {
            failure = GetLastError();
        }
    }

    if (bitmap != nullptr && pixels != nullptr) {
        previous_bitmap = SelectObject(memory_dc, bitmap);
        if (previous_bitmap == nullptr || previous_bitmap == HGDI_ERROR) {
            failure = GetLastError();
            previous_bitmap = nullptr;
        } else if (BitBlt(
                       memory_dc,
                       0,
                       0,
                       region.width,
                       region.height,
                       screen_dc,
                       region.x,
                       region.y,
                       SRCCOPY | CAPTUREBLT) != FALSE) {
            if (GdiFlush() != FALSE) {
                std::uint32_t pixel = 0;
                std::memcpy(&pixel, pixels, sizeof(pixel));
                checksum += pixel;
                copied = true;
            } else {
                failure = GetLastError();
            }
        } else {
            failure = GetLastError();
        }
    }

    if (previous_bitmap != nullptr) {
        SelectObject(memory_dc, previous_bitmap);
    }
    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    if (memory_dc != nullptr) {
        DeleteDC(memory_dc);
    }
    if (screen_dc != nullptr) {
        ReleaseDC(nullptr, screen_dc);
    }

    const auto end = Clock::now();
    if (!copied) {
        if (failure == ERROR_SUCCESS) {
            failure = ERROR_GEN_FAILURE;
        }
        throw std::runtime_error(
            "GDI baseline capture failed with Win32 error " +
            std::to_string(failure));
    }

    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::vector<double> measure_fluxcap(
    fluxcap::Session& session,
    std::size_t count,
    std::uint64_t& checksum) {
    std::vector<double> samples;
    samples.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const auto begin = Clock::now();
        auto frame = session.capture();
        consume_flux_frame(frame, checksum);
        frame.reset();
        const auto end = Clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(end - begin).count());
    }
    return samples;
}

std::vector<double> measure_gdi(
    const CaptureRegion& region,
    std::size_t count,
    std::uint64_t& checksum) {
    std::vector<double> samples;
    samples.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        samples.push_back(capture_gdi_rebuilt(region, checksum));
    }
    return samples;
}

double nearest_rank(const std::vector<double>& sorted, double percentile) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(sorted.size())));
    return sorted[rank == 0 ? 0 : rank - 1];
}

Summary summarize(std::vector<double> samples) {
    if (samples.empty()) {
        throw std::invalid_argument("cannot summarize an empty sample set");
    }

    const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    const double mean = total / static_cast<double>(samples.size());
    std::sort(samples.begin(), samples.end());
    return {
        samples.front(),
        nearest_rank(samples, 0.50),
        nearest_rank(samples, 0.95),
        nearest_rank(samples, 0.99),
        mean,
        1000.0 / mean};
}

void print_summary(std::string_view name, const Summary& summary) {
    std::cout << std::left << std::setw(24) << name
              << std::right << std::setw(11) << summary.minimum_ms
              << std::setw(11) << summary.p50_ms
              << std::setw(11) << summary.p95_ms
              << std::setw(11) << summary.p99_ms
              << std::setw(11) << summary.mean_ms
              << std::setw(13) << summary.frames_per_second << '\n';
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        ScopedDpiAwareness dpi;
        const Options options = parse_options(argc, argv);
        if (options.show_help) {
            print_usage();
            return 0;
        }

        auto config = fluxcap_config_default();
        if (options.detect_dirty) {
            config.flags |= static_cast<std::uint32_t>(FLUXCAP_FLAG_DETECT_DIRTY_REGIONS);
        } else {
            config.flags &= ~static_cast<std::uint32_t>(FLUXCAP_FLAG_DETECT_DIRTY_REGIONS);
        }

        fluxcap::Session session(config);
        auto probe = session.capture();
        std::uint64_t checksum = 0;
        consume_flux_frame(probe, checksum);
        const CaptureRegion region = region_from_frame(probe);
        probe.reset();

        for (std::size_t index = 0; index < options.warmup; ++index) {
            const auto frame = session.capture();
            consume_flux_frame(frame, checksum);
        }
        for (std::size_t index = 0; index < options.warmup; ++index) {
            static_cast<void>(capture_gdi_rebuilt(region, checksum));
        }

        const auto fluxcap_samples =
            measure_fluxcap(session, options.frames, checksum);
        const auto gdi_samples = measure_gdi(region, options.frames, checksum);

        std::cout << "FluxCap synchronous capture benchmark\n"
                  << "Frames: " << options.frames
                  << ", warmup: " << options.warmup
                  << ", dirty detection: "
                  << (options.detect_dirty ? "on" : "off") << '\n'
                  << "Region: " << region.x << ',' << region.y << ' '
                  << region.width << 'x' << region.height << "\n\n";

        std::cout << std::fixed << std::setprecision(3)
                  << std::left << std::setw(24) << "Method"
                  << std::right << std::setw(11) << "min ms"
                  << std::setw(11) << "p50 ms"
                  << std::setw(11) << "p95 ms"
                  << std::setw(11) << "p99 ms"
                  << std::setw(11) << "mean ms"
                  << std::setw(13) << "FPS" << '\n';
        print_summary("FluxCap steady-state", summarize(fluxcap_samples));
        print_summary("GDI rebuild/frame", summarize(gdi_samples));
        std::cout << "\nChecksum: " << checksum << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        print_usage();
        return 1;
    }
}
