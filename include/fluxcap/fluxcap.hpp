#ifndef FLUXCAP_FLUXCAP_HPP
#define FLUXCAP_FLUXCAP_HPP

#include "fluxcap.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace fluxcap {

class Error final : public std::runtime_error {
public:
    Error(fluxcap_status status, const std::string& message)
        : std::runtime_error(message), status_(status) {}

    [[nodiscard]] fluxcap_status status() const noexcept { return status_; }

private:
    fluxcap_status status_;
};

namespace detail {

inline void check(fluxcap_status status) {
    if (status == FLUXCAP_STATUS_OK) {
        return;
    }
    std::string message = fluxcap_status_string(status);
    const char* detail = fluxcap_last_error();
    if (detail != nullptr && detail[0] != '\0') {
        message.append(": ").append(detail);
    }
    throw Error(status, message);
}

struct SessionHandle final {
    explicit SessionHandle(fluxcap_session* value) noexcept : value(value) {}
    ~SessionHandle() { fluxcap_destroy(value); }
    SessionHandle(const SessionHandle&) = delete;
    SessionHandle& operator=(const SessionHandle&) = delete;
    fluxcap_session* value;
};

} // namespace detail

class Frame final {
public:
    Frame() = default;
    ~Frame() { reset(); }

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    Frame(Frame&& other) noexcept
        : owner_(std::move(other.owner_)), frame_(other.frame_) {
        other.frame_ = {};
    }

    Frame& operator=(Frame&& other) noexcept {
        if (this != &other) {
            reset();
            owner_ = std::move(other.owner_);
            frame_ = other.frame_;
            other.frame_ = {};
        }
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return frame_.pixels != nullptr;
    }
    [[nodiscard]] const uint8_t* data() const noexcept { return frame_.pixels; }
    [[nodiscard]] uint32_t width() const noexcept { return frame_.width; }
    [[nodiscard]] uint32_t height() const noexcept { return frame_.height; }
    [[nodiscard]] uint32_t stride() const noexcept { return frame_.stride; }
    [[nodiscard]] uint64_t sequence() const noexcept { return frame_.sequence; }
    [[nodiscard]] uint64_t timestamp_qpc() const noexcept { return frame_.timestamp_qpc; }
    [[nodiscard]] uint64_t capture_duration_ns() const noexcept {
        return frame_.capture_duration_ns;
    }
    [[nodiscard]] uint64_t dirty_base_sequence() const noexcept {
        return frame_.dirty_base_sequence;
    }
    [[nodiscard]] fluxcap_rect desktop_region() const noexcept {
        return frame_.desktop_region;
    }
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept {
        return {frame_.pixels, static_cast<size_t>(frame_.stride) * frame_.height};
    }
    [[nodiscard]] std::span<const fluxcap_rect> dirty_regions() const noexcept {
        return {frame_.dirty_regions, frame_.dirty_region_count};
    }
    [[nodiscard]] const fluxcap_frame& native() const noexcept { return frame_; }

    void reset() noexcept {
        if (frame_._internal_owner != nullptr) {
            (void)fluxcap_release_frame(&frame_);
        }
        owner_.reset();
        frame_ = {};
    }

private:
    friend class Session;

    Frame(std::shared_ptr<detail::SessionHandle> owner, fluxcap_frame frame) noexcept
        : owner_(std::move(owner)), frame_(frame) {}

    std::shared_ptr<detail::SessionHandle> owner_;
    fluxcap_frame frame_{};
};

class Session final {
public:
    explicit Session(const fluxcap_config& config = fluxcap_config_default()) {
        fluxcap_session* raw = nullptr;
        detail::check(fluxcap_create(&config, &raw));
        handle_ = std::make_shared<detail::SessionHandle>(raw);
    }

    ~Session() {
        if (handle_) {
            (void)fluxcap_stop(handle_->value);
        }
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept = default;
    Session& operator=(Session&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                (void)fluxcap_stop(handle_->value);
            }
            handle_ = std::move(other.handle_);
        }
        return *this;
    }

    [[nodiscard]] Frame capture() {
        fluxcap_frame raw{};
        detail::check(fluxcap_capture(handle_->value, &raw));
        return Frame(handle_, raw);
    }

    void start() { detail::check(fluxcap_start(handle_->value)); }
    void stop() { detail::check(fluxcap_stop(handle_->value)); }

    template <class Rep, class Period>
    [[nodiscard]] std::optional<Frame> acquire_latest(
        std::chrono::duration<Rep, Period> timeout) {
        const auto millis = timeout <= std::chrono::duration<Rep, Period>::zero()
            ? std::chrono::milliseconds::zero()
            : std::chrono::ceil<std::chrono::milliseconds>(timeout);
        const auto bounded = millis.count() >= static_cast<int64_t>(FLUXCAP_WAIT_INFINITE)
                ? FLUXCAP_WAIT_INFINITE - 1u
                : static_cast<uint32_t>(millis.count());
        fluxcap_frame raw{};
        const auto status = fluxcap_acquire_latest(handle_->value, bounded, &raw);
        if (status == FLUXCAP_STATUS_TIMEOUT) {
            return std::nullopt;
        }
        detail::check(status);
        return Frame(handle_, raw);
    }

    [[nodiscard]] fluxcap_stats stats() const {
        fluxcap_stats value{};
        detail::check(fluxcap_get_stats(handle_->value, &value));
        return value;
    }

    [[nodiscard]] fluxcap_session* native_handle() const noexcept {
        return handle_ ? handle_->value : nullptr;
    }

private:
    std::shared_ptr<detail::SessionHandle> handle_;
};

} // namespace fluxcap

#endif
