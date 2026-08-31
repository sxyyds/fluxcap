#include <fluxcap/fluxcap.h>

#include "dirty_tracker.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fluxcap::internal {
namespace {

thread_local std::array<char, 512> g_last_error{};

void clear_last_error() noexcept {
    g_last_error[0] = '\0';
}

void set_last_error(std::string_view message) noexcept {
    const size_t count = std::min(message.size(), g_last_error.size() - 1u);
    if (count != 0) {
        std::memcpy(g_last_error.data(), message.data(), count);
    }
    g_last_error[count] = '\0';
}

void set_last_error_parts(std::string_view prefix, std::string_view detail) noexcept {
    clear_last_error();
    const size_t prefix_count = std::min(prefix.size(), g_last_error.size() - 1u);
    if (prefix_count != 0) {
        std::memcpy(g_last_error.data(), prefix.data(), prefix_count);
    }
    const size_t remaining = g_last_error.size() - 1u - prefix_count;
    const size_t detail_count = std::min(detail.size(), remaining);
    if (detail_count != 0) {
        std::memcpy(g_last_error.data() + prefix_count, detail.data(), detail_count);
    }
    g_last_error[prefix_count + detail_count] = '\0';
}

std::string win32_failure(const char* operation, DWORD error = GetLastError()) {
    std::string message(operation);
    message.append(" failed (Win32 error ").append(std::to_string(error)).append(")");
    return message;
}

class CaptureError final : public std::runtime_error {
public:
    CaptureError(fluxcap_status status, std::string message)
        : std::runtime_error(std::move(message)), status_(status) {}

    [[nodiscard]] fluxcap_status status() const noexcept { return status_; }

private:
    fluxcap_status status_;
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

class ScreenDc final {
public:
    ScreenDc() noexcept : value_(GetDC(nullptr)) {}
    ~ScreenDc() {
        if (value_ != nullptr) {
            ReleaseDC(nullptr, value_);
        }
    }

    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

    bool refresh() noexcept {
        HDC replacement = GetDC(nullptr);
        if (replacement == nullptr) {
            return false;
        }
        if (value_ != nullptr) {
            ReleaseDC(nullptr, value_);
        }
        value_ = replacement;
        return true;
    }

private:
    HDC value_ = nullptr;
};

enum class SlotState : uint8_t {
    free,
    writing,
    ready,
    reading
};

constexpr uint64_t kSlotStateBits = 2;
constexpr uint64_t kSlotStateMask = (1ull << kSlotStateBits) - 1ull;

constexpr uint64_t slot_control(uint64_t generation, SlotState state) noexcept {
    return (generation << kSlotStateBits) | static_cast<uint64_t>(state);
}

constexpr SlotState slot_state(uint64_t control) noexcept {
    return static_cast<SlotState>(control & kSlotStateMask);
}

constexpr uint64_t slot_generation(uint64_t control) noexcept {
    return control >> kSlotStateBits;
}

struct FrameSlot final {
    FrameSlot() = default;
    ~FrameSlot() { destroy(); }

    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;

    void initialize(
        HDC source,
        uint32_t width,
        uint32_t height,
        size_t dirty_capacity) {
        dc = CreateCompatibleDC(source);
        if (dc == nullptr) {
            throw CaptureError(
                FLUXCAP_STATUS_SYSTEM_ERROR,
                win32_failure("CreateCompatibleDC"));
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* dib_pixels = nullptr;
        bitmap = CreateDIBSection(
            source,
            &info,
            DIB_RGB_COLORS,
            &dib_pixels,
            nullptr,
            0);
        if (bitmap == nullptr || dib_pixels == nullptr) {
            throw CaptureError(
                FLUXCAP_STATUS_SYSTEM_ERROR,
                win32_failure("CreateDIBSection"));
        }

        old_bitmap = SelectObject(dc, bitmap);
        if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
            throw CaptureError(
                FLUXCAP_STATUS_SYSTEM_ERROR,
                win32_failure("SelectObject"));
        }

        pixels = static_cast<uint8_t*>(dib_pixels);
        stride = width * 4u;
        dirty_regions.reserve(dirty_capacity);
    }

    void destroy() noexcept {
        if (dc != nullptr && old_bitmap != nullptr && old_bitmap != HGDI_ERROR) {
            SelectObject(dc, old_bitmap);
        }
        old_bitmap = nullptr;
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
        if (dc != nullptr) {
            DeleteDC(dc);
            dc = nullptr;
        }
        pixels = nullptr;
    }

    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    uint8_t* pixels = nullptr;
    uint32_t stride = 0;
    std::atomic<uint64_t> control{slot_control(0, SlotState::free)};
    std::vector<fluxcap_rect> dirty_regions;
    uint64_t sequence = 0;
    uint64_t dirty_base_sequence = 0;
    uint64_t timestamp_qpc = 0;
    uint64_t capture_duration_ns = 0;
};

struct AtomicStats final {
    std::atomic<uint64_t> captured_frames{0};
    std::atomic<uint64_t> overwritten_frames{0};
    std::atomic<uint64_t> no_buffer_skips{0};
    std::atomic<uint64_t> capture_failures{0};
    std::atomic<uint64_t> total_capture_ns{0};
    std::atomic<uint64_t> last_capture_ns{0};
};

class AtomicCounterGuard final {
public:
    explicit AtomicCounterGuard(std::atomic<uint32_t>& counter) noexcept
        : counter_(counter) {
        counter_.fetch_add(1, std::memory_order_acq_rel);
    }

