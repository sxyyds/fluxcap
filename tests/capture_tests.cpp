#include <fluxcap/fluxcap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::string last_error_suffix() {
    const char* detail = fluxcap_last_error();
    if (detail == nullptr || detail[0] == '\0') {
        return {};
    }
    return std::string("; last_error: ") + detail;
}

[[noreturn]] void fail(std::string message) {
    message.append(last_error_suffix());
    throw std::runtime_error(std::move(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(std::string(message));
    }
}

void require_status(
    fluxcap_status actual,
    fluxcap_status expected,
    std::string_view operation) {
    if (actual == expected) {
        return;
    }

    std::ostringstream stream;
    stream << operation << ": expected " << fluxcap_status_string(expected)
           << ", got " << fluxcap_status_string(actual);
    fail(stream.str());
}

class SessionHandle final {
public:
    explicit SessionHandle(const fluxcap_config& config) {
        require_status(
            fluxcap_create(&config, &session_),
            FLUXCAP_STATUS_OK,
            "fluxcap_create");
        require(session_ != nullptr, "fluxcap_create returned a null session");
    }

    ~SessionHandle() {
        if (session_ != nullptr) {
            (void)fluxcap_stop(session_);
            fluxcap_destroy(session_);
        }
    }

    SessionHandle(const SessionHandle&) = delete;
    SessionHandle& operator=(const SessionHandle&) = delete;

    [[nodiscard]] fluxcap_session* get() const noexcept { return session_; }

private:
    fluxcap_session* session_ = nullptr;
};

class FrameLease final {
public:
    FrameLease() = default;

    ~FrameLease() {
        if (frame_._internal_owner != nullptr) {
            (void)fluxcap_release_frame(&frame_);
        }
    }

    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    [[nodiscard]] fluxcap_frame* put() {
        if (frame_._internal_owner != nullptr) {
            throw std::logic_error("attempted to overwrite an active frame lease");
        }
        frame_ = {};
        return &frame_;
    }

    [[nodiscard]] const fluxcap_frame& get() const noexcept { return frame_; }

    void release(std::string_view operation = "fluxcap_release_frame") {
        require(frame_._internal_owner != nullptr, "frame does not hold an active lease");
        require_status(
            fluxcap_release_frame(&frame_),
            FLUXCAP_STATUS_OK,
            operation);
        require(frame_._internal_owner == nullptr, "released frame was not cleared");
    }

private:
    fluxcap_frame frame_{};
};

[[nodiscard]] bool rect_equal(
    const fluxcap_rect& left,
    const fluxcap_rect& right) noexcept {
    return left.x == right.x
        && left.y == right.y
        && left.width == right.width
        && left.height == right.height;
}

[[nodiscard]] fluxcap_rect primary_display_bounds() {
    std::uint32_t count = 0;
    require_status(
        fluxcap_enumerate_displays(nullptr, 0, &count),
        FLUXCAP_STATUS_OK,
        "fluxcap_enumerate_displays(count)");
    require(count > 0, "display enumeration returned no displays");

    std::vector<fluxcap_display> displays(count);
    for (fluxcap_display& display : displays) {
        display.struct_size = sizeof(display);
    }

    std::uint32_t reported_count = 0;
    require_status(
        fluxcap_enumerate_displays(displays.data(), count, &reported_count),
        FLUXCAP_STATUS_OK,
        "fluxcap_enumerate_displays(data)");
    require(reported_count > 0, "display enumeration became empty");

    const std::size_t available = std::min<std::size_t>(
        displays.size(),
        static_cast<std::size_t>(reported_count));
    const auto primary = std::find_if(
        displays.begin(),
        displays.begin() + static_cast<std::ptrdiff_t>(available),
        [](const fluxcap_display& display) {
            return (display.flags & FLUXCAP_DISPLAY_PRIMARY) != 0;
        });
    const fluxcap_display& selected = primary != displays.begin()
            + static_cast<std::ptrdiff_t>(available)
        ? *primary
        : displays.front();

    require(
        selected.bounds.width > 0 && selected.bounds.height > 0,
        "selected display has invalid bounds");
    return selected.bounds;
}

[[nodiscard]] fluxcap_rect small_region() {
    const fluxcap_rect bounds = primary_display_bounds();
    const std::int32_t width = std::min<std::int32_t>(64, bounds.width);
    const std::int32_t height = std::min<std::int32_t>(48, bounds.height);
    return {
        bounds.x + (bounds.width - width) / 2,
        bounds.y + (bounds.height - height) / 2,
        width,
        height};
}

[[nodiscard]] fluxcap_config small_config() {
    fluxcap_config config = fluxcap_config_default();
    config.region = small_region();
    return config;
}

void validate_frame(const fluxcap_frame& frame, std::string_view context) {
    require(frame.pixels != nullptr, std::string(context) + " returned null pixels");
    require(frame.width > 0 && frame.height > 0,
        std::string(context) + " returned empty dimensions");
    require(
        frame.stride >= static_cast<std::uint64_t>(frame.width) * 4u,
        std::string(context) + " returned an invalid stride");
    require(frame.format == FLUXCAP_PIXEL_FORMAT_BGRX8,
        std::string(context) + " returned an unexpected pixel format");
    require(frame.desktop_region.width == static_cast<std::int32_t>(frame.width)
            && frame.desktop_region.height == static_cast<std::int32_t>(frame.height),
        std::string(context) + " returned inconsistent desktop bounds");
    require(frame.sequence > 0, std::string(context) + " returned sequence zero");
    require(frame.timestamp_qpc > 0,
        std::string(context) + " returned timestamp zero");
    require(frame.qpc_frequency > 0,
        std::string(context) + " returned QPC frequency zero");
    require(frame._internal_owner != nullptr,
        std::string(context) + " did not return a frame lease");
    require(frame.dirty_region_count == 0 || frame.dirty_regions != nullptr,
        std::string(context) + " returned invalid dirty-region storage");
}

void test_default_synchronous_capture() {
    const fluxcap_config config = fluxcap_config_default();
    SessionHandle session(config);
    FrameLease frame;

    require_status(
        fluxcap_capture(session.get(), frame.put()),
        FLUXCAP_STATUS_OK,
        "default fluxcap_capture");
    validate_frame(frame.get(), "default capture");

    require(frame.get().dirty_region_count == 1,
        "default first frame was not reported as one full dirty region");
    const fluxcap_rect expected_dirty{
        0,
        0,
        static_cast<std::int32_t>(frame.get().width),
        static_cast<std::int32_t>(frame.get().height)};
    require(rect_equal(frame.get().dirty_regions[0], expected_dirty),
        "default first-frame dirty region did not cover the frame");
    frame.release();
}

void test_small_roi_synchronous_capture() {
    const fluxcap_config config = small_config();
    SessionHandle session(config);
    FrameLease frame;

    require_status(
        fluxcap_capture(session.get(), frame.put()),
        FLUXCAP_STATUS_OK,
        "ROI fluxcap_capture");
    validate_frame(frame.get(), "ROI capture");
    require(frame.get().width == static_cast<std::uint32_t>(config.region.width)
            && frame.get().height == static_cast<std::uint32_t>(config.region.height),
        "ROI capture returned the wrong dimensions");
    require(rect_equal(frame.get().desktop_region, config.region),
        "ROI capture returned the wrong desktop region");
    frame.release();
}

void test_frame_lease_exhaustion_and_release() {
    fluxcap_config config = small_config();
    config.buffer_count = 2;
    config.flags = 0;
    SessionHandle session(config);
    FrameLease first;
    FrameLease second;
    FrameLease third;

    require_status(
        fluxcap_capture(session.get(), first.put()),
        FLUXCAP_STATUS_OK,
        "first leased capture");
    require_status(
        fluxcap_capture(session.get(), second.put()),
        FLUXCAP_STATUS_OK,
        "second leased capture");

    const fluxcap_status exhausted = fluxcap_capture(session.get(), third.put());
    require_status(
        exhausted,
        FLUXCAP_STATUS_NO_BUFFER,
        "capture with all buffers leased");
    require(!last_error_suffix().empty(),
        "NO_BUFFER did not provide fluxcap_last_error diagnostics");

    first.release("release first exhausted-buffer lease");
    require_status(
        fluxcap_capture(session.get(), third.put()),
        FLUXCAP_STATUS_OK,
        "capture after releasing a buffer");
    validate_frame(third.get(), "post-release capture");

    second.release("release second exhausted-buffer lease");
    third.release("release post-exhaustion lease");

    fluxcap_stats stats{};
    require_status(
        fluxcap_get_stats(session.get(), &stats),
        FLUXCAP_STATUS_OK,
        "stats after buffer exhaustion");
    require(stats.no_buffer_skips == 1,
        "buffer exhaustion did not increment no_buffer_skips exactly once");
}

void test_async_start_acquire_stop() {
    fluxcap_config config = small_config();
    config.target_fps = 60;
    config.flags = 0;
    SessionHandle session(config);

    require_status(
        fluxcap_start(session.get()),
        FLUXCAP_STATUS_OK,
        "fluxcap_start");

    FrameLease first;
    require_status(
        fluxcap_acquire_latest(session.get(), 3000, first.put()),
        FLUXCAP_STATUS_OK,
        "first asynchronous acquire");
    validate_frame(first.get(), "first asynchronous frame");
    const std::uint64_t first_sequence = first.get().sequence;
    first.release("release first asynchronous frame");

    FrameLease second;
    require_status(
        fluxcap_acquire_latest(session.get(), 3000, second.put()),
        FLUXCAP_STATUS_OK,
        "second asynchronous acquire");
    validate_frame(second.get(), "second asynchronous frame");
    require(second.get().sequence > first_sequence,
        "asynchronous frame sequence did not advance");
    second.release("release second asynchronous frame");

    require_status(
        fluxcap_stop(session.get()),
        FLUXCAP_STATUS_OK,
        "fluxcap_stop");
}

void test_capture_stats() {
    fluxcap_config config = small_config();
    config.flags = 0;
    SessionHandle session(config);

    fluxcap_stats initial{};
    require_status(
        fluxcap_get_stats(session.get(), &initial),
        FLUXCAP_STATUS_OK,
        "initial fluxcap_get_stats");
    require(initial.captured_frames == 0
            && initial.overwritten_frames == 0
            && initial.no_buffer_skips == 0
            && initial.capture_failures == 0
            && initial.total_capture_ns == 0
            && initial.last_capture_ns == 0,
        "new session statistics were not zero initialized");

    FrameLease first;
    require_status(
        fluxcap_capture(session.get(), first.put()),
        FLUXCAP_STATUS_OK,
        "first stats capture");
    const std::uint64_t first_duration = first.get().capture_duration_ns;
    first.release("release first stats frame");

    FrameLease second;
    require_status(
        fluxcap_capture(session.get(), second.put()),
        FLUXCAP_STATUS_OK,
        "second stats capture");
    const std::uint64_t second_duration = second.get().capture_duration_ns;
    second.release("release second stats frame");

    fluxcap_stats stats{};
    require_status(
        fluxcap_get_stats(session.get(), &stats),
        FLUXCAP_STATUS_OK,
        "final fluxcap_get_stats");
    require(stats.captured_frames == 2,
        "captured_frames did not match two synchronous captures");
    require(stats.overwritten_frames == 0,
        "synchronous captures unexpectedly overwrote a frame");
    require(stats.no_buffer_skips == 0,
        "released synchronous captures unexpectedly dropped a frame");
    require(stats.capture_failures == 0,
        "successful synchronous captures incremented capture_failures");
    require(stats.total_capture_ns == first_duration + second_duration,
        "total_capture_ns did not equal the per-frame durations");
    require(stats.last_capture_ns == second_duration,
        "last_capture_ns did not match the newest frame");
}

void test_active_output_frame_is_rejected() {
    fluxcap_config config = small_config();
    config.flags = 0;
    SessionHandle session(config);
    fluxcap_frame frame{};

    require_status(
        fluxcap_capture(session.get(), &frame),
        FLUXCAP_STATUS_OK,
        "capture into an empty output frame");
    require_status(
        fluxcap_capture(session.get(), &frame),
        FLUXCAP_STATUS_INVALID_STATE,
        "capture into an active output frame");
    require_status(
        fluxcap_release_frame(&frame),
        FLUXCAP_STATUS_OK,
        "release active output frame");

    require_status(
        fluxcap_capture(session.get(), &frame),
        FLUXCAP_STATUS_OK,
        "capture after active output rejection");
    require_status(
        fluxcap_release_frame(&frame),
        FLUXCAP_STATUS_OK,
        "release frame after active output rejection");
}

void test_stale_frame_copy_cannot_release_new_lease() {
    fluxcap_config config = small_config();
    config.buffer_count = 2;
    config.flags = 0;
    SessionHandle session(config);

    fluxcap_frame first{};
    require_status(
        fluxcap_capture(session.get(), &first),
        FLUXCAP_STATUS_OK,
        "capture original lease");
    fluxcap_frame stale_copy = first;
    require_status(
        fluxcap_release_frame(&first),
        FLUXCAP_STATUS_OK,
        "release original lease");

    fluxcap_frame replacement{};
    require_status(
        fluxcap_capture(session.get(), &replacement),
        FLUXCAP_STATUS_OK,
        "capture replacement lease");
    require(replacement._internal_slot == stale_copy._internal_slot,
        "replacement capture did not reuse the expected slot");
    require(replacement._internal_token != stale_copy._internal_token,
        "replacement capture did not advance the slot generation");

    require_status(
        fluxcap_release_frame(&stale_copy),
        FLUXCAP_STATUS_INVALID_STATE,
        "release stale copied lease");

    fluxcap_frame second_slot{};
    require_status(
        fluxcap_capture(session.get(), &second_slot),
        FLUXCAP_STATUS_OK,
        "capture while replacement lease remains active");
    fluxcap_frame exhausted{};
    require_status(
        fluxcap_capture(session.get(), &exhausted),
        FLUXCAP_STATUS_NO_BUFFER,
        "verify stale release did not free replacement slot");

    require_status(
        fluxcap_release_frame(&replacement),
        FLUXCAP_STATUS_OK,
        "release replacement lease");
    require_status(
        fluxcap_release_frame(&second_slot),
        FLUXCAP_STATUS_OK,
        "release second slot lease");
}

void test_async_timeout_with_all_slots_leased() {
    using Clock = std::chrono::steady_clock;

    fluxcap_config config = small_config();
    config.buffer_count = 2;
    config.flags = 0;
    config.target_fps = 0;
    SessionHandle session(config);
    require_status(
        fluxcap_start(session.get()),
        FLUXCAP_STATUS_OK,
        "start timeout test worker");

    FrameLease first;
    FrameLease second;
    require_status(
        fluxcap_acquire_latest(session.get(), 3000, first.put()),
        FLUXCAP_STATUS_OK,
        "acquire first held async frame");
    require_status(
        fluxcap_acquire_latest(session.get(), 3000, second.put()),
        FLUXCAP_STATUS_OK,
        "acquire second held async frame");

    fluxcap_frame timed_out{};
    const auto begin = Clock::now();
    require_status(
        fluxcap_acquire_latest(session.get(), 75, &timed_out),
        FLUXCAP_STATUS_TIMEOUT,
        "bounded acquire with all slots leased");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - begin);
    require(elapsed >= std::chrono::milliseconds(40),
        "bounded acquire returned substantially before its timeout");
    require(elapsed < std::chrono::milliseconds(500),
        "bounded acquire exceeded its timeout by too much");

    first.release("release first timeout-test frame");
    second.release("release second timeout-test frame");
    require_status(
        fluxcap_stop(session.get()),
        FLUXCAP_STATUS_OK,
        "stop timeout test worker");
}

