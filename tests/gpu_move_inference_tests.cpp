#include "gpu_move_inference.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
namespace gpu = fluxcap::gpu;
namespace internal = fluxcap::gpu::internal;

constexpr std::uint32_t width = 128;
constexpr std::uint32_t height = 96;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

struct Device final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

Device create_device() {
    Device output;
    constexpr std::array<D3D_FEATURE_LEVEL, 2> levels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL created{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels.data(),
        static_cast<UINT>(levels.size()),
        D3D11_SDK_VERSION,
        &output.device,
        &created,
        &output.context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels.data(),
            static_cast<UINT>(levels.size()),
            D3D11_SDK_VERSION,
            &output.device,
            &created,
            &output.context);
    }
    if (FAILED(hr)) fail("D3D11 device creation failed: " + std::to_string(hr));
    return output;
}

std::vector<std::uint32_t> make_pattern() {
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t value = x * 0x9e3779b9u ^ y * 0x85ebca6bu;
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            pixels[static_cast<std::size_t>(y) * width + x] =
                0xff000000u | (value & 0x00ffffffu);
        }
    }
    return pixels;
}

void copy_region(
    const std::vector<std::uint32_t>& source,
    std::vector<std::uint32_t>& destination,
    std::uint32_t source_x,
    std::uint32_t source_y,
    std::uint32_t destination_x,
    std::uint32_t destination_y,
    std::uint32_t copy_width,
    std::uint32_t copy_height) {
    for (std::uint32_t y = 0; y < copy_height; ++y) {
        std::memcpy(
            destination.data()
                + static_cast<std::size_t>(destination_y + y) * width
                + destination_x,
            source.data()
                + static_cast<std::size_t>(source_y + y) * width
                + source_x,
            static_cast<std::size_t>(copy_width) * sizeof(std::uint32_t));
    }
}

void fill_region(
    std::vector<std::uint32_t>& pixels,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t fill_width,
    std::uint32_t fill_height,
    std::uint32_t value) {
    for (std::uint32_t row = 0; row < fill_height; ++row) {
        std::fill_n(
            pixels.begin()
                + static_cast<std::ptrdiff_t>(
                    static_cast<std::size_t>(y + row) * width + x),
            fill_width,
            value);
    }
}

ComPtr<ID3D11Texture2D> make_texture(
    ID3D11Device* device,
    const std::vector<std::uint32_t>& pixels,
    UINT bind_flags = D3D11_BIND_SHADER_RESOURCE,
    std::uint32_t texture_width = width,
    std::uint32_t texture_height = height) {
    if (pixels.size()
        != static_cast<std::size_t>(texture_width) * texture_height) {
        fail("test texture pixel dimensions do not match");
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = texture_width;
    description.Height = texture_height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bind_flags;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = texture_width * sizeof(std::uint32_t);
    ComPtr<ID3D11Texture2D> output;
    const HRESULT hr = device->CreateTexture2D(
        &description, &initial, &output);
    if (FAILED(hr)) fail("test texture creation failed: " + std::to_string(hr));
    return output;
}

internal::GpuMoveInference create_engine(
    ID3D11Device* device,
    std::uint32_t max_candidates = 64) {
    internal::GpuMoveInferenceConfig config;
    config.width = width;
    config.height = height;
    config.max_candidate_tiles = max_candidates;
    config.readback_slots = 3;
    config.minimum_group_tiles = 2;
    internal::GpuMoveInference output;
    const gpu::GpuError result = internal::GpuMoveInference::create(
        device, config, output);
    if (!result) fail(std::string("move engine creation failed: ") + result.what());
    return output;
}

internal::GpuMoveInferenceResult wait_for_result(
    internal::GpuMoveInference& engine,
    ID3D11DeviceContext* context) {
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        internal::GpuMoveInferenceResult output;
        bool ready = false;
        const gpu::GpuError result = engine.try_resolve(output, ready);
        if (!result) fail(std::string("move result failed: ") + result.what());
        if (ready) return output;
        std::this_thread::yield();
    }
    fail("timed out waiting for GPU move result");
}

