#include <fluxcap/gpu.hpp>

#include <windows.h>
#include <winrt/base.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace gpu = fluxcap::gpu;

struct EncodedStream final {
    std::vector<std::uint8_t> bytes;
    std::size_t packets = 0;
    std::size_t keyframes = 0;
};

void collect_packet(void* context, const gpu::EncodedPacket& packet) {
    auto& stream = *static_cast<EncodedStream*>(context);
    if (packet.data == nullptr || packet.size == 0) {
        return;
    }
    stream.bytes.insert(stream.bytes.end(), packet.data, packet.data + packet.size);
    ++stream.packets;
    stream.keyframes += packet.keyframe ? 1u : 0u;
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

HWND parse_window(const wchar_t* text) {
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != L'\0'
        || value > std::numeric_limits<std::uintptr_t>::max()) {
        fail("invalid HWND; pass a decimal or 0x-prefixed hexadecimal value");
    }
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
}

std::uint32_t parse_frame_count(const wchar_t* text) {
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != L'\0' || value == 0
        || value > 100'000ul) {
        fail("frame-count must be in the range 1..100000");
    }
    return static_cast<std::uint32_t>(value);
}

void require(const gpu::WgcResult& result, const char* operation) {
    if (!result) {
        fail(std::string(operation) + ": " + result.message
            + " (status=" + gpu::wgc_status_string(result.status) + ")");
    }
}

void require(const gpu::GpuError& result, const char* operation) {
    if (!result) {
        fail(std::string(operation) + ": " + result.what()
            + " (hr=" + std::to_string(result.hresult) + ")");
    }
}

void require(const gpu::GpuEncoderResult& result, const char* operation) {
    if (!result) {
        fail(std::string(operation) + ": " + result.message
            + " (status=" + gpu::gpu_encoder_status_string(result.status)
            + ", hr=" + std::to_string(result.hresult) + ")");
    }
}

void require(const gpu::ImageEncoderResult& result, const char* operation) {
    if (!result) {
        fail(std::string(operation) + ": " + result.message
            + " (status=" + gpu::image_encoder_status_string(result.status)
            + ", hr=" + std::to_string(result.hresult) + ")");
    }
}

