#include "dirty_tracker.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using fluxcap::internal::DirtyRect;
using fluxcap::internal::DirtyTracker;

constexpr std::uint32_t kBytesPerPixel = 4;

class Image final {
public:
    Image(std::uint32_t width, std::uint32_t height)
        : width_(width),
          height_(height),
          stride_(width * kBytesPerPixel),
          pixels_(static_cast<std::size_t>(stride_) * height, 0) {}

    void fill_tile(
        std::uint32_t tile_column,
        std::uint32_t tile_row,
        std::uint32_t tile_size,
        std::uint8_t value) {
        const std::uint32_t x = tile_column * tile_size;
        const std::uint32_t y = tile_row * tile_size;
        if (x >= width_ || y >= height_) {
            throw std::out_of_range("test tile is outside the image");
        }

        const std::uint32_t tile_width = std::min(tile_size, width_ - x);
        const std::uint32_t tile_height = std::min(tile_size, height_ - y);
        for (std::uint32_t row = 0; row < tile_height; ++row) {
            const std::size_t offset = static_cast<std::size_t>(y + row) * stride_
                + static_cast<std::size_t>(x) * kBytesPerPixel;
            std::fill_n(
                pixels_.data() + offset,
                static_cast<std::size_t>(tile_width) * kBytesPerPixel,
                value);
        }
    }

    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return pixels_.data();
    }

    [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t stride_;
    std::vector<std::uint8_t> pixels_;
};

[[nodiscard]] bool equal(const DirtyRect& left, const DirtyRect& right) noexcept {
    return left.x == right.x
        && left.y == right.y
        && left.width == right.width
        && left.height == right.height;
}

[[nodiscard]] std::string to_string(const DirtyRect& rect) {
    std::ostringstream stream;
    stream << '{' << rect.x << ',' << rect.y << ','
           << rect.width << ',' << rect.height << '}';
    return stream.str();
}

void expect_rects(
    const std::vector<DirtyRect>& actual,
    std::initializer_list<DirtyRect> expected,
    std::string_view context) {
    if (actual.size() != expected.size()) {
        std::ostringstream stream;
        stream << context << ": expected " << expected.size()
               << " dirty rect(s), got " << actual.size();
        throw std::runtime_error(stream.str());
    }

    std::size_t index = 0;
    for (const DirtyRect& expected_rect : expected) {
        if (!equal(actual[index], expected_rect)) {
            std::ostringstream stream;
            stream << context << ": rect " << index << " expected "
                   << to_string(expected_rect) << ", got "
                   << to_string(actual[index]);
            throw std::runtime_error(stream.str());
        }
        ++index;
    }
}

void establish_baseline(
    DirtyTracker& tracker,
    const Image& image,
    std::vector<DirtyRect>& output) {
    tracker.analyze(image.data(), image.stride(), output);
}

void test_first_frame_is_fully_dirty() {
    constexpr std::uint32_t width = 19;
    constexpr std::uint32_t height = 18;
    Image image(width, height);
    DirtyTracker tracker(width, height, 8);
    std::vector<DirtyRect> output;

    tracker.analyze(image.data(), image.stride(), output);

    expect_rects(output, {{0, 0, 19, 18}}, "first frame");
}

void test_unchanged_frame_is_clean() {
    Image image(24, 16);
    DirtyTracker tracker(24, 16, 8);
    std::vector<DirtyRect> output;

    establish_baseline(tracker, image, output);
    tracker.analyze(image.data(), image.stride(), output);

    expect_rects(output, {}, "unchanged frame");
}

void test_single_changed_tile() {
    Image image(24, 24);
    DirtyTracker tracker(24, 24, 8);
    std::vector<DirtyRect> output;
    establish_baseline(tracker, image, output);

    image.fill_tile(1, 1, 8, 0x31);
    tracker.analyze(image.data(), image.stride(), output);

    expect_rects(output, {{8, 8, 8, 8}}, "single changed tile");
}

void test_horizontal_tiles_are_merged() {
    Image image(32, 24);
    DirtyTracker tracker(32, 24, 8);
    std::vector<DirtyRect> output;
    establish_baseline(tracker, image, output);

    image.fill_tile(1, 1, 8, 0x42);
    image.fill_tile(2, 1, 8, 0x53);
    tracker.analyze(image.data(), image.stride(), output);

    expect_rects(output, {{8, 8, 16, 8}}, "horizontal merge");
}

void test_vertical_runs_are_merged() {
    Image image(32, 32);
    DirtyTracker tracker(32, 32, 8);
    std::vector<DirtyRect> output;
    establish_baseline(tracker, image, output);

    image.fill_tile(1, 1, 8, 0x64);
    image.fill_tile(2, 1, 8, 0x75);
    image.fill_tile(1, 2, 8, 0x86);
    image.fill_tile(2, 2, 8, 0x97);
    tracker.analyze(image.data(), image.stride(), output);

    expect_rects(output, {{8, 8, 16, 16}}, "vertical merge");
}

void test_edge_tile_uses_clipped_dimensions() {
    constexpr std::uint32_t width = 19;
    constexpr std::uint32_t height = 18;
    Image image(width, height);
    DirtyTracker tracker(width, height, 8);
    std::vector<DirtyRect> output;
    establish_baseline(tracker, image, output);

    image.fill_tile(2, 2, 8, 0xa8);
    tracker.analyze(image.data(), image.stride(), output);

    expect_rects(output, {{16, 16, 3, 2}}, "edge tile");
}

using TestFunction = std::function<void()>;

int run_test(std::string_view name, const TestFunction& test) {
    try {
        test();
        std::cout << "[PASS] " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "[FAIL] " << name << ": unknown exception\n";
        return 1;
    }
}

} // namespace

int main() {
    int failures = 0;
    failures += run_test("first frame is fully dirty", test_first_frame_is_fully_dirty);
    failures += run_test("unchanged frame is clean", test_unchanged_frame_is_clean);
    failures += run_test("single changed tile", test_single_changed_tile);
    failures += run_test("horizontal tiles are merged", test_horizontal_tiles_are_merged);
    failures += run_test("vertical runs are merged", test_vertical_runs_are_merged);
    failures += run_test("edge tile dimensions are clipped", test_edge_tile_uses_clipped_dimensions);
    return failures == 0 ? 0 : 1;
}
