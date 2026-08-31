#include <fluxcap/gpu.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

namespace gpu = fluxcap::gpu;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require(const gpu::GpuError& error, const char* message) {
    require(static_cast<bool>(error), message);
}

class BitWriter final {
public:
    void bits(std::uint32_t value, std::uint32_t count) {
        for (std::uint32_t index = 0; index < count; ++index) {
            bit(((value >> (count - index - 1u)) & 1u) != 0);
        }
    }

    void bit(bool value) {
        if ((bit_count_ & 7u) == 0) bytes_.push_back(0);
        if (value) {
            bytes_.back() |= static_cast<std::uint8_t>(
                1u << (7u - (bit_count_ & 7u)));
        }
        ++bit_count_;
    }

    void ue(std::uint32_t value) {
        const std::uint64_t code = static_cast<std::uint64_t>(value) + 1u;
        std::uint32_t width = 0;
        for (std::uint64_t copy = code; copy != 0; copy >>= 1u) ++width;
        for (std::uint32_t index = 1; index < width; ++index) bit(false);
        bits(static_cast<std::uint32_t>(code), width);
    }

    std::vector<std::uint8_t> finish() {
        bit(true);
        while ((bit_count_ & 7u) != 0) bit(false);
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint32_t bit_count_ = 0;
};

std::vector<std::uint8_t> escape_rbsp(
    const std::vector<std::uint8_t>& input) {
    std::vector<std::uint8_t> output;
    std::uint32_t zeros = 0;
    for (const std::uint8_t byte : input) {
        if (zeros >= 2 && byte <= 3) {
            output.push_back(3);
            zeros = 0;
        }
        output.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return output;
}

void append_start_code(std::vector<std::uint8_t>& output) {
    output.insert(output.end(), {0, 0, 0, 1});
}

void append_be16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8u));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_be32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24u));
    output.push_back(static_cast<std::uint8_t>(value >> 16u));
    output.push_back(static_cast<std::uint8_t>(value >> 8u));
    output.push_back(static_cast<std::uint8_t>(value));
}

std::vector<std::uint8_t> hevc_sps_packet() {
    BitWriter writer;
    writer.bits(0, 4);  // VPS id
    writer.bits(0, 3);  // max sub-layers minus one
    writer.bit(true);
    writer.bits(0, 32); // profile-tier-level
    writer.bits(0, 32);
    writer.bits(0, 32);
    writer.ue(0);       // SPS id
    writer.ue(1);       // 4:2:0
    writer.ue(64);
    writer.ue(64);
    writer.bit(false);  // conformance window
    writer.ue(2);       // 10-bit luma
    writer.ue(2);       // 10-bit chroma
    writer.ue(0);       // log2 max POC minus four
    writer.bit(true);   // ordering info for all layers
    writer.ue(0);
    writer.ue(0);
    writer.ue(0);
    for (std::uint32_t index = 0; index < 6; ++index) writer.ue(0);
    writer.bit(false);  // scaling lists
    writer.bit(false);  // AMP
    writer.bit(false);  // SAO
    writer.bit(false);  // PCM
    writer.ue(0);       // short-term reference sets
    writer.bit(false);  // long-term references
    writer.bit(false);  // temporal MVP
    writer.bit(false);  // strong intra smoothing
    writer.bit(true);   // VUI
    writer.bit(false);  // aspect ratio
    writer.bit(false);  // overscan
    writer.bit(true);   // video signal type
    writer.bits(5, 3);  // unspecified video format
    writer.bit(false);  // studio range
    writer.bit(true);   // color description
    writer.bits(9, 8);  // BT.2020 primaries
    writer.bits(16, 8); // PQ
    writer.bits(9, 8);  // BT.2020 non-constant matrix

    std::vector<std::uint8_t> output;
    append_start_code(output);
    output.push_back(static_cast<std::uint8_t>(33u << 1u));
    output.push_back(1);
    const auto escaped = escape_rbsp(writer.finish());
    output.insert(output.end(), escaped.begin(), escaped.end());
    return output;
}

std::vector<std::uint8_t> hevc_hdr_sei_packet(
    const gpu::GpuHdr10StaticMetadata& metadata) {
    std::vector<std::uint8_t> rbsp;
    rbsp.push_back(137);
    rbsp.push_back(24);
    append_be16(rbsp, metadata.green_primary_x);
    append_be16(rbsp, metadata.green_primary_y);
    append_be16(rbsp, metadata.blue_primary_x);
    append_be16(rbsp, metadata.blue_primary_y);
    append_be16(rbsp, metadata.red_primary_x);
    append_be16(rbsp, metadata.red_primary_y);
    append_be16(rbsp, metadata.white_point_x);
    append_be16(rbsp, metadata.white_point_y);
    append_be32(rbsp, metadata.max_mastering_luminance);
    append_be32(rbsp, metadata.min_mastering_luminance);
    rbsp.push_back(144);
    rbsp.push_back(4);
    append_be16(rbsp, metadata.max_content_light_level);
    append_be16(rbsp, metadata.max_frame_average_light_level);
    rbsp.push_back(0x80);

    std::vector<std::uint8_t> output;
    append_start_code(output);
    output.push_back(static_cast<std::uint8_t>(39u << 1u));
    output.push_back(1);
    const auto escaped = escape_rbsp(rbsp);
    output.insert(output.end(), escaped.begin(), escaped.end());
    return output;
}