void wait_for_no_pending(
    internal::GpuMoveInference& engine,
    ID3D11DeviceContext* context) {
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.pending_count() == 0) return;
        internal::GpuMoveInferenceResult output;
        bool ready = false;
        const gpu::GpuError result = engine.try_resolve(output, ready);
        if (!result) fail(std::string("move drain failed: ") + result.what());
        if (ready) fail("unexpected finalized result while draining cancellation");
        std::this_thread::yield();
    }
    fail("timed out draining canceled GPU move result");
}

void commit_submission(
    internal::GpuMoveInference& engine,
    const internal::GpuMoveInferenceSubmitInfo& info) {
    if (!engine.commit_submission(info)) {
        fail("GPU move-inference submission could not be finalized");
    }
}

void submit_baseline(
    internal::GpuMoveInference& engine,
    ID3D11Texture2D* texture,
    std::uint64_t sequence) {
    const gpu::WgcRect full{0, 0, width, height};
    internal::GpuMoveInferenceSubmitInfo info;
    const gpu::GpuError result = engine.submit(
        texture, sequence, std::span(&full, 1), info);
    if (!result) fail(std::string("baseline submission failed: ") + result.what());
    if ((info.flags & internal::gpu_move_submit_baseline) == 0
        || (info.flags & internal::gpu_move_submit_scheduled) != 0) {
        fail("first frame was not treated as a move-inference baseline");
    }
    commit_submission(engine, info);
}

void test_grouped_unique_move(const Device& device) {
    constexpr std::uint32_t source_x = 16;
    constexpr std::uint32_t source_y = 16;
    constexpr std::uint32_t destination_x = 80;
    constexpr std::uint32_t destination_y = 48;
    constexpr std::uint32_t move_width = 32;
    constexpr std::uint32_t move_height = 32;

    const std::vector<std::uint32_t> baseline_pixels = make_pattern();
    std::vector<std::uint32_t> moved_pixels = baseline_pixels;
    copy_region(
        baseline_pixels,
        moved_pixels,
        source_x,
        source_y,
        destination_x,
        destination_y,
        move_width,
        move_height);
    fill_region(
        moved_pixels,
        source_x,
        source_y,
        move_width,
        move_height,
        0xff010203u);

    const auto baseline = make_texture(device.device.Get(), baseline_pixels);
    const auto moved = make_texture(device.device.Get(), moved_pixels);
    auto engine = create_engine(device.device.Get());
    submit_baseline(engine, baseline.Get(), 10);

    const std::array<gpu::WgcRect, 2> dirty{{
        {static_cast<std::int32_t>(source_x),
         static_cast<std::int32_t>(source_y), move_width, move_height},
        {static_cast<std::int32_t>(destination_x),
         static_cast<std::int32_t>(destination_y), move_width, move_height}}};
    internal::GpuMoveInferenceSubmitInfo info;
    const gpu::GpuError submitted = engine.submit(
        moved.Get(), 11, dirty, info);
    if (!submitted) fail(std::string("move submission failed: ") + submitted.what());
    if ((info.flags & internal::gpu_move_submit_scheduled) == 0
        || info.candidate_count != 8
        || info.base_sequence != 10) {
        fail("unique move submission did not schedule the expected candidates");
    }
    commit_submission(engine, info);

    const internal::GpuMoveInferenceResult output = wait_for_result(
        engine, device.context.Get());
    if (output.sequence != 11
        || output.base_sequence != 10
        || output.move_count != 1
        || (output.flags & internal::gpu_move_result_inferred) == 0
        || (output.flags & internal::gpu_move_result_dirty_preserved) == 0) {
        fail("unique move result has an invalid frame contract");
    }
    const auto& move = output.moves[0];
    if (move.rectangle.source_x != source_x
        || move.rectangle.source_y != source_y
        || move.rectangle.destination.x != destination_x
        || move.rectangle.destination.y != destination_y
        || move.rectangle.destination.width != move_width
        || move.rectangle.destination.height != move_height
        || move.tile_count != 4
        || move.confidence
            != internal::GpuMoveConfidence::dual_hash_unique_grouped
        || (move.evidence_flags
            & internal::gpu_move_evidence_unique_previous) == 0
        || (move.evidence_flags
            & internal::gpu_move_evidence_dirty_preserved) == 0) {
        fail("unique tile matches were not grouped into the expected move");
    }
}

