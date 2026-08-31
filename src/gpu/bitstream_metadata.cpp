#include <fluxcap/gpu.hpp>

#include <mferror.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace fluxcap::gpu {
namespace {

GpuError result(
    GpuStatus status,
    HRESULT hresult,
    const char* message) noexcept {
    GpuError output;
    output.status = status;
    output.hresult = hresult;
    if (message != nullptr) {
        (void)strncpy_s(
            output.message.data(), output.message.size(), message, _TRUNCATE);
    }
    return output;
}

class BitReader final {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) noexcept
        : bytes_(bytes) {}

    bool read_bits(std::uint32_t count, std::uint32_t& value) noexcept {
        value = 0;
        if (count > 32 || count > remaining()) return false;
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::size_t byte = bit_ / 8;
            const std::uint32_t shift = 7u - static_cast<std::uint32_t>(bit_ % 8);
            value = (value << 1u) | ((bytes_[byte] >> shift) & 1u);
            ++bit_;
        }
        return true;
    }

    bool read_bit(bool& value) noexcept {
        std::uint32_t bit = 0;
        if (!read_bits(1, bit)) return false;
        value = bit != 0;
        return true;
    }

    bool skip(std::uint32_t count) noexcept {
        if (count > remaining()) return false;
        bit_ += count;
        return true;
    }

    bool read_ue(std::uint32_t& value) noexcept {
        std::uint32_t leading = 0;
        bool bit = false;
        while (true) {
            if (!read_bit(bit)) return false;
            if (bit) break;
            if (++leading > 31) return false;
        }
        std::uint32_t suffix = 0;
        if (leading != 0 && !read_bits(leading, suffix)) return false;
        const std::uint64_t decoded = ((std::uint64_t{1} << leading) - 1u)
            + suffix;
        if (decoded > std::numeric_limits<std::uint32_t>::max()) return false;
        value = static_cast<std::uint32_t>(decoded);
        return true;
    }

    bool read_se(std::int32_t& value) noexcept {
        std::uint32_t code = 0;
        if (!read_ue(code)) return false;
        const std::int64_t magnitude = (static_cast<std::uint64_t>(code) + 1) / 2;
        const std::int64_t decoded = (code & 1u) != 0 ? magnitude : -magnitude;
        if (decoded < std::numeric_limits<std::int32_t>::min()
            || decoded > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        value = static_cast<std::int32_t>(decoded);
        return true;
    }

private:
    std::uint32_t remaining() const noexcept {
        const std::size_t total = bytes_.size() * 8u;
        return bit_ <= total
            ? static_cast<std::uint32_t>(
                std::min<std::size_t>(total - bit_, UINT32_MAX))
            : 0;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t bit_ = 0;
};

std::vector<std::uint8_t> rbsp(std::span<const std::uint8_t> source) {
    std::vector<std::uint8_t> output;
    output.reserve(source.size());
    std::uint32_t zeros = 0;
    for (const std::uint8_t byte : source) {
        if (zeros >= 2 && byte == 0x03) {
            zeros = 0;
            continue;
        }
        output.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return output;
}

std::uint16_t read_be16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8u) | bytes[1]);
}

std::uint32_t read_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24u)
        | (static_cast<std::uint32_t>(bytes[1]) << 16u)
        | (static_cast<std::uint32_t>(bytes[2]) << 8u)
        | bytes[3];
}