std::vector<std::uint8_t> h264_sps_packet() {
    BitWriter writer;
    writer.bits(66, 8); // baseline
    writer.bits(0, 8);  // constraints
    writer.bits(30, 8); // level
    writer.ue(0);       // SPS id
    writer.ue(0);       // log2 frame number minus four
    writer.ue(0);       // POC type
    writer.ue(0);       // log2 POC minus four
    writer.ue(0);       // references
    writer.bit(false);  // gaps
    writer.ue(3);
    writer.ue(3);
    writer.bit(true);   // frame-only
    writer.bit(true);   // direct inference
    writer.bit(false);  // crop
    writer.bit(true);   // VUI
    writer.bit(false);  // aspect ratio
    writer.bit(false);  // overscan
    writer.bit(true);   // video signal type
    writer.bits(5, 3);
    writer.bit(true);   // full range
    writer.bit(true);   // color description
    writer.bits(1, 8);  // BT.709 primaries
    writer.bits(4, 8);  // gamma 2.2
    writer.bits(1, 8);  // BT.709 matrix

    std::vector<std::uint8_t> output;
    append_start_code(output);
    output.push_back(0x67);
    const auto escaped = escape_rbsp(writer.finish());
    output.insert(output.end(), escaped.begin(), escaped.end());
    return output;
}

void append_leb128(std::vector<std::uint8_t>& output, std::uint64_t value) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fu);
        value >>= 7u;
        if (value != 0) byte |= 0x80u;
        output.push_back(byte);
    } while (value != 0);
}

void append_av1_obu(
    std::vector<std::uint8_t>& output,
    std::uint8_t type,
    const std::vector<std::uint8_t>& payload) {
    output.push_back(static_cast<std::uint8_t>((type << 3u) | 0x02u));
    append_leb128(output, payload.size());
    output.insert(output.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> av1_sequence_packet() {
    BitWriter writer;
    writer.bits(0, 3);  // Main profile
    writer.bit(true);   // still picture
    writer.bit(true);   // reduced still-picture header
    writer.bits(0, 5);  // level
    writer.bits(5, 4);  // width bits minus one
    writer.bits(5, 4);  // height bits minus one
    writer.bits(63, 6);
    writer.bits(63, 6);
    writer.bit(false);  // 128x128 superblock
    writer.bit(true);   // filter intra
    writer.bit(true);   // intra edge filter
    writer.bit(false);  // superres
    writer.bit(true);   // CDEF
    writer.bit(true);   // restoration
    writer.bit(true);   // high bit depth (10-bit)
    writer.bit(false);  // not monochrome
    writer.bit(true);   // color description
    writer.bits(9, 8);
    writer.bits(16, 8);
    writer.bits(9, 8);
    writer.bit(false);  // studio range
    std::vector<std::uint8_t> output;
    append_av1_obu(output, 1, writer.finish());
    return output;
}

std::uint16_t cta_to_av1_chromaticity(std::uint16_t value) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint64_t>(value) * 65'536u + 25'000u)
        / 50'000u);
}

std::vector<std::uint8_t> av1_hdr_metadata_packet(
    const gpu::GpuHdr10StaticMetadata& metadata) {
    std::vector<std::uint8_t> output;
    std::vector<std::uint8_t> cll{1};
    append_be16(cll, metadata.max_content_light_level);
    append_be16(cll, metadata.max_frame_average_light_level);
    cll.push_back(0x80);
    append_av1_obu(output, 5, cll);

    std::vector<std::uint8_t> mdcv{2};
    append_be16(mdcv, cta_to_av1_chromaticity(metadata.red_primary_x));
    append_be16(mdcv, cta_to_av1_chromaticity(metadata.red_primary_y));
    append_be16(mdcv, cta_to_av1_chromaticity(metadata.green_primary_x));
    append_be16(mdcv, cta_to_av1_chromaticity(metadata.green_primary_y));
    append_be16(mdcv, cta_to_av1_chromaticity(metadata.blue_primary_x));
    append_be16(mdcv, cta_to_av1_chromaticity(metadata.blue_primary_y));
    append_be16(mdcv, cta_to_av1_chromaticity(metadata.white_point_x));
    append_be16(mdcv, cta_to_av1_chromaticity(metadata.white_point_y));
    append_be32(mdcv, metadata.max_mastering_luminance * 256u);
    append_be32(
        mdcv,
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(metadata.min_mastering_luminance)
                    * 16'384u
                + 5'000u) / 10'000u));
    mdcv.push_back(0x80);
    append_av1_obu(output, 5, mdcv);
    return output;
}