void test_duplicate_source_fails_closed(const Device& device) {
    constexpr std::uint32_t source_x = 16;
    constexpr std::uint32_t source_y = 16;
    constexpr std::uint32_t duplicate_x = 48;
    constexpr std::uint32_t duplicate_y = 16;
    constexpr std::uint32_t destination_x = 80;
    constexpr std::uint32_t destination_y = 48;
    constexpr std::uint32_t move_width = 32;
    constexpr std::uint32_t move_height = 32;

    std::vector<std::uint32_t> baseline_pixels = make_pattern();
    const std::vector<std::uint32_t> unique_source = baseline_pixels;
    copy_region(
        unique_source,
        baseline_pixels,
        source_x,
        source_y,
        duplicate_x,
        duplicate_y,
        move_width,
        move_height);
    std::vector<std::uint32_t> moved_pixels = baseline_pixels;
    copy_region(
        unique_source,
        moved_pixels,
        source_x,
        source_y,
        destination_x,
        destination_y,
        move_width,
        move_height);
    fill_region(
        moved_pixels,
        source_x,
        source_y,
        move_width,
        move_height,
        0xff040506u);

    const auto baseline = make_texture(device.device.Get(), baseline_pixels);
    const auto moved = make_texture(device.device.Get(), moved_pixels);
    auto engine = create_engine(device.device.Get());
    submit_baseline(engine, baseline.Get(), 20);
    const gpu::WgcRect dirty{
        static_cast<std::int32_t>(destination_x),
        static_cast<std::int32_t>(destination_y),
        move_width,
        move_height};
    internal::GpuMoveInferenceSubmitInfo info;
    const gpu::GpuError submitted = engine.submit(
        moved.Get(), 21, std::span(&dirty, 1), info);
    if (!submitted || (info.flags & internal::gpu_move_submit_scheduled) == 0) {
        fail("duplicate-source test was not scheduled");
    }
    commit_submission(engine, info);
    const internal::GpuMoveInferenceResult output = wait_for_result(
        engine, device.context.Get());
    if (output.move_count != 0
        || (output.flags & internal::gpu_move_result_inferred) != 0) {
        fail("non-unique previous hashes were incorrectly classified as moves");
    }
}

void test_candidate_overflow_fails_closed(const Device& device) {
    const std::vector<std::uint32_t> baseline_pixels = make_pattern();
    std::vector<std::uint32_t> changed_pixels = baseline_pixels;
    fill_region(changed_pixels, 16, 16, 32, 32, 0xff070809u);
    const auto baseline = make_texture(device.device.Get(), baseline_pixels);
    const auto changed = make_texture(device.device.Get(), changed_pixels);
    auto engine = create_engine(device.device.Get(), 2);
    submit_baseline(engine, baseline.Get(), 30);
    const gpu::WgcRect dirty{16, 16, 32, 32};
    internal::GpuMoveInferenceSubmitInfo info;
    const gpu::GpuError submitted = engine.submit(
        changed.Get(), 31, std::span(&dirty, 1), info);
    if (!submitted
        || (info.flags & internal::gpu_move_submit_fail_closed) == 0
        || (info.flags & internal::gpu_move_submit_candidate_overflow) == 0
        || (info.flags & internal::gpu_move_submit_scheduled) != 0) {
        fail("candidate overflow did not fail closed");
    }
    commit_submission(engine, info);
    device.context->Flush();
    internal::GpuMoveInferenceResult output;
    bool ready = true;
    const gpu::GpuError resolved = engine.try_resolve(output, ready);
    if (!resolved || ready) {
        fail("overflow frame unexpectedly produced inferred move metadata");
    }
}