std::vector<std::uint8_t> encode_image(
    gpu::WicImageEncoder& encoder,
    ID3D11Texture2D* texture,
    gpu::ImageFormat format) {
    gpu::ImageEncodeOptions options;
    options.format = format;
    options.jpeg_quality = 0.92F;
    std::vector<std::uint8_t> output;
    require(
        encoder.encode_texture_to_memory_sync(texture, output, options),
        format == gpu::ImageFormat::png ? "PNG encode" : "JPEG encode");
    return output;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 3) {
        std::cerr << "Usage: fluxcap_gpu_pipeline [HWND] [frame-count]\n"
                     "  HWND accepts decimal or 0x-prefixed hexadecimal input.\n"
                     "  The current foreground window and 120 frames are used by default.\n";
        return 2;
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        HWND window = argc >= 2 ? parse_window(argv[1]) : GetForegroundWindow();
        const std::uint32_t frame_count = argc >= 3 ? parse_frame_count(argv[2]) : 120u;
        if (window == nullptr || !IsWindow(window)) {
            fail("the selected HWND is not a valid window");
        }

        std::cout << "Capturing HWND 0x" << std::hex
                  << reinterpret_cast<std::uintptr_t>(window) << std::dec << '\n';

        gpu::WgcCaptureOptions capture_options;
        capture_options.buffer_count = 3;
        capture_options.include_cursor = true;

        gpu::WgcCapture capture;
        require(
            gpu::WgcCapture::create_for_window(
                window, nullptr, capture_options, capture),
            "WGC create");
        require(capture.start(), "WGC start");

        gpu::WgcFrameLease frame;
        require(capture.acquire_latest(5'000, frame), "first-frame acquire");
        if (frame.info().width < 2 || frame.info().height < 2) {
            fail("captured window is too small for NV12 video");
        }

        gpu::WicImageEncoder image_encoder;
        require(image_encoder.initialize(capture.device()), "image encoder initialize");
        const auto png = encode_image(image_encoder, frame.texture(), gpu::ImageFormat::png);
        const auto jpeg = encode_image(image_encoder, frame.texture(), gpu::ImageFormat::jpeg);
        std::cout << "First frame: " << frame.info().width << 'x' << frame.info().height
                  << ", PNG=" << png.size() << " bytes"
                  << ", JPEG=" << jpeg.size() << " bytes\n";

        gpu::GpuTransformConfig transform_config;
        transform_config.input_width = frame.info().width;
        transform_config.input_height = frame.info().height;
        transform_config.output_width = frame.info().width & ~1u;
        transform_config.output_height = frame.info().height & ~1u;
        transform_config.output_format = gpu::GpuPixelFormat::nv12;
        transform_config.frame_rate_numerator = 60;
        transform_config.frame_rate_denominator = 1;

        gpu::GpuTransform transform;
        require(
            gpu::GpuTransform::create(capture.device(), transform_config, transform),
            "BGRA-to-NV12 transform create");

        gpu::GpuEncoderConfig encoder_config;
        encoder_config.codec = gpu::VideoCodec::h264;
        encoder_config.width = transform_config.output_width;
        encoder_config.height = transform_config.output_height;
        encoder_config.frame_rate_numerator = transform_config.frame_rate_numerator;
        encoder_config.frame_rate_denominator = transform_config.frame_rate_denominator;
        encoder_config.bitrate = 8'000'000;
        encoder_config.gop_size = 120;
        encoder_config.input_format = DXGI_FORMAT_NV12;
        encoder_config.low_latency = true;

        gpu::GpuEncoderSupport support;
        require(gpu::GpuEncoder::probe(capture.device(), encoder_config, support),
                "H.264 encoder probe");
        if (!support.supported || !support.d3d11_aware) {
            fail("no D3D11-aware H.264 hardware encoder is available");
        }

        EncodedStream stream;
        gpu::GpuEncoder encoder;
        require(
            encoder.initialize(
                capture.device(), encoder_config, &collect_packet, &stream),
            "H.264 encoder initialize");

        const std::int64_t frame_duration =
            10'000'000LL * encoder_config.frame_rate_denominator
            / encoder_config.frame_rate_numerator;
        const auto started_at = std::chrono::steady_clock::now();

        for (std::uint32_t index = 0; index < frame_count; ++index) {
            bool has_new_frame = index == 0;
            if (index != 0) {
                const auto presentation_time = started_at + std::chrono::nanoseconds(
                    frame_duration * 100LL * static_cast<std::int64_t>(index));
                std::this_thread::sleep_until(presentation_time);
                frame.reset();
                const auto acquired = capture.acquire_latest(0, frame);
                if (acquired) {
                    has_new_frame = true;
                } else if (acquired.status != gpu::WgcStatus::timeout) {
                    require(acquired, "frame acquire");
                }
            }

            if (has_new_frame) {
                if (frame.info().width != transform_config.input_width
                    || frame.info().height != transform_config.input_height) {
                    fail("window size changed during capture; restart the example");
                }
                require(transform.process(frame.texture()), "BGRA-to-NV12 transform");
            }
            require(
                encoder.encode_texture(
                    transform.output_texture(),
                    static_cast<std::int64_t>(index) * frame_duration,
                    frame_duration,
                    index == 0),
                "H.264 encode");
        }

        require(encoder.drain(), "H.264 drain");
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_at).count();
        if (stream.bytes.empty()) {
            fail("H.264 encoder produced no packets");
        }

        const auto capture_stats = capture.stats();
        capture.stop();
        std::cout << "H.264: " << stream.packets << " packets, "
                  << stream.keyframes << " keyframes, " << stream.bytes.size()
                  << " bytes in memory\n"
                  << "Pipeline: " << frame_count << " frames in " << elapsed
                  << " s (" << (static_cast<double>(frame_count) / elapsed) << " fps)\n"
                  << "WGC: received=" << capture_stats.received_frames
                  << ", published=" << capture_stats.published_frames
                  << ", overwritten=" << capture_stats.overwritten_frames
                  << ", source-dropped=" << capture_stats.dropped_at_source << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU pipeline failed: " << error.what() << '\n';
        return 1;
    }
}