void test_hevc_vui_and_hdr10_merge() {
    gpu::EncodedVideoMetadata metadata;
    const auto sps = hevc_sps_packet();
    gpu::EncodedPacket packet{
        sps.data(), sps.size(), gpu::VideoCodec::hevc, 0, 0, true, true};
    require(gpu::inspect_encoded_packet_metadata(packet, metadata),
        "HEVC SPS inspection failed");
    require(metadata.color_description_present
            && metadata.color_primaries == 9
            && metadata.transfer_characteristics == 16
            && metadata.matrix_coefficients == 9
            && !metadata.full_range,
        "HEVC VUI color metadata was decoded incorrectly");

    gpu::GpuHdr10StaticMetadata expected;
    expected.enabled = true;
    expected.max_mastering_luminance = 1'500;
    expected.min_mastering_luminance = 5;
    expected.max_content_light_level = 1'200;
    expected.max_frame_average_light_level = 450;
    const auto sei = hevc_hdr_sei_packet(expected);
    packet.data = sei.data();
    packet.size = sei.size();
    require(gpu::inspect_encoded_packet_metadata(packet, metadata),
        "HEVC HDR SEI inspection failed");
    require(gpu::validate_encoded_video_metadata(
            metadata,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
            expected),
        "HEVC VUI/HDR10 validation failed");

    auto wrong = expected;
    ++wrong.max_content_light_level;
    const auto rejected = gpu::validate_encoded_video_metadata(
        metadata,
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
        wrong);
    require(!rejected && rejected.status == gpu::GpuStatus::unsupported,
        "HDR10 validation accepted mismatched MaxCLL");
}

void test_h264_vui() {
    const auto sps = h264_sps_packet();
    const gpu::EncodedPacket packet{
        sps.data(), sps.size(), gpu::VideoCodec::h264, 0, 0, true, true};
    gpu::EncodedVideoMetadata metadata;
    require(gpu::inspect_encoded_packet_metadata(packet, metadata),
        "H.264 SPS inspection failed");
    require(gpu::validate_encoded_video_metadata(
            metadata, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),
        "H.264 VUI validation failed");
}

void test_av1_color_and_hdr10_metadata() {
    gpu::EncodedVideoMetadata metadata;
    const auto sequence = av1_sequence_packet();
    gpu::EncodedPacket packet{
        sequence.data(), sequence.size(), gpu::VideoCodec::av1,
        0, 0, true, false};
    require(gpu::inspect_encoded_packet_metadata(packet, metadata),
        "AV1 sequence-header inspection failed");
    gpu::GpuHdr10StaticMetadata expected;
    expected.enabled = true;
    const auto hdr = av1_hdr_metadata_packet(expected);
    packet.data = hdr.data();
    packet.size = hdr.size();
    require(gpu::inspect_encoded_packet_metadata(packet, metadata),
        "AV1 HDR metadata inspection failed");
    require(gpu::validate_encoded_video_metadata(
            metadata,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
            expected),
        "AV1 color/HDR metadata validation failed");
}

void test_fail_closed_inputs() {
    const std::uint8_t malformed[] = {0, 0, 1, 0x67};
    gpu::EncodedPacket packet{
        malformed, sizeof(malformed), gpu::VideoCodec::h264,
        0, 0, false, true};
    gpu::EncodedVideoMetadata metadata;
    const auto rejected = gpu::inspect_encoded_packet_metadata(packet, metadata);
    require(!rejected && rejected.status == gpu::GpuStatus::invalid_argument,
        "malformed SPS did not fail closed");
    const std::uint8_t malformed_av1[] = {0x82};
    packet = {
        malformed_av1, sizeof(malformed_av1), gpu::VideoCodec::av1,
        0, 0, false, false};
    const auto invalid_av1 = gpu::inspect_encoded_packet_metadata(
        packet, metadata);
    require(!invalid_av1
            && invalid_av1.status == gpu::GpuStatus::invalid_argument,
        "malformed AV1 OBU did not fail closed");
}

} // namespace

int main() {
    try {
        test_hevc_vui_and_hdr10_merge();
        test_h264_vui();
        test_av1_color_and_hdr10_metadata();
        test_fail_closed_inputs();
        std::cout << "bitstream metadata tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bitstream metadata test failed: " << error.what() << '\n';
        return 1;
    }
}
