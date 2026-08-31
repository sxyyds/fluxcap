#include <fluxcap/gpu.hpp>

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
namespace gpu = fluxcap::gpu;

struct Options final {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t operations = 2'000;
    std::uint32_t encode_operations = 1'000;
    std::uint32_t warmup = 100;
    std::uint32_t queue_depth = 8;
    std::uint32_t media_fps = 240;
    std::uint32_t paced_fps = 1'000;
    bool show_help = false;
};

struct PacketSink final {
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> bytes{0};
};

std::uint32_t parse_number(std::wstring_view text, const char* option) {
    if (text.empty()) throw std::invalid_argument(std::string(option) + " requires a value");
    std::uint64_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            throw std::invalid_argument(std::string(option) + " must be an integer");
        }
        value = value * 10u + static_cast<std::uint64_t>(character - L'0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument(std::string(option) + " is too large");
        }
    }
    return static_cast<std::uint32_t>(value);
}

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.show_help = true;
            continue;
        }
        const std::size_t separator = argument.find(L'=');
        if (separator == std::wstring_view::npos) {
            throw std::invalid_argument("options must use --name=value syntax");
        }
        const auto name = argument.substr(0, separator);
        const auto value = argument.substr(separator + 1);
        if (name == L"--width") options.width = parse_number(value, "--width");
        else if (name == L"--height") options.height = parse_number(value, "--height");
        else if (name == L"--operations") options.operations = parse_number(value, "--operations");
        else if (name == L"--encode-operations") {
            options.encode_operations = parse_number(value, "--encode-operations");
        }
        else if (name == L"--warmup") options.warmup = parse_number(value, "--warmup");
        else if (name == L"--queue") options.queue_depth = parse_number(value, "--queue");
        else if (name == L"--media-fps") options.media_fps = parse_number(value, "--media-fps");
        else if (name == L"--paced-fps") options.paced_fps = parse_number(value, "--paced-fps");
        else throw std::invalid_argument("unknown benchmark option");
    }
    if (options.width == 0 || options.height == 0
        || (options.width & 1u) != 0 || (options.height & 1u) != 0
        || options.operations == 0 || options.encode_operations == 0
        || options.media_fps == 0 || options.paced_fps == 0
        || options.queue_depth < 2 || options.queue_depth > 32) {
        throw std::invalid_argument(
            "width/height must be positive even values, operations/fps positive, queue 2..32");
    }
    return options;
}

double seconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double>(duration).count();
}

void collect_packet(void* context, const gpu::EncodedPacket& packet) {
    auto& sink = *static_cast<PacketSink*>(context);
    sink.packets.fetch_add(1, std::memory_order_relaxed);
    sink.bytes.fetch_add(packet.size, std::memory_order_relaxed);
}

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void wait_for_gpu(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_QUERY_DESC description{};
    description.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> query;
    HRESULT hr = device->CreateQuery(&description, &query);
    if (FAILED(hr)) fail("D3D11 event query creation failed: " + std::to_string(hr));
    context->End(query.Get());
    context->Flush();
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    for (;;) {
        BOOL completed = FALSE;
        hr = context->GetData(
            query.Get(), &completed, sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && completed != FALSE) return;
        if (FAILED(hr)) fail("D3D11 event query failed: " + std::to_string(hr));
        if (Clock::now() >= deadline) fail("GPU completion query timed out");
        std::this_thread::yield();
    }
}