bool parse_sei(
    std::span<const std::uint8_t> bytes,
    EncodedVideoMetadata& output) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (bytes[offset] == 0x80) return true;
        std::uint32_t type = 0;
        while (offset < bytes.size() && bytes[offset] == 0xff) {
            if (type > UINT32_MAX - 255u) return false;
            type += 255u;
            ++offset;
        }
        if (offset >= bytes.size()) return false;
        type += bytes[offset++];
        std::uint32_t size = 0;
        while (offset < bytes.size() && bytes[offset] == 0xff) {
            if (size > UINT32_MAX - 255u) return false;
            size += 255u;
            ++offset;
        }
        if (offset >= bytes.size()) return false;
        size += bytes[offset++];
        if (size > bytes.size() - offset) return false;
        const std::uint8_t* payload = bytes.data() + offset;
        if (type == 137 && size >= 24) {
            // ST 2086 syntax order is green, blue, red.
            output.hdr10.green_primary_x = read_be16(payload + 0);
            output.hdr10.green_primary_y = read_be16(payload + 2);
            output.hdr10.blue_primary_x = read_be16(payload + 4);
            output.hdr10.blue_primary_y = read_be16(payload + 6);
            output.hdr10.red_primary_x = read_be16(payload + 8);
            output.hdr10.red_primary_y = read_be16(payload + 10);
            output.hdr10.white_point_x = read_be16(payload + 12);
            output.hdr10.white_point_y = read_be16(payload + 14);
            output.hdr10.max_mastering_luminance = read_be32(payload + 16);
            output.hdr10.min_mastering_luminance = read_be32(payload + 20);
            output.mastering_display_present = true;
            output.hdr10.enabled = true;
        } else if (type == 144 && size >= 4) {
            output.hdr10.max_content_light_level = read_be16(payload + 0);
            output.hdr10.max_frame_average_light_level = read_be16(payload + 2);
            output.content_light_level_present = true;
            output.hdr10.enabled = true;
        }
        offset += size;
    }
    return true;
}

bool parse_vui_color(
    BitReader& reader,
    EncodedVideoMetadata& output) noexcept {
    bool present = false;
    if (!reader.read_bit(present)) return false;
    if (present) {
        std::uint32_t aspect = 0;
        if (!reader.read_bits(8, aspect)) return false;
        if (aspect == 255 && !reader.skip(32)) return false;
    }
    if (!reader.read_bit(present)) return false;
    if (present && !reader.skip(1)) return false;
    if (!reader.read_bit(present)) return false;
    if (!present) return true;
    output.video_signal_type_present = true;
    std::uint32_t value = 0;
    bool full_range = false;
    bool color_present = false;
    if (!reader.read_bits(3, value)
        || !reader.read_bit(full_range)
        || !reader.read_bit(color_present)) {
        return false;
    }
    output.full_range = full_range;
    if (!color_present) return true;
    std::uint32_t primaries = 0;
    std::uint32_t transfer = 0;
    std::uint32_t matrix = 0;
    if (!reader.read_bits(8, primaries)
        || !reader.read_bits(8, transfer)
        || !reader.read_bits(8, matrix)) {
        return false;
    }
    output.color_description_present = true;
    output.color_primaries = static_cast<std::uint8_t>(primaries);
    output.transfer_characteristics = static_cast<std::uint8_t>(transfer);
    output.matrix_coefficients = static_cast<std::uint8_t>(matrix);
    return true;
}