void test_dirty_regions_name_their_base_sequence() {
    fluxcap_config config = small_config();
    config.flags = FLUXCAP_FLAG_DETECT_DIRTY_REGIONS;
    SessionHandle session(config);
    FrameLease first;
    FrameLease second;

    require_status(
        fluxcap_capture(session.get(), first.put()),
        FLUXCAP_STATUS_OK,
        "capture first dirty-base frame");
    require(first.get().dirty_base_sequence == 0,
        "first dirty frame did not use the full-frame base sentinel");
    const std::uint64_t first_sequence = first.get().sequence;
    first.release("release first dirty-base frame");

    require_status(
        fluxcap_capture(session.get(), second.put()),
        FLUXCAP_STATUS_OK,
        "capture second dirty-base frame");
    require(second.get().dirty_base_sequence == first_sequence,
        "dirty metadata did not name the preceding captured sequence");
    second.release("release second dirty-base frame");
}

void test_async_mailbox_stays_fresh_without_a_waiter() {
    fluxcap_config config = small_config();
    config.buffer_count = 2;
    config.flags = 0;
    config.target_fps = 0;
    SessionHandle session(config);
    require_status(
        fluxcap_start(session.get()),
        FLUXCAP_STATUS_OK,
        "start mailbox freshness worker");

    FrameLease held;
    require_status(
        fluxcap_acquire_latest(session.get(), 3000, held.put()),
        FLUXCAP_STATUS_OK,
        "acquire held mailbox frame");
    const std::uint64_t held_sequence = held.get().sequence;

    fluxcap_stats before{};
    require_status(
        fluxcap_get_stats(session.get(), &before),
        FLUXCAP_STATUS_OK,
        "read mailbox stats before idle period");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fluxcap_stats after{};
    require_status(
        fluxcap_get_stats(session.get(), &after),
        FLUXCAP_STATUS_OK,
        "read mailbox stats after idle period");
    require(after.captured_frames >= before.captured_frames + 2,
        "producer stopped refreshing its only ready mailbox frame");

    FrameLease newest;
    require_status(
        fluxcap_acquire_latest(session.get(), 3000, newest.put()),
        FLUXCAP_STATUS_OK,
        "acquire refreshed mailbox frame");
    require(newest.get().sequence > held_sequence,
        "refreshed mailbox frame did not advance its sequence");

    held.release("release held mailbox frame");
    newest.release("release refreshed mailbox frame");
    require_status(
        fluxcap_stop(session.get()),
        FLUXCAP_STATUS_OK,
        "stop mailbox freshness worker");
}

