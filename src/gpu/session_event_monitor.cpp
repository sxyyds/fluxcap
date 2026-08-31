#include "session_event_monitor.hpp"

#include <windows.h>
#include <wtsapi32.h>

#include <chrono>
#include <thread>

namespace fluxcap::gpu::internal {
namespace {

std::uint32_t translate_session_message(WPARAM parameter) noexcept {
    switch (parameter) {
    case WTS_SESSION_LOCK: return session_event_locked;
    case WTS_SESSION_UNLOCK: return session_event_unlocked;
    case WTS_REMOTE_CONNECT: return session_event_remote_connect;
    case WTS_REMOTE_DISCONNECT: return session_event_remote_disconnect;
    case WTS_CONSOLE_CONNECT: return session_event_console_connect;
    case WTS_CONSOLE_DISCONNECT: return session_event_console_disconnect;
    default: return 0;
    }
}

struct MonitorWindowContext final {
    SessionEventMonitor* monitor = nullptr;
};

constexpr wchar_t window_class_name[] = L"FluxCapSessionEventMonitor";

LRESULT CALLBACK monitor_window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) noexcept {
    switch (message) {
    case WM_WTSSESSION_CHANGE: {
        if (auto* context = reinterpret_cast<MonitorWindowContext*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            context != nullptr && context->monitor != nullptr) {
            const std::uint32_t flags = translate_session_message(wparam);
            if (flags != 0) context->monitor->report(flags);
        }
        return 0;
    }
    case WM_POWERBROADCAST: {
        if (wparam == PBT_APMSUSPEND || wparam == PBT_APMRESUMEAUTOMATIC
            || wparam == PBT_APMRESUMESUSPEND) {
            if (auto* context = reinterpret_cast<MonitorWindowContext*>(
                    GetWindowLongPtrW(window, GWLP_USERDATA));
                context != nullptr && context->monitor != nullptr) {
                context->monitor->report(
                    wparam == PBT_APMSUSPEND ? session_event_suspend
                                             : session_event_resume);
            }
        }
        return TRUE;
    }
    case WM_DESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void monitor_thread_main(SessionEventMonitor* monitor, HANDLE stop_event) {
    // A unique-per-process class name keeps repeated create/stop cycles from
    // colliding with a still-registered atom.
    wchar_t class_name[64]{};
    (void)swprintf_s(
        class_name,
        L"%s-%p",
        window_class_name,
        static_cast<const void*>(monitor));

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &monitor_window_procedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    const ATOM registered = RegisterClassExW(&window_class);
    if (registered == 0) return;

    MonitorWindowContext context{monitor};
    HWND window = CreateWindowExW(
        0,
        class_name,
        L"FluxCapSessionEventMonitor",
        WS_OVERLAPPEDWINDOW /* ignored for message-only windows */,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        window_class.hInstance,
        nullptr);
    if (window == nullptr) {
        UnregisterClassW(class_name, window_class.hInstance);
        return;
    }
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&context));

    const BOOL notified = WTSRegisterSessionNotification(
        window, NOTIFY_FOR_THIS_SESSION);
    HPOWERNOTIFY power_notification = nullptr;
    if (notified) {
        // RegisterSuspendResumeNotification (Windows 8+) delivers
        // WM_POWERBROADCAST through the same window.
        power_notification = RegisterSuspendResumeNotification(window, 0);
    }
    if (notified) {
        monitor->report(0 /* marks the monitor as active without an event */);
    }

    MSG message{};
    for (;;) {
        const BOOL yielded = MsgWaitForMultipleObjects(
            1,
            &stop_event,
            FALSE,
            INFINITE,
            QS_ALLINPUT);
        if (yielded == WAIT_OBJECT_0) break;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) goto finished;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

finished:
    if (power_notification != nullptr) {
        UnregisterSuspendResumeNotification(power_notification);
    }
    if (notified) WTSUnRegisterSessionNotification(window);
    if (IsWindow(window)) DestroyWindow(window);
    UnregisterClassW(class_name, window_class.hInstance);
}

} // namespace

SessionEventMonitor::SessionEventMonitor() noexcept = default;

SessionEventMonitor::~SessionEventMonitor() { stop(); }

void SessionEventMonitor::report(std::uint32_t event_flags) noexcept {
    if (event_flags != 0) {
        flags_.store(event_flags, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    active_.store(true, std::memory_order_release);
}

void SessionEventMonitor::start() noexcept {
    if (active_.load(std::memory_order_acquire)) return;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop == nullptr) return;
    try {
        auto* thread = new std::thread(
            [this, stop]() noexcept {
                monitor_thread_main(this, stop);
            });
        thread_ = thread;
        stop_event_ = stop;
        // Wait briefly for the window thread to finish registering so that
        // callers observe `active()` deterministically right after start().
        for (int spin = 0; spin < 100; ++spin) {
            if (active_.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (...) {
        SetEvent(stop);
        CloseHandle(stop);
    }
}

void SessionEventMonitor::stop() noexcept {
    auto* thread = static_cast<std::thread*>(thread_);
    if (thread == nullptr) return;
    HANDLE stop = static_cast<HANDLE>(stop_event_);
    if (stop != nullptr) SetEvent(stop);
    if (thread->joinable()) thread->join();
    delete thread;
    thread_ = nullptr;
    if (stop != nullptr) CloseHandle(stop);
    stop_event_ = nullptr;
    active_.store(false, std::memory_order_release);
}

} // namespace fluxcap::gpu::internal