ComPtr<ID3D11Texture2D> create_source(
    ID3D11Device* device, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t blue = (x * 255u) / width;
            const std::uint32_t green = (y * 255u) / height;
            const std::uint32_t red = ((x ^ y) & 255u);
            pixels[static_cast<std::size_t>(y) * width + x] =
                blue | (green << 8) | (red << 16) | 0xff000000u;
        }
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = width * sizeof(std::uint32_t);
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, &data, &texture);
    if (FAILED(hr)) fail("source texture creation failed: " + std::to_string(hr));
    return texture;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.show_help) {
            std::cout
                << "Usage: fluxcap_gpu_throughput_bench [--width=N] [--height=N]\n"
                   "       [--operations=N] [--encode-operations=N] [--warmup=N]\n"
                   "       [--queue=2..32] [--media-fps=N] [--paced-fps=N]\n";
            return 0;
        }

        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL created_level{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device,
            &created_level,
            &context);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                levels + 1,
                1,
                D3D11_SDK_VERSION,
                &device,
                &created_level,
                &context);
        }
        if (FAILED(hr)) fail("D3D11CreateDevice failed: " + std::to_string(hr));
        auto source = create_source(device.Get(), options.width, options.height);

        gpu::GpuTransformConfig transform_config;
        transform_config.input_width = options.width;
        transform_config.input_height = options.height;
        transform_config.output_width = options.width;
        transform_config.output_height = options.height;
        transform_config.output_format = gpu::GpuPixelFormat::nv12;
        transform_config.frame_rate_numerator = options.media_fps;
        gpu::GpuTransform transform;
        const auto transform_created = gpu::GpuTransform::create(
            device.Get(), transform_config, transform);
        if (!transform_created) {
            fail(std::string("GPU transform create failed: ") + transform_created.what());
        }
        for (std::uint32_t index = 0; index < options.warmup; ++index) {
            const auto transformed = transform.process(source.Get());
            if (!transformed) fail(std::string("transform warmup failed: ") + transformed.what());
        }
        wait_for_gpu(device.Get(), context.Get());

        const auto transform_begin = Clock::now();
        for (std::uint32_t index = 0; index < options.operations; ++index) {
            const auto transformed = transform.process(source.Get());
            if (!transformed) fail(std::string("transform failed: ") + transformed.what());
        }
        const auto transform_submitted = Clock::now();
        wait_for_gpu(device.Get(), context.Get());
        const auto transform_completed = Clock::now();
        const double transform_submit_seconds = seconds(transform_submitted - transform_begin);
        const double transform_complete_seconds = seconds(transform_completed - transform_begin);

        gpu::AsyncGpuPipelineConfig pipeline_config;
        pipeline_config.transform = transform_config;
        pipeline_config.encoder.codec = gpu::VideoCodec::h264;
        pipeline_config.encoder.width = options.width;
        pipeline_config.encoder.height = options.height;
        pipeline_config.encoder.frame_rate_numerator = options.media_fps;
        pipeline_config.encoder.frame_rate_denominator = 1;
        pipeline_config.encoder.bitrate = 8'000'000;
        pipeline_config.encoder.gop_size = options.media_fps;
        pipeline_config.encoder.input_format = DXGI_FORMAT_NV12;
        pipeline_config.encoder.low_latency = true;
        pipeline_config.encoder.input_pool_size = std::max(4u, options.queue_depth);
        pipeline_config.queue_depth = options.queue_depth;

        PacketSink direct_packets;
        gpu::GpuEncoder direct_encoder;
        const auto direct_initialized = direct_encoder.initialize(
            device.Get(),
            pipeline_config.encoder,
            &collect_packet,
            &direct_packets);
        if (!direct_initialized) {
            fail("direct hardware encoder initialize failed: " + direct_initialized.message);
        }
        const std::int64_t frame_duration = 10'000'000LL / options.media_fps;
        const auto direct_begin = Clock::now();
        for (std::uint32_t index = 0; index < options.encode_operations; ++index) {
            const auto encoded = direct_encoder.encode_texture(
                transform.output_texture(),
                static_cast<std::int64_t>(index) * frame_duration,
                frame_duration,
                index == 0);
            if (!encoded) fail("direct hardware encode failed: " + encoded.message);
        }
        const auto direct_inputs_done = Clock::now();
        const auto direct_drained = direct_encoder.drain();
        const auto direct_done = Clock::now();
        if (!direct_drained) fail("direct hardware encoder drain failed: " + direct_drained.message);
        direct_encoder.close();
        const double direct_input_seconds = seconds(direct_inputs_done - direct_begin);
        const double direct_total_seconds = seconds(direct_done - direct_begin);

        PacketSink paced_packets;
        gpu::AsyncGpuPipeline paced_pipeline;
        const auto paced_created = gpu::AsyncGpuPipeline::create(
            device.Get(), pipeline_config, &collect_packet, &paced_packets, paced_pipeline);
        if (!paced_created) {
            fail("paced asynchronous pipeline create failed: " + paced_created.message);
        }
        const auto paced_begin = Clock::now();
        for (std::uint32_t index = 0; index < options.encode_operations; ++index) {
            const auto target = paced_begin + std::chrono::nanoseconds(
                static_cast<std::int64_t>(index) * 1'000'000'000LL
                / options.paced_fps);
            while (Clock::now() < target) YieldProcessor();
            const auto submitted = paced_pipeline.submit_texture(
                source.Get(),
                static_cast<std::int64_t>(index) * frame_duration,
                frame_duration,
                index == 0);
            if (!submitted) fail("paced pipeline submit failed: " + submitted.message);
        }
        const auto paced_submitted = Clock::now();
        const auto paced_drained = paced_pipeline.drain(60'000);
        const auto paced_done = Clock::now();
        if (!paced_drained) fail("paced pipeline drain failed: " + paced_drained.message);
        const auto paced_stats = paced_pipeline.stats();
        const double paced_submit_seconds = seconds(paced_submitted - paced_begin);
        const double paced_total_seconds = seconds(paced_done - paced_begin);

        PacketSink packets;
        gpu::AsyncGpuPipeline pipeline;
        const auto pipeline_created = gpu::AsyncGpuPipeline::create(
            device.Get(), pipeline_config, &collect_packet, &packets, pipeline);
        if (!pipeline_created) {
            fail("asynchronous pipeline create failed: " + pipeline_created.message);
        }
        const auto submit_begin = Clock::now();
        for (std::uint32_t index = 0; index < options.operations; ++index) {
            const auto submitted = pipeline.submit_texture(
                source.Get(),
                static_cast<std::int64_t>(index) * frame_duration,
                frame_duration,
                index == 0);
            if (!submitted) fail("pipeline submit failed: " + submitted.message);
        }
        const auto submit_end = Clock::now();
        const auto drained = pipeline.drain(60'000);
        const auto pipeline_end = Clock::now();
        if (!drained) fail("pipeline drain failed: " + drained.message);
        const auto stats = pipeline.stats();
        const double submit_seconds = seconds(submit_end - submit_begin);
        const double pipeline_seconds = seconds(pipeline_end - submit_begin);

        std::cout << std::fixed << std::setprecision(2)
                  << "FluxCap synthetic GPU saturation benchmark\n"
                  << "Input: " << options.width << 'x' << options.height
                  << " BGRA, operations=" << options.operations
                  << ", encode-operations=" << options.encode_operations
                  << ", media-fps=" << options.media_fps
                  << ", paced-fps=" << options.paced_fps
                  << ", queue=" << options.queue_depth << "\n"
                  << "Synthetic source reuses one immutable texture; these are GPU operations, "
                     "not unique captured frames.\n\n"
                  << "transform CPU submit: "
                  << (options.operations / transform_submit_seconds) << " ops/s\n"
                  << "transform GPU batch complete: "
                  << (options.operations / transform_complete_seconds) << " ops/s\n"
                  << "encoder accepted inputs: "
                  << (options.encode_operations / direct_input_seconds) << " frames/s\n"
                  << "encoder completed packets: "
                  << (direct_packets.packets.load(std::memory_order_relaxed)
                      / direct_total_seconds)
                  << " packets/s\n"
                  << "paced async accepted: "
                  << (paced_stats.accepted_frames / paced_submit_seconds) << " frames/s\n"
                  << "paced async processed: "
                  << (paced_stats.processed_frames / paced_total_seconds) << " frames/s"
                  << " (overwritten=" << paced_stats.overwritten_frames << ")\n"
                  << "async API accepted submit: "
                  << (stats.accepted_frames / submit_seconds) << " ops/s\n"
                  << "pipeline processed: "
                  << (stats.processed_frames / pipeline_seconds) << " frames/s\n"
                  << "encoded packets: "
                  << (packets.packets.load(std::memory_order_relaxed) / pipeline_seconds)
                  << " packets/s\n\n"
                  << "accepted=" << stats.accepted_frames
                  << ", processed=" << stats.processed_frames
                  << ", overwritten=" << stats.overwritten_frames
                  << ", rejected=" << stats.rejected_frames
                  << ", packets=" << packets.packets.load(std::memory_order_relaxed)
                  << ", bytes=" << packets.bytes.load(std::memory_order_relaxed)
                  << ", max-ready=" << stats.maximum_ready_depth << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU throughput benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