    ~AtomicCounterGuard() {
        counter_.fetch_sub(1, std::memory_order_acq_rel);
    }

    AtomicCounterGuard(const AtomicCounterGuard&) = delete;
    AtomicCounterGuard& operator=(const AtomicCounterGuard&) = delete;

private:
    std::atomic<uint32_t>& counter_;
};

fluxcap_rect virtual_desktop_rect() {
    ScopedDpiAwareness dpi;
    return {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

bool rect_fits_in(const fluxcap_rect& inner, const fluxcap_rect& outer) noexcept {
    const int64_t inner_right = static_cast<int64_t>(inner.x) + inner.width;
    const int64_t inner_bottom = static_cast<int64_t>(inner.y) + inner.height;
    const int64_t outer_right = static_cast<int64_t>(outer.x) + outer.width;
    const int64_t outer_bottom = static_cast<int64_t>(outer.y) + outer.height;
    return inner.x >= outer.x && inner.y >= outer.y
        && inner_right <= outer_right && inner_bottom <= outer_bottom;
}

fluxcap_config validate_config(const fluxcap_config& source) {
    if (source.struct_size != sizeof(fluxcap_config)) {
        throw CaptureError(
            FLUXCAP_STATUS_INVALID_ARGUMENT,
            "config.struct_size does not match the FluxCap 1 ABI");
    }
    if (source.abi_version != FLUXCAP_ABI_VERSION) {
        throw CaptureError(
            FLUXCAP_STATUS_INVALID_ARGUMENT,
            "config.abi_version does not match FLUXCAP_ABI_VERSION");
    }
    if (source.buffer_count < 2 || source.buffer_count > 16) {
        throw CaptureError(
            FLUXCAP_STATUS_INVALID_ARGUMENT,
            "buffer_count must be between 2 and 16");
    }
    if (source.target_fps > 1000) {
        throw CaptureError(
            FLUXCAP_STATUS_INVALID_ARGUMENT,
            "target_fps must not exceed 1000");
    }

    constexpr uint32_t known_flags = FLUXCAP_FLAG_INCLUDE_LAYERED_WINDOWS
        | FLUXCAP_FLAG_INCLUDE_CURSOR
        | FLUXCAP_FLAG_DETECT_DIRTY_REGIONS
        | FLUXCAP_FLAG_HIGH_PRIORITY_THREAD;
    if ((source.flags & ~known_flags) != 0) {
        throw CaptureError(
            FLUXCAP_STATUS_INVALID_ARGUMENT,
            "config.flags contains an unknown flag");
    }

    if ((source.flags & FLUXCAP_FLAG_DETECT_DIRTY_REGIONS) != 0) {
        if (source.tile_size < 8 || source.tile_size > 512
            || !std::has_single_bit(source.tile_size)) {
            throw CaptureError(
                FLUXCAP_STATUS_INVALID_ARGUMENT,
                "tile_size must be a power of two between 8 and 512");
        }
    }

    fluxcap_config result = source;
    const bool default_region = source.region.width == 0 && source.region.height == 0;
    if (default_region) {
        result.region = virtual_desktop_rect();
    } else if (source.region.width <= 0 || source.region.height <= 0) {
        throw CaptureError(
            FLUXCAP_STATUS_INVALID_ARGUMENT,
            "region width and height must both be positive or both be zero");
    }

    const fluxcap_rect desktop = virtual_desktop_rect();
    if (desktop.width <= 0 || desktop.height <= 0) {
        throw CaptureError(
            FLUXCAP_STATUS_SYSTEM_ERROR,
            "Windows reported an empty virtual desktop");
    }
    if (!rect_fits_in(result.region, desktop)) {
        throw CaptureError(
            FLUXCAP_STATUS_INVALID_ARGUMENT,
            "capture region is outside the current virtual desktop");
    }
    if (static_cast<uint64_t>(result.region.width) * 4u
        > std::numeric_limits<uint32_t>::max()) {
        throw CaptureError(
            FLUXCAP_STATUS_INVALID_ARGUMENT,
            "capture row is too wide for the BGRX8 ABI");
    }

    const uint64_t frame_bytes = static_cast<uint64_t>(result.region.width)
        * static_cast<uint64_t>(result.region.height) * 4u;
    if (frame_bytes > std::numeric_limits<size_t>::max()
        || frame_bytes > std::numeric_limits<size_t>::max() / result.buffer_count) {
        throw CaptureError(
            FLUXCAP_STATUS_OUT_OF_MEMORY,
            "requested frame pool exceeds the process address space");
    }
    return result;
}

uint64_t qpc_delta_to_ns(uint64_t delta, uint64_t frequency) noexcept {
    const uint64_t whole_seconds = delta / frequency;
    const uint64_t remainder = delta % frequency;
    return whole_seconds * 1'000'000'000ull
        + remainder * 1'000'000'000ull / frequency;
}

} // namespace

class Session final {
public:
    explicit Session(const fluxcap_config& requested)
        : config_(validate_config(requested)), region_(config_.region) {
        ScopedDpiAwareness dpi;
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            throw CaptureError(
                FLUXCAP_STATUS_SYSTEM_ERROR,
                win32_failure("QueryPerformanceFrequency"));
        }
        qpc_frequency_ = static_cast<uint64_t>(frequency.QuadPart);

        ScreenDc source_dc;
        if (source_dc.get() == nullptr) {
            throw CaptureError(
                FLUXCAP_STATUS_SYSTEM_ERROR,
                win32_failure("GetDC"));
        }

        try {
            size_t dirty_capacity = 1;
            if (dirty_detection_enabled()) {
                dirty_tracker_ = std::make_unique<DirtyTracker>(
                    static_cast<uint32_t>(region_.width),
                    static_cast<uint32_t>(region_.height),
                    config_.tile_size);
                dirty_capacity = dirty_tracker_->max_dirty_rects();
            }

            slots_.reserve(config_.buffer_count);
            for (uint32_t index = 0; index < config_.buffer_count; ++index) {
                auto slot = std::make_unique<FrameSlot>();
                slot->initialize(
                    source_dc.get(),
                    static_cast<uint32_t>(region_.width),
                    static_cast<uint32_t>(region_.height),
                    dirty_capacity);
                slots_.push_back(std::move(slot));
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Session() {
        try {
            (void)stop();
        } catch (...) {
        }
        cleanup();
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    fluxcap_status capture(fluxcap_frame& output) {
        ScopedDpiAwareness dpi;
        std::scoped_lock producer_lock(producer_mutex_);
        if (running_.load(std::memory_order_acquire)) {
            set_last_error("synchronous capture is unavailable while the worker is running");
            return FLUXCAP_STATUS_INVALID_STATE;
        }

        ScreenDc source_dc;
        if (source_dc.get() == nullptr) {
            set_last_error(win32_failure("GetDC"));
            return FLUXCAP_STATUS_SYSTEM_ERROR;
        }

        size_t slot_index = 0;
        FrameSlot* slot = acquire_write_slot(slot_index);
        if (slot == nullptr) {
            stats_.no_buffer_skips.fetch_add(1, std::memory_order_relaxed);
            set_last_error("all frame slots are currently leased by callers");
            return FLUXCAP_STATUS_NO_BUFFER;
        }

        const fluxcap_status status = capture_into(*slot, source_dc);
        if (status != FLUXCAP_STATUS_OK) {
            release_write_slot(*slot);
            return status;
        }

        slot->control.store(
            slot_control(slot->sequence, SlotState::reading),
            std::memory_order_release);
        fill_frame(*slot, slot_index, output);
        return FLUXCAP_STATUS_OK;
    }

    fluxcap_status start() {
        std::lock_guard control_lock(control_mutex_);
        if (worker_.joinable() || running_.load(std::memory_order_acquire)) {
            set_last_error("capture worker is already running");
            return FLUXCAP_STATUS_INVALID_STATE;
        }

        for (auto& slot : slots_) {
            uint64_t control = slot->control.load(std::memory_order_acquire);
            while (slot_state(control) == SlotState::ready
                && !slot->control.compare_exchange_weak(
                    control,
                    slot_control(slot_generation(control), SlotState::free),
                    std::memory_order_acq_rel)) {
            }
        }

        clear_worker_diagnostics();

        {
            std::lock_guard event_lock(event_mutex_);
            stop_requested_.store(false, std::memory_order_release);
            running_.store(true, std::memory_order_release);
        }
        try {
            worker_ = std::thread([this] { worker_main(); });
        } catch (const std::system_error& error) {
            {
                std::lock_guard event_lock(event_mutex_);
                running_.store(false, std::memory_order_release);
            }
            event_cv_.notify_all();
            set_last_error_parts("failed to create capture thread: ", error.what());
            return FLUXCAP_STATUS_SYSTEM_ERROR;
        }
        return FLUXCAP_STATUS_OK;
    }

    fluxcap_status stop() {
        std::lock_guard control_lock(control_mutex_);
        {
            std::lock_guard event_lock(event_mutex_);
            stop_requested_.store(true, std::memory_order_release);
        }
        event_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        {
            std::lock_guard event_lock(event_mutex_);
            running_.store(false, std::memory_order_release);
        }
        event_cv_.notify_all();
        return FLUXCAP_STATUS_OK;
    }

    fluxcap_status acquire_latest(uint32_t timeout_ms, fluxcap_frame& output) {
        AtomicCounterGuard acquiring(active_acquires_);
        using Clock = std::chrono::steady_clock;
        const Clock::time_point deadline = timeout_ms == FLUXCAP_WAIT_INFINITE
            ? Clock::time_point::max()
            : Clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock event_lock(event_mutex_);

        const auto ready_or_stopped = [this] {
            return has_ready_frame()
                || !running_.load(std::memory_order_acquire);
        };

        for (;;) {
            while (!has_ready_frame()) {
                if (!running_.load(std::memory_order_acquire)) {
                    const fluxcap_status terminal = worker_terminal_status_.load(
                        std::memory_order_acquire);
                    if (terminal != FLUXCAP_STATUS_OK) {
                        copy_worker_error_to_caller();
                        return terminal;
                    }
                    set_last_error("capture worker is not running and has no published frame");
                    return FLUXCAP_STATUS_INVALID_STATE;
                }
                if (timeout_ms == 0) {
                    return timeout_with_worker_diagnostics();
                }
                if (timeout_ms == FLUXCAP_WAIT_INFINITE) {
                    event_cv_.wait(event_lock, ready_or_stopped);
                } else if (!event_cv_.wait_until(
                               event_lock,
                               deadline,
                               ready_or_stopped)) {
                    return timeout_with_worker_diagnostics();
                }
            }

            size_t newest_index = slots_.size();
            uint64_t newest_sequence = 0;
            uint64_t newest_control = 0;
            for (size_t index = 0; index < slots_.size(); ++index) {
                FrameSlot& candidate = *slots_[index];
                const uint64_t control = candidate.control.load(std::memory_order_acquire);
                if (slot_state(control) != SlotState::ready) {
                    continue;
                }
                const uint64_t candidate_sequence = slot_generation(control);
                if (candidate_sequence >= newest_sequence) {
                    newest_sequence = candidate_sequence;
                    newest_index = index;
                    newest_control = control;
                }
            }

            if (newest_index == slots_.size()) {
                if (timeout_ms != FLUXCAP_WAIT_INFINITE && Clock::now() >= deadline) {
                    return timeout_with_worker_diagnostics();
                }
                continue;
            }

            FrameSlot& newest = *slots_[newest_index];
            if (!newest.control.compare_exchange_strong(
                    newest_control,
                    slot_control(newest_sequence, SlotState::reading),
                    std::memory_order_acq_rel)) {
                if (timeout_ms != FLUXCAP_WAIT_INFINITE && Clock::now() >= deadline) {
                    return timeout_with_worker_diagnostics();
                }
                continue;
            }

            fill_frame(newest, newest_index, output);
            event_lock.unlock();

            for (auto& slot : slots_) {
                if (slot.get() == &newest) {
                    continue;
                }
                uint64_t control = slot->control.load(std::memory_order_acquire);
                while (slot_state(control) == SlotState::ready
                    && slot_generation(control) < newest_sequence
                    && !slot->control.compare_exchange_weak(
                        control,
                        slot_control(slot_generation(control), SlotState::free),
                        std::memory_order_acq_rel)) {
                }
            }
            return FLUXCAP_STATUS_OK;
        }
    }

    fluxcap_status release(uint32_t slot_index, uint64_t token) noexcept {
        if (slot_index >= slots_.size()) {
            set_last_error("frame lease has an invalid slot index");
            return FLUXCAP_STATUS_INVALID_ARGUMENT;
        }
        FrameSlot& slot = *slots_[slot_index];
        uint64_t expected = slot_control(token, SlotState::reading);
        if (!slot.control.compare_exchange_strong(
                expected,
                slot_control(token, SlotState::free),
                std::memory_order_acq_rel)) {
            set_last_error("frame has already been released or its lease generation is stale");
            return FLUXCAP_STATUS_INVALID_STATE;
        }
        return FLUXCAP_STATUS_OK;
    }

    void read_stats(fluxcap_stats& output) const noexcept {
        output.captured_frames = stats_.captured_frames.load(std::memory_order_relaxed);
        output.overwritten_frames = stats_.overwritten_frames.load(std::memory_order_relaxed);
        output.no_buffer_skips = stats_.no_buffer_skips.load(std::memory_order_relaxed);
        output.capture_failures = stats_.capture_failures.load(std::memory_order_relaxed);
        output.total_capture_ns = stats_.total_capture_ns.load(std::memory_order_relaxed);
        output.last_capture_ns = stats_.last_capture_ns.load(std::memory_order_relaxed);
    }

private:
    void clear_worker_diagnostics() noexcept {
        clear_worker_error_message();
        worker_terminal_status_.store(FLUXCAP_STATUS_OK, std::memory_order_release);
    }

    void clear_worker_error_message() noexcept {
        AcquireSRWLockExclusive(&worker_error_lock_);
        worker_error_[0] = '\0';
        ReleaseSRWLockExclusive(&worker_error_lock_);
    }

    void record_worker_error(
        fluxcap_status status,
        std::string_view message,
        bool terminal) noexcept {
        AcquireSRWLockExclusive(&worker_error_lock_);
        const size_t count = std::min(message.size(), worker_error_.size() - 1u);
        if (count != 0) {
            std::memcpy(worker_error_.data(), message.data(), count);
        }
        worker_error_[count] = '\0';
        ReleaseSRWLockExclusive(&worker_error_lock_);
        if (terminal) {
            worker_terminal_status_.store(status, std::memory_order_release);
        }
    }

    void copy_worker_error_to_caller() const noexcept {
        std::array<char, 512> copy{};
        AcquireSRWLockShared(&worker_error_lock_);
        std::memcpy(copy.data(), worker_error_.data(), copy.size());
        ReleaseSRWLockShared(&worker_error_lock_);
        if (copy[0] != '\0') {
            set_last_error(copy.data());
        }
    }

    fluxcap_status timeout_with_worker_diagnostics() const noexcept {
        copy_worker_error_to_caller();
        return FLUXCAP_STATUS_TIMEOUT;
    }

    [[nodiscard]] bool dirty_detection_enabled() const noexcept {
        return (config_.flags & FLUXCAP_FLAG_DETECT_DIRTY_REGIONS) != 0;
    }

    [[nodiscard]] bool has_ready_frame() const noexcept {
        return std::any_of(slots_.begin(), slots_.end(), [](const auto& slot) {
            return slot_state(slot->control.load(std::memory_order_acquire))
                == SlotState::ready;
        });
    }

    FrameSlot* acquire_write_slot(
        size_t& output_index,
        bool preserve_last_ready = false) noexcept {
        for (size_t index = 0; index < slots_.size(); ++index) {
            uint64_t control = slots_[index]->control.load(std::memory_order_acquire);
            while (slot_state(control) == SlotState::free) {
                if (slots_[index]->control.compare_exchange_weak(
                        control,
                        slot_control(slot_generation(control), SlotState::writing),
                        std::memory_order_acq_rel)) {
                    output_index = index;
                    return slots_[index].get();
                }
            }
        }

        for (size_t attempt = 0; attempt < slots_.size(); ++attempt) {
            size_t oldest_index = slots_.size();
            uint64_t oldest_sequence = std::numeric_limits<uint64_t>::max();
            uint64_t oldest_control = 0;
            size_t ready_count = 0;
            for (size_t index = 0; index < slots_.size(); ++index) {
                const FrameSlot& slot = *slots_[index];
                const uint64_t control = slot.control.load(std::memory_order_acquire);
                if (slot_state(control) != SlotState::ready) {
                    continue;
                }
                ++ready_count;
                if (slot_generation(control) < oldest_sequence) {
                    oldest_sequence = slot_generation(control);
                    oldest_index = index;
                    oldest_control = control;
                }
            }
            if (oldest_index == slots_.size()
                || (preserve_last_ready && ready_count <= 1)) {
                return nullptr;
            }

            if (slots_[oldest_index]->control.compare_exchange_strong(
                    oldest_control,
                    slot_control(oldest_sequence, SlotState::writing),
                    std::memory_order_acq_rel)) {
                stats_.overwritten_frames.fetch_add(1, std::memory_order_relaxed);
                output_index = oldest_index;
                return slots_[oldest_index].get();
            }
        }
        return nullptr;
    }

    static void release_write_slot(FrameSlot& slot) noexcept {
        const uint64_t control = slot.control.load(std::memory_order_relaxed);
        slot.control.store(
            slot_control(slot_generation(control), SlotState::free),
            std::memory_order_release);
    }

    bool capture_pixels(FrameSlot& slot, ScreenDc& source_dc) {
        const DWORD raster_operation = SRCCOPY
            | ((config_.flags & FLUXCAP_FLAG_INCLUDE_LAYERED_WINDOWS) != 0
                ? CAPTUREBLT
                : 0u);

        SetLastError(ERROR_SUCCESS);
        if (BitBlt(
                slot.dc,
                0,
                0,
                region_.width,
                region_.height,
                source_dc.get(),
                region_.x,
                region_.y,
                raster_operation)) {
            return true;
        }

        const DWORD first_error = GetLastError();
        if (source_dc.refresh()) {
            SetLastError(ERROR_SUCCESS);
            if (BitBlt(
                    slot.dc,
                    0,
                    0,
                    region_.width,
                    region_.height,
                    source_dc.get(),
                    region_.x,
                    region_.y,
                    raster_operation)) {
                return true;
            }
        }

        const DWORD retry_error = GetLastError();
        set_last_error(win32_failure(
            "BitBlt",
            retry_error != ERROR_SUCCESS ? retry_error : first_error));
        return false;
    }

    void draw_cursor(FrameSlot& slot) noexcept {
        if ((config_.flags & FLUXCAP_FLAG_INCLUDE_CURSOR) == 0) {
            return;
        }

        CURSORINFO cursor_info{};
        cursor_info.cbSize = sizeof(cursor_info);
        if (!GetCursorInfo(&cursor_info)
            || (cursor_info.flags & CURSOR_SHOWING) == 0
            || cursor_info.hCursor == nullptr) {
            return;
        }

        if (cursor_info.hCursor != cached_cursor_) {
            ICONINFO icon_info{};
            if (GetIconInfo(cursor_info.hCursor, &icon_info)) {
                cursor_hotspot_.x = static_cast<LONG>(icon_info.xHotspot);
                cursor_hotspot_.y = static_cast<LONG>(icon_info.yHotspot);
                if (icon_info.hbmMask != nullptr) {
                    DeleteObject(icon_info.hbmMask);
                }
                if (icon_info.hbmColor != nullptr) {
                    DeleteObject(icon_info.hbmColor);
                }
                cached_cursor_ = cursor_info.hCursor;
            }
        }

        const int x = cursor_info.ptScreenPos.x - region_.x - cursor_hotspot_.x;
        const int y = cursor_info.ptScreenPos.y - region_.y - cursor_hotspot_.y;
        (void)DrawIconEx(
            slot.dc,
            x,
            y,
            cursor_info.hCursor,
            0,
            0,
            0,
            nullptr,
            DI_NORMAL);
    }

    fluxcap_status capture_into(FrameSlot& slot, ScreenDc& source_dc) {
        LARGE_INTEGER before{};
        LARGE_INTEGER after{};
        QueryPerformanceCounter(&before);

        if (!capture_pixels(slot, source_dc)) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            return FLUXCAP_STATUS_SYSTEM_ERROR;
        }
        draw_cursor(slot);
        if (!GdiFlush()) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            set_last_error(win32_failure("GdiFlush"));
            return FLUXCAP_STATUS_SYSTEM_ERROR;
        }

        try {
            if (dirty_tracker_) {
                slot.dirty_base_sequence = dirty_tracker_->has_baseline()
                    ? sequence_
                    : 0;
                dirty_tracker_->analyze(slot.pixels, slot.stride, slot.dirty_regions);
            } else {
                slot.dirty_base_sequence = 0;
                slot.dirty_regions.clear();
                slot.dirty_regions.push_back({
                    0,
                    0,
                    region_.width,
                    region_.height});
            }
        } catch (const std::bad_alloc&) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            set_last_error("memory allocation failed during dirty-region analysis");
            return FLUXCAP_STATUS_OUT_OF_MEMORY;
        } catch (const std::exception& error) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            set_last_error_parts("dirty-region analysis failed: ", error.what());
            return FLUXCAP_STATUS_SYSTEM_ERROR;
        }

        QueryPerformanceCounter(&after);
        const uint64_t delta = static_cast<uint64_t>(after.QuadPart - before.QuadPart);
        const uint64_t duration_ns = qpc_delta_to_ns(delta, qpc_frequency_);
        if (sequence_ == (std::numeric_limits<uint64_t>::max() >> kSlotStateBits)) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            set_last_error("frame sequence space is exhausted");
            return FLUXCAP_STATUS_SYSTEM_ERROR;
        }
        slot.sequence = ++sequence_;
        slot.timestamp_qpc = static_cast<uint64_t>(after.QuadPart);
        slot.capture_duration_ns = duration_ns;

        stats_.captured_frames.fetch_add(1, std::memory_order_relaxed);
        stats_.total_capture_ns.fetch_add(duration_ns, std::memory_order_relaxed);
        stats_.last_capture_ns.store(duration_ns, std::memory_order_relaxed);
        return FLUXCAP_STATUS_OK;
    }