void test_reset_discards_old_epoch(const Device& device) {
    const auto pixels = make_pattern();
    const auto baseline = make_texture(device.device.Get(), pixels);
    auto engine = create_engine(device.device.Get());
    submit_baseline(engine, baseline.Get(), 40);
    const gpu::WgcRect dirty{0, 0, 32, 32};
    internal::GpuMoveInferenceSubmitInfo info;
    const gpu::GpuError submitted = engine.submit(
        baseline.Get(), 41, std::span(&dirty, 1), info);
    if (!submitted || (info.flags & internal::gpu_move_submit_scheduled) == 0) {
        fail("old-epoch result was not scheduled");
    }
    commit_submission(engine, info);
    const std::uint64_t old_epoch = engine.epoch();
    engine.reset_history();
    if (engine.epoch() == old_epoch) fail("move-inference reset did not advance epoch");
    submit_baseline(engine, baseline.Get(), 1);
    internal::GpuMoveInferenceSubmitInfo new_info;
    const gpu::GpuError new_submitted = engine.submit(
        baseline.Get(), 2, std::span(&dirty, 1), new_info);
    if (!new_submitted
        || (new_info.flags & internal::gpu_move_submit_scheduled) == 0) {
        fail("new-epoch result was not scheduled");
    }
    commit_submission(engine, new_info);
    const internal::GpuMoveInferenceResult new_result = wait_for_result(
        engine, device.context.Get());
    if (new_result.epoch == old_epoch
        || new_result.epoch != engine.epoch()
        || new_result.sequence != 2
        || new_result.base_sequence != 1) {
        fail("reset exposed stale metadata or lost the new epoch result");
    }
}

void test_baseline_reset_preserves_finalized_pending(const Device& device) {
    const auto pixels = make_pattern();
    const auto texture = make_texture(device.device.Get(), pixels);
    auto engine = create_engine(device.device.Get());
    submit_baseline(engine, texture.Get(), 60);
    const gpu::WgcRect dirty{0, 0, 32, 32};

    internal::GpuMoveInferenceSubmitInfo finalized;
    const gpu::GpuError submitted = engine.submit(
        texture.Get(), 61, std::span(&dirty, 1), finalized);
    if (!submitted
        || (finalized.flags & internal::gpu_move_submit_scheduled) == 0) {
        fail("finalized pre-discontinuity result was not scheduled");
    }
    commit_submission(engine, finalized);

    const std::uint64_t epoch = engine.epoch();
    engine.reset_baseline();
    if (engine.epoch() != epoch || engine.pending_count() != 1) {
        fail("baseline reset invalidated a finalized pending result");
    }

    internal::GpuMoveInferenceSubmitInfo canceled_baseline;
    const gpu::GpuError baseline_submitted = engine.submit(
        texture.Get(), 62, std::span(&dirty, 1), canceled_baseline);
    if (!baseline_submitted
        || (canceled_baseline.flags & internal::gpu_move_submit_baseline) == 0) {
        fail("post-discontinuity frame was not treated as a fresh baseline");
    }
    auto wrong_submission = canceled_baseline;
    ++wrong_submission.sequence;
    if (engine.cancel_submission(wrong_submission)) {
        fail("move inference canceled a non-matching submission");
    }
    if (!engine.cancel_submission(canceled_baseline)) {
        fail("move inference could not cancel the exact baseline submission");
    }

    const internal::GpuMoveInferenceResult result = wait_for_result(
        engine, device.context.Get());
    if (result.epoch != epoch
        || result.sequence != 61
        || result.base_sequence != 60) {
        fail("discontinuity or cancellation discarded the finalized result");
    }

    internal::GpuMoveInferenceSubmitInfo rebuilt;
    const gpu::GpuError rebuilt_submit = engine.submit(
        texture.Get(), 63, std::span(&dirty, 1), rebuilt);
    if (!rebuilt_submit
        || (rebuilt.flags & internal::gpu_move_submit_baseline) == 0) {
        fail("canceled discontinuity frame was retained as a hash baseline");
    }
    commit_submission(engine, rebuilt);
}