bool skip_h264_scaling_list(BitReader& reader, std::uint32_t count) noexcept {
    std::int32_t last = 8;
    std::int32_t next = 8;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (next != 0) {
            std::int32_t delta = 0;
            if (!reader.read_se(delta)) return false;
            next = (last + delta + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
    return true;
}

bool parse_h264_sps(
    std::span<const std::uint8_t> bytes,
    EncodedVideoMetadata& output) noexcept {
    BitReader reader(bytes);
    std::uint32_t profile = 0;
    std::uint32_t value = 0;
    if (!reader.read_bits(8, profile)
        || !reader.skip(16)
        || !reader.read_ue(value)) {
        return false;
    }
    constexpr std::array<std::uint32_t, 11> high_profiles{
        44, 83, 86, 100, 110, 118, 122, 128, 134, 135, 138};
    if (std::find(high_profiles.begin(), high_profiles.end(), profile)
        != high_profiles.end()) {
        std::uint32_t chroma = 0;
        if (!reader.read_ue(chroma) || chroma > 3) return false;
        if (chroma == 3 && !reader.skip(1)) return false;
        if (!reader.read_ue(value) || !reader.read_ue(value)
            || !reader.skip(1)) {
            return false;
        }
        bool scaling = false;
        if (!reader.read_bit(scaling)) return false;
        if (scaling) {
            const std::uint32_t count = chroma == 3 ? 12 : 8;
            for (std::uint32_t index = 0; index < count; ++index) {
                bool list = false;
                if (!reader.read_bit(list)) return false;
                if (list && !skip_h264_scaling_list(
                        reader, index < 6 ? 16 : 64)) {
                    return false;
                }
            }
        }
    }
    if (!reader.read_ue(value)) return false;
    std::uint32_t poc_type = 0;
    if (!reader.read_ue(poc_type)) return false;
    if (poc_type == 0) {
        if (!reader.read_ue(value)) return false;
    } else if (poc_type == 1) {
        std::int32_t signed_value = 0;
        std::uint32_t count = 0;
        if (!reader.skip(1)
            || !reader.read_se(signed_value)
            || !reader.read_se(signed_value)
            || !reader.read_ue(count)
            || count > 256) {
            return false;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            if (!reader.read_se(signed_value)) return false;
        }
    } else if (poc_type != 2) {
        return false;
    }
    if (!reader.read_ue(value) || !reader.skip(1)
        || !reader.read_ue(value) || !reader.read_ue(value)) {
        return false;
    }
    bool frame_only = false;
    if (!reader.read_bit(frame_only)) return false;
    if (!frame_only && !reader.skip(1)) return false;
    if (!reader.skip(1)) return false;
    bool crop = false;
    if (!reader.read_bit(crop)) return false;
    if (crop) {
        for (std::uint32_t index = 0; index < 4; ++index) {
            if (!reader.read_ue(value)) return false;
        }
    }
    bool vui = false;
    return reader.read_bit(vui) && (!vui || parse_vui_color(reader, output));
}

bool skip_hevc_profile_tier_level(
    BitReader& reader,
    std::uint32_t max_sub_layers) noexcept {
    if (!reader.skip(96)) return false;
    std::array<bool, 8> profile{};
    std::array<bool, 8> level{};
    for (std::uint32_t index = 0; index < max_sub_layers; ++index) {
        if (!reader.read_bit(profile[index]) || !reader.read_bit(level[index])) {
            return false;
        }
    }
    if (max_sub_layers > 0 && max_sub_layers < 8
        && !reader.skip((8 - max_sub_layers) * 2)) {
        return false;
    }
    for (std::uint32_t index = 0; index < max_sub_layers; ++index) {
        if (profile[index] && !reader.skip(88)) return false;
        if (level[index] && !reader.skip(8)) return false;
    }
    return true;
}

bool skip_hevc_scaling_list(BitReader& reader) noexcept {
    for (std::uint32_t size = 0; size < 4; ++size) {
        const std::uint32_t step = size == 3 ? 3 : 1;
        for (std::uint32_t matrix = 0; matrix < 6; matrix += step) {
            bool explicit_coefficients = false;
            if (!reader.read_bit(explicit_coefficients)) return false;
            std::uint32_t value = 0;
            if (!explicit_coefficients) {
                if (!reader.read_ue(value)) return false;
                continue;
            }
            std::int32_t delta = 0;
            if (size > 1 && !reader.read_se(delta)) return false;
            const std::uint32_t coefficients = std::min(
                64u, 1u << (4u + (size << 1u)));
            for (std::uint32_t index = 0; index < coefficients; ++index) {
                if (!reader.read_se(delta)) return false;
            }
        }
    }
    return true;
}

bool skip_hevc_short_term_sets(
    BitReader& reader,
    std::uint32_t count,
    std::vector<std::uint32_t>& delta_counts) {
    if (count > 64) return false;
    delta_counts.clear();
    delta_counts.reserve(count);
    for (std::uint32_t set = 0; set < count; ++set) {
        bool predicted = false;
        if (set != 0 && !reader.read_bit(predicted)) return false;
        if (predicted) {
            if (!reader.skip(1)) return false;
            std::uint32_t value = 0;
            if (!reader.read_ue(value)) return false;
            const std::uint32_t reference = delta_counts.back();
            std::uint32_t current = 0;
            for (std::uint32_t index = 0; index <= reference; ++index) {
                bool used = false;
                bool use_delta = false;
                if (!reader.read_bit(used)) return false;
                if (!used && !reader.read_bit(use_delta)) return false;
                if (used || use_delta) ++current;
            }
            delta_counts.push_back(current);
        } else {
            std::uint32_t negative = 0;
            std::uint32_t positive = 0;
            if (!reader.read_ue(negative) || !reader.read_ue(positive)
                || negative > 64 || positive > 64
                || negative + positive > 64) {
                return false;
            }
            std::uint32_t value = 0;
            for (std::uint32_t index = 0; index < negative + positive; ++index) {
                if (!reader.read_ue(value) || !reader.skip(1)) return false;
            }
            delta_counts.push_back(negative + positive);
        }
    }
    return true;
}

bool parse_hevc_sps(
    std::span<const std::uint8_t> bytes,
    EncodedVideoMetadata& output) {
    BitReader reader(bytes);
    std::uint32_t max_sub_layers = 0;
    std::uint32_t value = 0;
    if (!reader.skip(4)
        || !reader.read_bits(3, max_sub_layers)
        || !reader.skip(1)
        || !skip_hevc_profile_tier_level(reader, max_sub_layers)
        || !reader.read_ue(value)) {
        return false;
    }
    std::uint32_t chroma = 0;
    if (!reader.read_ue(chroma) || chroma > 3) return false;
    if (chroma == 3 && !reader.skip(1)) return false;
    if (!reader.read_ue(value) || !reader.read_ue(value)) return false;
    bool conformance = false;
    if (!reader.read_bit(conformance)) return false;
    if (conformance) {
        for (std::uint32_t index = 0; index < 4; ++index) {
            if (!reader.read_ue(value)) return false;
        }
    }
    std::uint32_t log2_max_poc_minus4 = 0;
    if (!reader.read_ue(value) || !reader.read_ue(value)
        || !reader.read_ue(log2_max_poc_minus4)
        || log2_max_poc_minus4 > 12) {
        return false;
    }
    bool ordering_all_layers = false;
    if (!reader.read_bit(ordering_all_layers)) return false;
    const std::uint32_t first_layer = ordering_all_layers ? 0 : max_sub_layers;
    for (std::uint32_t index = first_layer; index <= max_sub_layers; ++index) {
        if (!reader.read_ue(value) || !reader.read_ue(value)
            || !reader.read_ue(value)) {
            return false;
        }
    }
    for (std::uint32_t index = 0; index < 6; ++index) {
        if (!reader.read_ue(value)) return false;
    }
    bool scaling_enabled = false;
    if (!reader.read_bit(scaling_enabled)) return false;
    if (scaling_enabled) {
        bool scaling_present = false;
        if (!reader.read_bit(scaling_present)) return false;
        if (scaling_present && !skip_hevc_scaling_list(reader)) return false;
    }
    if (!reader.skip(2)) return false;
    bool pcm = false;
    if (!reader.read_bit(pcm)) return false;
    if (pcm && (!reader.skip(8)
            || !reader.read_ue(value) || !reader.read_ue(value)
            || !reader.skip(1))) {
        return false;
    }
    std::uint32_t short_term_count = 0;
    std::vector<std::uint32_t> delta_counts;
    if (!reader.read_ue(short_term_count)
        || !skip_hevc_short_term_sets(
            reader, short_term_count, delta_counts)) {
        return false;
    }
    bool long_term = false;
    if (!reader.read_bit(long_term)) return false;
    if (long_term) {
        std::uint32_t count = 0;
        if (!reader.read_ue(count) || count > 32) return false;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (!reader.skip(log2_max_poc_minus4 + 4u)
                || !reader.skip(1)) {
                return false;
            }
        }
    }
    if (!reader.skip(2)) return false;
    bool vui = false;
    return reader.read_bit(vui) && (!vui || parse_vui_color(reader, output));
}

template <class Callback>
bool for_each_annex_b_nal(
    std::span<const std::uint8_t> bytes,
    Callback&& callback) {
    const auto find_start = [&](std::size_t from, std::size_t& length) {
        for (std::size_t index = from; index + 3 <= bytes.size(); ++index) {
            if (bytes[index] != 0 || bytes[index + 1] != 0) continue;
            if (bytes[index + 2] == 1) {
                length = 3;
                return index;
            }
            if (index + 4 <= bytes.size()
                && bytes[index + 2] == 0 && bytes[index + 3] == 1) {
                length = 4;
                return index;
            }
        }
        length = 0;
        return bytes.size();
    };

    std::size_t prefix = 0;
    std::size_t start = find_start(0, prefix);
    if (start == bytes.size()) return false;
    while (start < bytes.size()) {
        const std::size_t nal_start = start + prefix;
        std::size_t next_prefix = 0;
        const std::size_t next = find_start(nal_start, next_prefix);
        std::size_t nal_end = next;
        while (nal_end > nal_start && bytes[nal_end - 1] == 0) --nal_end;
        if (nal_end > nal_start
            && !callback(bytes.subspan(nal_start, nal_end - nal_start))) {
            return false;
        }
        start = next;
        prefix = next_prefix;
    }
    return true;
}

bool inspect_h264(
    std::span<const std::uint8_t> packet,
    EncodedVideoMetadata& output) {
    return for_each_annex_b_nal(packet, [&](std::span<const std::uint8_t> nal) {
        if (nal.empty()) return false;
        const std::uint8_t type = nal[0] & 0x1fu;
        if (type != 6 && type != 7) return true;
        const auto data = rbsp(nal.subspan(1));
        return type == 7
            ? parse_h264_sps(data, output)
            : parse_sei(data, output);
    });
}

bool inspect_hevc(
    std::span<const std::uint8_t> packet,
    EncodedVideoMetadata& output) {
    return for_each_annex_b_nal(packet, [&](std::span<const std::uint8_t> nal) {
        if (nal.size() < 2) return false;
        const std::uint8_t type = (nal[0] >> 1u) & 0x3fu;
        if (type != 33 && type != 39 && type != 40) return true;
        const auto data = rbsp(nal.subspan(2));
        return type == 33
            ? parse_hevc_sps(data, output)
            : parse_sei(data, output);
    });
}

bool read_leb128(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset,
    std::uint64_t& value) noexcept {
    value = 0;
    for (std::uint32_t index = 0; index < 8; ++index) {
        if (offset >= bytes.size()) return false;
        const std::uint8_t byte = bytes[offset++];
        value |= static_cast<std::uint64_t>(byte & 0x7fu) << (index * 7u);
        if ((byte & 0x80u) == 0) return true;
    }
    return false;
}

bool skip_av1_timing_info(
    BitReader& reader,
    bool& decoder_model_present,
    std::uint32_t& buffer_delay_length) noexcept {
    if (!reader.skip(64)) return false;
    bool equal_picture_interval = false;
    std::uint32_t value = 0;
    if (!reader.read_bit(equal_picture_interval)
        || (equal_picture_interval && !reader.read_ue(value))
        || !reader.read_bit(decoder_model_present)) {
        return false;
    }
    if (!decoder_model_present) return true;
    std::uint32_t minus_one = 0;
    if (!reader.read_bits(5, minus_one)
        || !reader.skip(32 + 5 + 5)) {
        return false;
    }
    buffer_delay_length = minus_one + 1u;
    return true;
}

bool parse_av1_sequence_header(
    std::span<const std::uint8_t> bytes,
    EncodedVideoMetadata& output) noexcept {
    BitReader reader(bytes);
    std::uint32_t profile = 0;
    bool reduced = false;
    if (!reader.read_bits(3, profile) || profile > 2
        || !reader.skip(1) || !reader.read_bit(reduced)) {
        return false;
    }
    bool decoder_model_present = false;
    bool initial_delay_present = false;
    std::uint32_t buffer_delay_length = 0;
    if (reduced) {
        if (!reader.skip(5)) return false;
    } else {
        bool timing_present = false;
        if (!reader.read_bit(timing_present)) return false;
        if (timing_present
            && !skip_av1_timing_info(
                reader, decoder_model_present, buffer_delay_length)) {
            return false;
        }
        if (!reader.read_bit(initial_delay_present)) return false;
        std::uint32_t operating_points_minus_one = 0;
        if (!reader.read_bits(5, operating_points_minus_one)
            || operating_points_minus_one > 31) {
            return false;
        }
        for (std::uint32_t index = 0;
             index <= operating_points_minus_one;
             ++index) {
            std::uint32_t level = 0;
            if (!reader.skip(12) || !reader.read_bits(5, level)) return false;
            if (level > 7 && !reader.skip(1)) return false;
            if (decoder_model_present) {
                bool present = false;
                if (!reader.read_bit(present)) return false;
                if (present
                    && (!reader.skip(buffer_delay_length * 2u)
                        || !reader.skip(1))) {
                    return false;
                }
            }
            if (initial_delay_present) {
                bool present = false;
                if (!reader.read_bit(present)
                    || (present && !reader.skip(4))) {
                    return false;
                }
            }
        }
    }

    std::uint32_t width_bits_minus_one = 0;
    std::uint32_t height_bits_minus_one = 0;
    if (!reader.read_bits(4, width_bits_minus_one)
        || !reader.read_bits(4, height_bits_minus_one)
        || !reader.skip(width_bits_minus_one + 1u)
        || !reader.skip(height_bits_minus_one + 1u)) {
        return false;
    }
    if (!reduced) {
        bool frame_ids = false;
        if (!reader.read_bit(frame_ids)
            || (frame_ids && !reader.skip(7))) {
            return false;
        }
    }
    if (!reader.skip(3)) return false;
    if (!reduced) {
        if (!reader.skip(4)) return false;
        bool order_hint = false;
        if (!reader.read_bit(order_hint)) return false;
        if (order_hint && !reader.skip(2)) return false;
        bool choose_screen_tools = false;
        if (!reader.read_bit(choose_screen_tools)) return false;
        bool screen_tools = true;
        if (!choose_screen_tools && !reader.read_bit(screen_tools)) return false;
        if (screen_tools) {
            bool choose_integer_mv = false;
            if (!reader.read_bit(choose_integer_mv)) return false;
            if (!choose_integer_mv && !reader.skip(1)) return false;
        }
        if (order_hint && !reader.skip(3)) return false;
    }
    if (!reader.skip(3)) return false;

    bool high_bitdepth = false;
    bool twelve_bit = false;
    if (!reader.read_bit(high_bitdepth)) return false;
    if (profile == 2 && high_bitdepth && !reader.read_bit(twelve_bit)) {
        return false;
    }
    bool monochrome = false;
    if (profile != 1 && !reader.read_bit(monochrome)) return false;
    bool color_description = false;
    if (!reader.read_bit(color_description)) return false;
    std::uint32_t primaries = 2;
    std::uint32_t transfer = 2;
    std::uint32_t matrix = 2;
    if (color_description
        && (!reader.read_bits(8, primaries)
            || !reader.read_bits(8, transfer)
            || !reader.read_bits(8, matrix))) {
        return false;
    }
    output.video_signal_type_present = true;
    output.color_description_present = color_description;
    output.color_primaries = static_cast<std::uint8_t>(primaries);
    output.transfer_characteristics = static_cast<std::uint8_t>(transfer);
    output.matrix_coefficients = static_cast<std::uint8_t>(matrix);
    if (monochrome) return reader.read_bit(output.full_range);
    if (primaries == 1 && transfer == 13 && matrix == 0) {
        output.full_range = true;
        return true;
    }
    return reader.read_bit(output.full_range);
}

std::uint16_t av1_chromaticity_to_cta(std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>(std::min<std::uint64_t>(
        50'000u,
        (static_cast<std::uint64_t>(value) * 50'000u + 32'768u)
            / 65'536u));
}

bool parse_av1_metadata(
    std::span<const std::uint8_t> bytes,
    EncodedVideoMetadata& output) noexcept {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    if (!read_leb128(bytes, offset, type)) return false;
    const std::uint8_t* payload = bytes.data() + offset;
    const std::size_t size = bytes.size() - offset;
    if (type == 1) {
        if (size < 4) return false;
        output.hdr10.max_content_light_level = read_be16(payload);
        output.hdr10.max_frame_average_light_level = read_be16(payload + 2);
        output.content_light_level_present = true;
        output.hdr10.enabled = true;
    } else if (type == 2) {
        if (size < 24) return false;
        output.hdr10.red_primary_x = av1_chromaticity_to_cta(
            read_be16(payload + 0));
        output.hdr10.red_primary_y = av1_chromaticity_to_cta(
            read_be16(payload + 2));
        output.hdr10.green_primary_x = av1_chromaticity_to_cta(
            read_be16(payload + 4));
        output.hdr10.green_primary_y = av1_chromaticity_to_cta(
            read_be16(payload + 6));
        output.hdr10.blue_primary_x = av1_chromaticity_to_cta(
            read_be16(payload + 8));
        output.hdr10.blue_primary_y = av1_chromaticity_to_cta(
            read_be16(payload + 10));
        output.hdr10.white_point_x = av1_chromaticity_to_cta(
            read_be16(payload + 12));
        output.hdr10.white_point_y = av1_chromaticity_to_cta(
            read_be16(payload + 14));
        const std::uint32_t max_fixed = read_be32(payload + 16);
        const std::uint32_t min_fixed = read_be32(payload + 20);
        output.hdr10.max_mastering_luminance =
            (static_cast<std::uint64_t>(max_fixed) + 128u) / 256u;
        output.hdr10.min_mastering_luminance =
            (static_cast<std::uint64_t>(min_fixed) * 10'000u + 8'192u)
            / 16'384u;
        output.mastering_display_present = true;
        output.hdr10.enabled = true;
    }
    return true;
}

bool inspect_av1(
    std::span<const std::uint8_t> packet,
    EncodedVideoMetadata& output) noexcept {
    std::size_t offset = 0;
    while (offset < packet.size()) {
        const std::uint8_t header = packet[offset++];
        if ((header & 0x81u) != 0) return false;
        const std::uint8_t type = (header >> 3u) & 0x0fu;
        const bool extension = (header & 0x04u) != 0;
        const bool has_size = (header & 0x02u) != 0;
        if (extension) {
            if (offset >= packet.size() || (packet[offset] & 0x07u) != 0) {
                return false;
            }
            ++offset;
        }
        std::uint64_t payload_size = packet.size() - offset;
        if (has_size && !read_leb128(packet, offset, payload_size)) return false;
        if (payload_size > packet.size() - offset) return false;
        const auto payload = packet.subspan(
            offset, static_cast<std::size_t>(payload_size));
        if (type == 1 && !parse_av1_sequence_header(payload, output)) {
            return false;
        }
        if (type == 5 && !parse_av1_metadata(payload, output)) return false;
        offset += static_cast<std::size_t>(payload_size);
        if (!has_size) break;
    }
    return true;
}

bool expected_color_codes(
    DXGI_COLOR_SPACE_TYPE color_space,
    std::uint8_t& primaries,
    std::uint8_t& transfer,
    std::uint8_t& matrix,
    bool& full_range) noexcept {
    switch (color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        primaries = 1; transfer = 4; matrix = 1; full_range = true; return true;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        primaries = 1; transfer = 8; matrix = 1; full_range = true; return true;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709:
        primaries = 1; transfer = 1; matrix = 1; full_range = false; return true;
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709:
        primaries = 1; transfer = 1; matrix = 1; full_range = true; return true;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020:
        primaries = 9; transfer = 14; matrix = 9; full_range = false; return true;
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020:
        primaries = 9; transfer = 14; matrix = 9; full_range = true; return true;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        primaries = 9; transfer = 16; matrix = 9; full_range = false; return true;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
        primaries = 9; transfer = 18; matrix = 9; full_range = false; return true;
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        primaries = 9; transfer = 18; matrix = 9; full_range = true; return true;
    default:
        return false;
    }
}

bool same_hdr10(
    const GpuHdr10StaticMetadata& left,
    const GpuHdr10StaticMetadata& right) noexcept {
    return left.red_primary_x == right.red_primary_x
        && left.red_primary_y == right.red_primary_y
        && left.green_primary_x == right.green_primary_x
        && left.green_primary_y == right.green_primary_y
        && left.blue_primary_x == right.blue_primary_x
        && left.blue_primary_y == right.blue_primary_y
        && left.white_point_x == right.white_point_x
        && left.white_point_y == right.white_point_y
        && left.max_mastering_luminance == right.max_mastering_luminance
        && left.min_mastering_luminance == right.min_mastering_luminance
        && left.max_content_light_level == right.max_content_light_level
        && left.max_frame_average_light_level
            == right.max_frame_average_light_level;
}

} // namespace

GpuError inspect_encoded_packet_metadata(
    const EncodedPacket& packet,
    EncodedVideoMetadata& output) noexcept {
    if (packet.data == nullptr || packet.size == 0) {
        return result(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "encoded packet is empty");
    }
    if (packet.codec != VideoCodec::av1 && !packet.annex_b) {
        return result(
            GpuStatus::unsupported, E_NOTIMPL,
            "H.264/HEVC metadata inspection requires Annex-B packets");
    }
    try {
        const std::span<const std::uint8_t> bytes(packet.data, packet.size);
        const bool parsed = packet.codec == VideoCodec::h264
            ? inspect_h264(bytes, output)
            : packet.codec == VideoCodec::hevc
            ? inspect_hevc(bytes, output)
            : inspect_av1(bytes, output);
        return parsed
            ? result(GpuStatus::ok, S_OK, "ok")
            : result(
                GpuStatus::invalid_argument, E_INVALIDARG,
                "encoded packet metadata is malformed");
    } catch (const std::bad_alloc&) {
        return result(
            GpuStatus::out_of_memory, E_OUTOFMEMORY,
            "encoded metadata inspection allocation failed");
    } catch (...) {
        return result(
            GpuStatus::system_error, E_FAIL,
            "unknown encoded metadata inspection failure");
    }
}

GpuError validate_encoded_video_metadata(
    const EncodedVideoMetadata& metadata,
    DXGI_COLOR_SPACE_TYPE expected_color_space,
    const GpuHdr10StaticMetadata& expected_hdr10) noexcept {
    std::uint8_t primaries = 0;
    std::uint8_t transfer = 0;
    std::uint8_t matrix = 0;
    bool full_range = false;
    if (!expected_color_codes(
            expected_color_space, primaries, transfer, matrix, full_range)) {
        return result(
            GpuStatus::invalid_argument, E_INVALIDARG,
            "unsupported expected bitstream color space");
    }
    if (!metadata.video_signal_type_present
        || !metadata.color_description_present
        || metadata.color_primaries != primaries
        || metadata.transfer_characteristics != transfer
        || metadata.matrix_coefficients != matrix
        || metadata.full_range != full_range) {
        return result(
            GpuStatus::unsupported, MF_E_INVALIDMEDIATYPE,
            "bitstream VUI color description does not match");
    }
    if (expected_hdr10.enabled
        && (!metadata.mastering_display_present
            || !metadata.content_light_level_present
            || !same_hdr10(metadata.hdr10, expected_hdr10))) {
        return result(
            GpuStatus::unsupported, MF_E_INVALIDMEDIATYPE,
            "bitstream HDR10 static metadata does not match");
    }
    return result(GpuStatus::ok, S_OK, "ok");
}

} // namespace fluxcap::gpu