    void fill_frame(FrameSlot& slot, size_t slot_index, fluxcap_frame& output) const noexcept {
        output = {};
        output.pixels = slot.pixels;
        output.width = static_cast<uint32_t>(region_.width);
        output.height = static_cast<uint32_t>(region_.height);
        output.stride = slot.stride;
        output.format = FLUXCAP_PIXEL_FORMAT_BGRX8;
        output.desktop_region = region_;
        output.dirty_regions = slot.dirty_regions.empty()
            ? nullptr
            : slot.dirty_regions.data();
        output.dirty_region_count = static_cast<uint32_t>(slot.dirty_regions.size());
        output.sequence = slot.sequence;
        output.timestamp_qpc = slot.timestamp_qpc;
        output.qpc_frequency = qpc_frequency_;
        output.capture_duration_ns = slot.capture_duration_ns;
        output.dirty_base_sequence = slot.dirty_base_sequence;
        output._internal_token = output.sequence;
        output._internal_slot = static_cast<uint32_t>(slot_index);
    }

    void worker_main_impl() {
        ScopedDpiAwareness dpi;
        ScreenDc source_dc;
        if (source_dc.get() == nullptr) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            set_last_error(win32_failure("GetDC"));
            record_worker_error(
                FLUXCAP_STATUS_SYSTEM_ERROR,
                g_last_error.data(),
                true);
            return;
        }
        if ((config_.flags & FLUXCAP_FLAG_HIGH_PRIORITY_THREAD) != 0) {
            (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        }

        using Clock = std::chrono::steady_clock;
        Clock::time_point next_deadline = Clock::now();
        Clock::duration frame_period{};
        if (config_.target_fps != 0) {
            frame_period = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / config_.target_fps));
        }