void test_cancel_current_preserves_pending_and_hash_baseline(
    const Device& device) {
    const auto pixels = make_pattern();
    const auto texture = make_texture(device.device.Get(), pixels);
    auto engine = create_engine(device.device.Get());
    submit_baseline(engine, texture.Get(), 70);
    const gpu::WgcRect dirty{0, 0, 32, 32};

    internal::GpuMoveInferenceSubmitInfo finalized;
    const gpu::GpuError finalized_submit = engine.submit(
        texture.Get(), 71, std::span(&dirty, 1), finalized);
    if (!finalized_submit
        || (finalized.flags & internal::gpu_move_submit_scheduled) == 0) {
        fail("older finalized result was not scheduled");
    }
    commit_submission(engine, finalized);

    internal::GpuMoveInferenceSubmitInfo canceled;
    const gpu::GpuError canceled_submit = engine.submit(
        texture.Get(), 72, std::span(&dirty, 1), canceled);
    if (!canceled_submit
        || (canceled.flags & internal::gpu_move_submit_scheduled) == 0) {
        fail("current provisional result was not scheduled");
    }
    auto wrong_token = canceled;
    ++wrong_token.submission_token;
    if (engine.cancel_submission(wrong_token)) {
        fail("move inference accepted a stale cancellation token");
    }
    if (!engine.cancel_submission(canceled)) {
        fail("move inference could not cancel the current submission");
    }
    if (engine.pending_count() != 2) {
        fail("canceling the current submit removed unrelated GPU work");
    }

    const internal::GpuMoveInferenceResult older = wait_for_result(
        engine, device.context.Get());
    if (older.sequence != 71 || older.base_sequence != 70) {
        fail("current cancellation discarded or rewrote the older result");
    }
    wait_for_no_pending(engine, device.context.Get());

    for (std::uint64_t sequence = 80; sequence < 88; ++sequence) {
        internal::GpuMoveInferenceSubmitInfo repeated_cancel;
        const gpu::GpuError repeated_submit = engine.submit(
            texture.Get(), sequence, std::span(&dirty, 1), repeated_cancel);
        if (!repeated_submit
            || (repeated_cancel.flags
                & internal::gpu_move_submit_scheduled) == 0
            || (repeated_cancel.flags
                & internal::gpu_move_submit_no_readback_slot) != 0) {
            fail("repeated cancellation exhausted move readback slots");
        }
        if (!engine.cancel_submission(repeated_cancel)) {
            fail("repeated move submission could not be canceled");
        }
        wait_for_no_pending(engine, device.context.Get());
    }

    internal::GpuMoveInferenceSubmitInfo next;
    const gpu::GpuError next_submit = engine.submit(
        texture.Get(), 90, std::span(&dirty, 1), next);
    if (!next_submit
        || (next.flags & internal::gpu_move_submit_scheduled) == 0
        || next.base_sequence != 71) {
        fail("current cancellation did not restore the finalized hash baseline");
    }
    commit_submission(engine, next);
    const internal::GpuMoveInferenceResult next_result = wait_for_result(
        engine, device.context.Get());
    if (next_result.sequence != 90 || next_result.base_sequence != 71) {
        fail("restored hash baseline was not used by the next result");
    }
}

