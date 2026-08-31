#ifndef FLUXCAP_GPU_SESSION_EVENT_MONITOR_HPP
#define FLUXCAP_GPU_SESSION_EVENT_MONITOR_HPP

#include <atomic>
#include <cstdint>

namespace fluxcap::gpu::internal {

// Flags reported by SessionEventMonitor. They describe the desktop/session
// state that screen capture must react to with a full-frame refresh.
inline constexpr std::uint32_t session_event_locked = 1u << 0;
inline constexpr std::uint32_t session_event_unlocked = 1u << 1;
inline constexpr std::uint32_t session_event_remote_connect = 1u << 2;
inline constexpr std::uint32_t session_event_remote_disconnect = 1u << 3;
inline constexpr std::uint32_t session_event_console_connect = 1u << 4;
inline constexpr std::uint32_t session_event_console_disconnect = 1u << 5;
inline constexpr std::uint32_t session_event_suspend = 1u << 6;
inline constexpr std::uint32_t session_event_resume = 1u << 7;

// Owns a message-only window on a dedicated thread and registers
// WTSRegisterSessionNotification plus suspend/resume power notifications.
// Every event increments generation() so capture workers can cheaply poll for
// "something happened since I last looked". `flags()` returns the latest
// event kind. Construction failure leaves the monitor inert: generation stays
// zero and capture proceeds without session awareness.
class SessionEventMonitor final {
public:
    SessionEventMonitor() noexcept;
    ~SessionEventMonitor();
    SessionEventMonitor(const SessionEventMonitor&) = delete;
    SessionEventMonitor& operator=(const SessionEventMonitor&) = delete;
    SessionEventMonitor(SessionEventMonitor&&) = delete;
    SessionEventMonitor& operator=(SessionEventMonitor&&) = delete;

    void start() noexcept;
    void stop() noexcept;

    // Called from the monitor window's message procedure (and once with zero
    // to publish successful registration).
    void report(std::uint32_t event_flags) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint32_t flags() const noexcept {
        return flags_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool active() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint32_t> flags_{0};
    std::atomic<bool> active_{false};
    void* thread_{nullptr};
    void* stop_event_{nullptr};
};

} // namespace fluxcap::gpu::internal
#endif