        while (!stop_requested_.load(std::memory_order_acquire)) {
            bool captured = false;
            {
                std::scoped_lock producer_lock(producer_mutex_);
                size_t slot_index = 0;
                const bool preserve_mailbox = active_acquires_.load(
                    std::memory_order_acquire) != 0;
                FrameSlot* slot = acquire_write_slot(slot_index, preserve_mailbox);
                if (slot == nullptr) {
                    stats_.no_buffer_skips.fetch_add(1, std::memory_order_relaxed);
                } else {
                    const fluxcap_status status = capture_into(*slot, source_dc);
                    if (status == FLUXCAP_STATUS_OK) {
                        clear_worker_error_message();
                        {
                            std::lock_guard event_lock(event_mutex_);
                            slot->control.store(
                                slot_control(slot->sequence, SlotState::ready),
                                std::memory_order_release);
                        }
                        event_cv_.notify_all();
                        captured = true;
                    } else {
                        record_worker_error(status, g_last_error.data(), false);
                        release_write_slot(*slot);
                    }
                }
            }

            if (config_.target_fps != 0) {
                next_deadline += frame_period;
                const auto now = Clock::now();
                if (next_deadline <= now) {
                    next_deadline = now;
                    continue;
                }
                std::unique_lock event_lock(event_mutex_);
                event_cv_.wait_until(event_lock, next_deadline, [this] {
                    return stop_requested_.load(std::memory_order_acquire);
                });
            } else if (!captured) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

    }