void test_source_reconfiguration_preserves_finalized_results(
    const Device& device) {
    constexpr std::uint32_t old_texture_width = 192;
    constexpr std::uint32_t old_texture_height = 128;
    constexpr std::uint32_t old_region_x = 32;
    constexpr std::uint32_t old_region_y = 16;
    constexpr std::uint32_t new_texture_width = 224;
    constexpr std::uint32_t new_texture_height = 160;
    constexpr std::uint32_t new_region_x = 64;
    constexpr std::uint32_t new_region_y = 32;

    const auto make_source_pixels = [](
        std::uint32_t texture_width,
        std::uint32_t texture_height,
        std::uint32_t region_x,
        std::uint32_t region_y,
        std::uint32_t background) {
        const auto local = make_pattern();
        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(texture_width) * texture_height,
            background);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::copy_n(
                local.data() + static_cast<std::size_t>(y) * width,
                width,
                pixels.data()
                    + static_cast<std::size_t>(region_y + y) * texture_width
                    + region_x);
        }
        return pixels;
    };

    const auto old_texture = make_texture(
        device.device.Get(),
        make_source_pixels(
            old_texture_width,
            old_texture_height,
            old_region_x,
            old_region_y,
            0xff101010u),
        D3D11_BIND_SHADER_RESOURCE,
        old_texture_width,
        old_texture_height);
    const auto new_texture = make_texture(
        device.device.Get(),
        make_source_pixels(
            new_texture_width,
            new_texture_height,
            new_region_x,
            new_region_y,
            0xffe0e0e0u),
        D3D11_BIND_SHADER_RESOURCE,
        new_texture_width,
        new_texture_height);

    internal::GpuMoveInferenceConfig config;
    config.width = width;
    config.height = height;
    config.texture_width = old_texture_width;
    config.texture_height = old_texture_height;
    config.region_x = old_region_x;
    config.region_y = old_region_y;
    config.max_candidate_tiles = 64;
    config.readback_slots = 3;
    config.minimum_group_tiles = 2;
    internal::GpuMoveInference engine;
    const gpu::GpuError created = internal::GpuMoveInference::create(
        device.device.Get(), config, engine);
    if (!created) {
        fail(std::string("source-reconfiguration engine creation failed: ")
            + created.what());
    }

    submit_baseline(engine, old_texture.Get(), 100);
    const gpu::WgcRect dirty{0, 0, 32, 32};
    internal::GpuMoveInferenceSubmitInfo old_pending;
    const gpu::GpuError old_submitted = engine.submit(
        old_texture.Get(), 101, std::span(&dirty, 1), old_pending);
    if (!old_submitted
        || (old_pending.flags & internal::gpu_move_submit_scheduled) == 0) {
        fail("pre-reconfiguration result was not scheduled");
    }

    const gpu::GpuError provisional_reconfigure = engine.reconfigure_source(
        new_texture_width,
        new_texture_height,
        new_region_x,
        new_region_y);
    const auto provisional_config = engine.config();
    if (provisional_reconfigure.status != gpu::GpuStatus::invalid_argument
        || provisional_config.texture_width != old_texture_width
        || provisional_config.texture_height != old_texture_height
        || provisional_config.region_x != old_region_x
        || provisional_config.region_y != old_region_y
        || engine.pending_count() != 1) {
        fail("source reconfiguration accepted a provisional submission");
    }
    commit_submission(engine, old_pending);

    const std::uint64_t epoch = engine.epoch();
    const gpu::GpuError reconfigured = engine.reconfigure_source(
        new_texture_width,
        new_texture_height,
        new_region_x,
        new_region_y);
    const auto active_config = engine.config();
    if (!reconfigured
        || engine.epoch() != epoch
        || engine.pending_count() != 1
        || active_config.texture_width != new_texture_width
        || active_config.texture_height != new_texture_height
        || active_config.region_x != new_region_x
        || active_config.region_y != new_region_y) {
        fail("source reconfiguration replaced history or lost its new source");
    }

    internal::GpuMoveInferenceSubmitInfo stale_sequence;
    const gpu::GpuError stale_submitted = engine.submit(
        new_texture.Get(), 101, std::span(&dirty, 1), stale_sequence);
    if (stale_submitted
        || stale_submitted.status != gpu::GpuStatus::invalid_argument) {
        fail("source reconfiguration reset the same-epoch sequence high-water mark");
    }

    submit_baseline(engine, new_texture.Get(), 102);
    internal::GpuMoveInferenceSubmitInfo new_pending;
    const gpu::GpuError new_submitted = engine.submit(
        new_texture.Get(), 103, std::span(&dirty, 1), new_pending);
    if (!new_submitted
        || (new_pending.flags & internal::gpu_move_submit_scheduled) == 0
        || new_pending.base_sequence != 102) {
        fail("new source did not establish a fresh committed hash baseline");
    }
    commit_submission(engine, new_pending);

    const internal::GpuMoveInferenceResult old_result = wait_for_result(
        engine, device.context.Get());
    if (old_result.epoch != epoch
        || old_result.sequence != 101
        || old_result.base_sequence != 100) {
        fail("source reconfiguration discarded or rewrote the old result");
    }
    const internal::GpuMoveInferenceResult new_result = wait_for_result(
        engine, device.context.Get());
    if (new_result.epoch != epoch
        || new_result.sequence != 103
        || new_result.base_sequence != 102) {
        fail("source reconfiguration broke result order or sequence lineage");
    }
}