void test_multiple_async_consumers_make_progress() {
    constexpr int consumer_count = 4;
    constexpr int frames_per_consumer = 40;

    fluxcap_config config = small_config();
    config.buffer_count = 3;
    config.flags = 0;
    config.target_fps = 0;
    SessionHandle session(config);
    require_status(
        fluxcap_start(session.get()),
        FLUXCAP_STATUS_OK,
        "start multi-consumer worker");

    std::atomic<int> completed_frames{0};
    std::mutex error_mutex;
    std::string first_error;
    const auto record_error = [&](std::string message) {
        std::lock_guard lock(error_mutex);
        if (first_error.empty()) {
            first_error = std::move(message);
        }
    };

    std::vector<std::thread> consumers;
    consumers.reserve(consumer_count);
    for (int consumer = 0; consumer < consumer_count; ++consumer) {
        consumers.emplace_back([&, consumer] {
            for (int index = 0; index < frames_per_consumer; ++index) {
                fluxcap_frame frame{};
                const fluxcap_status acquire = fluxcap_acquire_latest(
                    session.get(),
                    3000,
                    &frame);
                if (acquire != FLUXCAP_STATUS_OK) {
                    record_error(
                        "consumer " + std::to_string(consumer)
                        + " acquire failed: " + fluxcap_status_string(acquire)
                        + "; " + fluxcap_last_error());
                    return;
                }
                if (frame.sequence == 0 || frame.pixels == nullptr) {
                    record_error(
                        "consumer " + std::to_string(consumer)
                        + " received an invalid frame");
                    (void)fluxcap_release_frame(&frame);
                    return;
                }
                const fluxcap_status release = fluxcap_release_frame(&frame);
                if (release != FLUXCAP_STATUS_OK) {
                    record_error(
                        "consumer " + std::to_string(consumer)
                        + " release failed: " + fluxcap_status_string(release)
                        + "; " + fluxcap_last_error());
                    return;
                }
                completed_frames.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& consumer : consumers) {
        consumer.join();
    }
    require_status(
        fluxcap_stop(session.get()),
        FLUXCAP_STATUS_OK,
        "stop multi-consumer worker");

    require(first_error.empty(), first_error.empty()
        ? "multi-consumer capture failed without diagnostics"
        : first_error);
    require(completed_frames.load(std::memory_order_relaxed)
            == consumer_count * frames_per_consumer,
        "not every asynchronous consumer made progress");
}

using TestFunction = std::function<void()>;

int run_test(std::string_view name, const TestFunction& test) {
    try {
        test();
        std::cout << "[PASS] " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << name << ": " << error.what();
        const std::string suffix = last_error_suffix();
        if (!suffix.empty() && std::string_view(error.what()).find("last_error:")
                == std::string_view::npos) {
            std::cerr << suffix;
        }
        std::cerr << '\n';
        return 1;
    } catch (...) {
        std::cerr << "[FAIL] " << name << ": unknown exception"
                  << last_error_suffix() << '\n';
        return 1;
    }
}

} // namespace

int main() {
    int failures = 0;
    failures += run_test(
        "default synchronous capture",
        test_default_synchronous_capture);
    failures += run_test(
        "small ROI synchronous capture",
        test_small_roi_synchronous_capture);
    failures += run_test(
        "frame lease exhaustion and release",
        test_frame_lease_exhaustion_and_release);
    failures += run_test(
        "asynchronous start, acquire, and stop",
        test_async_start_acquire_stop);
    failures += run_test("capture statistics", test_capture_stats);
    failures += run_test(
        "active output frame is rejected",
        test_active_output_frame_is_rejected);
    failures += run_test(
        "stale frame copy cannot release a new lease",
        test_stale_frame_copy_cannot_release_new_lease);
    failures += run_test(
        "bounded async acquire timeout",
        test_async_timeout_with_all_slots_leased);
    failures += run_test(
        "dirty regions name their base sequence",
        test_dirty_regions_name_their_base_sequence);
    failures += run_test(
        "async mailbox stays fresh without a waiter",
        test_async_mailbox_stays_fresh_without_a_waiter);
    failures += run_test(
        "multiple async consumers make progress",
        test_multiple_async_consumers_make_progress);
    return failures == 0 ? 0 : 1;
}