    void worker_main() noexcept {
        try {
            worker_main_impl();
        } catch (const std::bad_alloc&) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            record_worker_error(
                FLUXCAP_STATUS_OUT_OF_MEMORY,
                "capture worker stopped because memory allocation failed",
                true);
        } catch (const std::exception& error) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            set_last_error_parts("capture worker stopped: ", error.what());
            record_worker_error(
                FLUXCAP_STATUS_SYSTEM_ERROR,
                g_last_error.data(),
                true);
        } catch (...) {
            stats_.capture_failures.fetch_add(1, std::memory_order_relaxed);
            record_worker_error(
                FLUXCAP_STATUS_SYSTEM_ERROR,
                "capture worker stopped because of an unknown failure",
                true);
        }

        for (auto& slot : slots_) {
            uint64_t control = slot->control.load(std::memory_order_acquire);
            if (slot_state(control) == SlotState::writing) {
                slot->control.store(
                    slot_control(slot_generation(control), SlotState::free),
                    std::memory_order_release);
            }
        }
        {
            std::lock_guard event_lock(event_mutex_);
            running_.store(false, std::memory_order_release);
        }
        event_cv_.notify_all();
    }

    void cleanup() noexcept {
        slots_.clear();
        dirty_tracker_.reset();
    }

    fluxcap_config config_{};
    fluxcap_rect region_{};
    HCURSOR cached_cursor_ = nullptr;
    POINT cursor_hotspot_{};
    uint64_t qpc_frequency_ = 0;
    uint64_t sequence_ = 0;
    std::vector<std::unique_ptr<FrameSlot>> slots_;
    std::unique_ptr<DirtyTracker> dirty_tracker_;
    AtomicStats stats_;

    std::mutex producer_mutex_;
    std::mutex control_mutex_;
    std::mutex event_mutex_;
    std::condition_variable event_cv_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> active_acquires_{0};
    mutable SRWLOCK worker_error_lock_ = SRWLOCK_INIT;
    std::array<char, 512> worker_error_{};
    std::atomic<fluxcap_status> worker_terminal_status_{FLUXCAP_STATUS_OK};
};