void test_in_place_roi_hashing(const Device& device) {
    constexpr std::uint32_t texture_width = 192;
    constexpr std::uint32_t texture_height = 128;
    constexpr std::uint32_t region_x = 32;
    constexpr std::uint32_t region_y = 16;
    constexpr std::uint32_t source_x = 16;
    constexpr std::uint32_t source_y = 16;
    constexpr std::uint32_t destination_x = 80;
    constexpr std::uint32_t destination_y = 48;
    constexpr std::uint32_t move_width = 32;
    constexpr std::uint32_t move_height = 32;

    const std::vector<std::uint32_t> local_baseline = make_pattern();
    std::vector<std::uint32_t> local_moved = local_baseline;
    copy_region(
        local_baseline,
        local_moved,
        source_x,
        source_y,
        destination_x,
        destination_y,
        move_width,
        move_height);
    fill_region(
        local_moved,
        source_x,
        source_y,
        move_width,
        move_height,
        0xff112233u);

    std::vector<std::uint32_t> baseline_pixels(
        static_cast<std::size_t>(texture_width) * texture_height,
        0xff010101u);
    std::vector<std::uint32_t> moved_pixels(
        static_cast<std::size_t>(texture_width) * texture_height,
        0xfffefefeu);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::copy_n(
            local_baseline.data() + static_cast<std::size_t>(y) * width,
            width,
            baseline_pixels.data()
                + static_cast<std::size_t>(region_y + y) * texture_width
                + region_x);
        std::copy_n(
            local_moved.data() + static_cast<std::size_t>(y) * width,
            width,
            moved_pixels.data()
                + static_cast<std::size_t>(region_y + y) * texture_width
                + region_x);
    }

    const auto baseline = make_texture(
        device.device.Get(),
        baseline_pixels,
        D3D11_BIND_SHADER_RESOURCE,
        texture_width,
        texture_height);
    const auto moved = make_texture(
        device.device.Get(),
        moved_pixels,
        D3D11_BIND_SHADER_RESOURCE,
        texture_width,
        texture_height);

    internal::GpuMoveInferenceConfig config;
    config.width = width;
    config.height = height;
    config.texture_width = texture_width;
    config.texture_height = texture_height;
    config.region_x = region_x;
    config.region_y = region_y;
    config.max_candidate_tiles = 64;
    config.readback_slots = 3;
    config.minimum_group_tiles = 2;
    internal::GpuMoveInference engine;
    const gpu::GpuError created = internal::GpuMoveInference::create(
        device.device.Get(), config, engine);
    if (!created) fail(std::string("ROI move engine creation failed: ") + created.what());
    submit_baseline(engine, baseline.Get(), 50);

    const std::array<gpu::WgcRect, 2> dirty{{
        {static_cast<std::int32_t>(source_x),
         static_cast<std::int32_t>(source_y), move_width, move_height},
        {static_cast<std::int32_t>(destination_x),
         static_cast<std::int32_t>(destination_y), move_width, move_height}}};
    internal::GpuMoveInferenceSubmitInfo info;
    const gpu::GpuError submitted = engine.submit(
        moved.Get(), 51, dirty, info);
    if (!submitted || (info.flags & internal::gpu_move_submit_scheduled) == 0) {
        fail("in-place ROI move submission was not scheduled");
    }
    commit_submission(engine, info);
    const internal::GpuMoveInferenceResult output = wait_for_result(
        engine, device.context.Get());
    if (output.sequence != 51
        || output.base_sequence != 50
        || output.move_count != 1
        || output.moves[0].rectangle.source_x != source_x
        || output.moves[0].rectangle.source_y != source_y
        || output.moves[0].rectangle.destination.x != destination_x
        || output.moves[0].rectangle.destination.y != destination_y
        || output.moves[0].rectangle.destination.width != move_width
        || output.moves[0].rectangle.destination.height != move_height) {
        fail("in-place ROI hashing did not preserve region-local coordinates");
    }

    config.region_x = texture_width - width + 1;
    internal::GpuMoveInference invalid;
    const gpu::GpuError rejected = internal::GpuMoveInference::create(
        device.device.Get(), config, invalid);
    if (rejected.status != gpu::GpuStatus::invalid_argument) {
        fail("out-of-bounds move-inference ROI was accepted");
    }
}

} // namespace

int main() {
    try {
        const Device device = create_device();
        test_grouped_unique_move(device);
        test_duplicate_source_fails_closed(device);
        test_candidate_overflow_fails_closed(device);
        test_reset_discards_old_epoch(device);
        test_baseline_reset_preserves_finalized_pending(device);
        test_cancel_current_preserves_pending_and_hash_baseline(device);
        test_source_reconfiguration_preserves_finalized_results(device);
        test_in_place_roi_hashing(device);
        std::cout << "gpu_move_inference_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "gpu_move_inference_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