struct DisplayEnumerationContext final {
    std::vector<fluxcap_display> displays;
    bool allocation_failed = false;
};

BOOL CALLBACK enumerate_monitor(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM parameter) noexcept {
    auto& context = *reinterpret_cast<DisplayEnumerationContext*>(parameter);
    try {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return TRUE;
        }

        fluxcap_display display{};
        display.struct_size = sizeof(display);
        display.bounds = {
            info.rcMonitor.left,
            info.rcMonitor.top,
            info.rcMonitor.right - info.rcMonitor.left,
            info.rcMonitor.bottom - info.rcMonitor.top};
        display.work_area = {
            info.rcWork.left,
            info.rcWork.top,
            info.rcWork.right - info.rcWork.left,
            info.rcWork.bottom - info.rcWork.top};
        if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0) {
            display.flags |= FLUXCAP_DISPLAY_PRIMARY;
        }

        (void)WideCharToMultiByte(
            CP_UTF8,
            0,
            info.szDevice,
            -1,
            display.device_name,
            static_cast<int>(sizeof(display.device_name)),
            nullptr,
            nullptr);

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)
            && mode.dmDisplayFrequency > 1) {
            display.refresh_hz = mode.dmDisplayFrequency;
        }
        context.displays.push_back(display);
        return TRUE;
    } catch (const std::bad_alloc&) {
        context.allocation_failed = true;
        return FALSE;
    } catch (...) {
        return FALSE;
    }
}

fluxcap_status translate_exception() noexcept {
    try {
        throw;
    } catch (const CaptureError& error) {
        set_last_error(error.what());
        return error.status();
    } catch (const std::bad_alloc&) {
        set_last_error("memory allocation failed");
        return FLUXCAP_STATUS_OUT_OF_MEMORY;
    } catch (const std::exception& error) {
        set_last_error(error.what());
        return FLUXCAP_STATUS_SYSTEM_ERROR;
    } catch (...) {
        set_last_error("unknown internal failure");
        return FLUXCAP_STATUS_SYSTEM_ERROR;
    }
}

} // namespace fluxcap::internal

struct fluxcap_session final {
    std::unique_ptr<fluxcap::internal::Session> implementation;
};

extern "C" {

fluxcap_config FLUXCAP_CALL fluxcap_config_default(void) {
    fluxcap_config config{};
    config.struct_size = sizeof(config);
    config.abi_version = FLUXCAP_ABI_VERSION;
    config.buffer_count = 3;
    config.tile_size = 64;
    config.target_fps = 0;
    config.flags = FLUXCAP_FLAG_INCLUDE_LAYERED_WINDOWS
        | FLUXCAP_FLAG_DETECT_DIRTY_REGIONS;
    return config;
}

fluxcap_status FLUXCAP_CALL fluxcap_enumerate_displays(
    fluxcap_display* displays,
    uint32_t capacity,
    uint32_t* display_count) {
    using namespace fluxcap::internal;
    clear_last_error();
    if (display_count == nullptr || (displays == nullptr && capacity != 0)) {
        set_last_error("display_count is required and capacity requires a display buffer");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }

    try {
        ScopedDpiAwareness dpi;
        DisplayEnumerationContext context;
        if (!EnumDisplayMonitors(
                nullptr,
                nullptr,
                &enumerate_monitor,
                reinterpret_cast<LPARAM>(&context))) {
            if (context.allocation_failed) {
                set_last_error("memory allocation failed while enumerating displays");
                return FLUXCAP_STATUS_OUT_OF_MEMORY;
            }
            set_last_error(win32_failure("EnumDisplayMonitors"));
            return FLUXCAP_STATUS_SYSTEM_ERROR;
        }

        *display_count = static_cast<uint32_t>(context.displays.size());
        const uint32_t copy_count = std::min(capacity, *display_count);
        for (uint32_t index = 0; index < copy_count; ++index) {
            if (displays[index].struct_size != sizeof(fluxcap_display)) {
                set_last_error(
                    "each output display struct_size must match the FluxCap 1 ABI");
                return FLUXCAP_STATUS_INVALID_ARGUMENT;
            }
        }
        for (uint32_t index = 0; index < copy_count; ++index) {
            displays[index] = context.displays[index];
        }
        return FLUXCAP_STATUS_OK;
    } catch (...) {
        return translate_exception();
    }
}

fluxcap_status FLUXCAP_CALL fluxcap_create(
    const fluxcap_config* config,
    fluxcap_session** out_session) {
    using namespace fluxcap::internal;
    clear_last_error();
    if (out_session == nullptr) {
        set_last_error("out_session must not be null");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }
    *out_session = nullptr;

    try {
        fluxcap_config effective{};
        if (config == nullptr) {
            effective = fluxcap_config_default();
        } else {
            uint32_t caller_size = 0;
            std::memcpy(&caller_size, config, sizeof(caller_size));
            if (caller_size != sizeof(fluxcap_config)) {
                set_last_error("config.struct_size does not match the FluxCap 1 ABI");
                return FLUXCAP_STATUS_INVALID_ARGUMENT;
            }
            std::memcpy(&effective, config, sizeof(effective));
        }
        auto session = std::make_unique<fluxcap_session>();
        session->implementation = std::make_unique<Session>(effective);
        *out_session = session.release();
        return FLUXCAP_STATUS_OK;
    } catch (...) {
        return translate_exception();
    }
}

void FLUXCAP_CALL fluxcap_destroy(fluxcap_session* session) {
    delete session;
}

fluxcap_status FLUXCAP_CALL fluxcap_capture(
    fluxcap_session* session,
    fluxcap_frame* out_frame) {
    using namespace fluxcap::internal;
    clear_last_error();
    if (session == nullptr || session->implementation == nullptr || out_frame == nullptr) {
        set_last_error("session and out_frame must not be null");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }
    if (out_frame->_internal_owner != nullptr) {
        set_last_error("out_frame already contains an active lease");
        return FLUXCAP_STATUS_INVALID_STATE;
    }
    *out_frame = {};
    try {
        const fluxcap_status status = session->implementation->capture(*out_frame);
        if (status == FLUXCAP_STATUS_OK) {
            out_frame->_internal_owner = session;
        }
        return status;
    } catch (...) {
        return translate_exception();
    }
}

fluxcap_status FLUXCAP_CALL fluxcap_start(fluxcap_session* session) {
    using namespace fluxcap::internal;
    clear_last_error();
    if (session == nullptr || session->implementation == nullptr) {
        set_last_error("session must not be null");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }
    try {
        return session->implementation->start();
    } catch (...) {
        return translate_exception();
    }
}

fluxcap_status FLUXCAP_CALL fluxcap_stop(fluxcap_session* session) {
    using namespace fluxcap::internal;
    clear_last_error();
    if (session == nullptr || session->implementation == nullptr) {
        set_last_error("session must not be null");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }
    try {
        return session->implementation->stop();
    } catch (...) {
        return translate_exception();
    }
}

fluxcap_status FLUXCAP_CALL fluxcap_acquire_latest(
    fluxcap_session* session,
    uint32_t timeout_ms,
    fluxcap_frame* out_frame) {
    using namespace fluxcap::internal;
    clear_last_error();
    if (session == nullptr || session->implementation == nullptr || out_frame == nullptr) {
        set_last_error("session and out_frame must not be null");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }
    if (out_frame->_internal_owner != nullptr) {
        set_last_error("out_frame already contains an active lease");
        return FLUXCAP_STATUS_INVALID_STATE;
    }
    *out_frame = {};
    try {
        const fluxcap_status status = session->implementation->acquire_latest(
            timeout_ms,
            *out_frame);
        if (status == FLUXCAP_STATUS_OK) {
            out_frame->_internal_owner = session;
        }
        return status;
    } catch (...) {
        return translate_exception();
    }
}

fluxcap_status FLUXCAP_CALL fluxcap_release_frame(fluxcap_frame* frame) {
    using namespace fluxcap::internal;
    clear_last_error();
    if (frame == nullptr || frame->_internal_owner == nullptr) {
        set_last_error("frame does not contain an active lease");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }

    auto* session = static_cast<fluxcap_session*>(frame->_internal_owner);
    if (session->implementation == nullptr) {
        set_last_error("frame lease owner is invalid");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }
    const fluxcap_status status = session->implementation->release(
        frame->_internal_slot,
        frame->_internal_token);
    if (status == FLUXCAP_STATUS_OK) {
        *frame = {};
    }
    return status;
}

fluxcap_status FLUXCAP_CALL fluxcap_get_stats(
    const fluxcap_session* session,
    fluxcap_stats* out_stats) {
    using namespace fluxcap::internal;
    clear_last_error();
    if (session == nullptr || session->implementation == nullptr || out_stats == nullptr) {
        set_last_error("session and out_stats must not be null");
        return FLUXCAP_STATUS_INVALID_ARGUMENT;
    }
    session->implementation->read_stats(*out_stats);
    return FLUXCAP_STATUS_OK;
}

const char* FLUXCAP_CALL fluxcap_status_string(fluxcap_status status) {
    switch (status) {
    case FLUXCAP_STATUS_OK:
        return "ok";
    case FLUXCAP_STATUS_INVALID_ARGUMENT:
        return "invalid argument";
    case FLUXCAP_STATUS_OUT_OF_MEMORY:
        return "out of memory";
    case FLUXCAP_STATUS_SYSTEM_ERROR:
        return "system error";
    case FLUXCAP_STATUS_INVALID_STATE:
        return "invalid state";
    case FLUXCAP_STATUS_TIMEOUT:
        return "timeout";
    case FLUXCAP_STATUS_NO_BUFFER:
        return "no frame buffer available";
    case FLUXCAP_STATUS_NOT_SUPPORTED:
        return "not supported";
    default:
        return "unknown status";
    }
}

const char* FLUXCAP_CALL fluxcap_last_error(void) {
    return fluxcap::internal::g_last_error.data();
}

const char* FLUXCAP_CALL fluxcap_version_string(void) {
    return "0.1.0";
}

} // extern "C"
