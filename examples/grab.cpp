#include <fluxcap/fluxcap.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

#pragma pack(push, 1)
struct BitmapFileHeader {
    std::uint16_t signature;
    std::uint32_t file_size;
    std::uint16_t reserved1;
    std::uint16_t reserved2;
    std::uint32_t pixel_offset;
};

struct BitmapInfoHeader {
    std::uint32_t header_size;
    std::int32_t width;
    std::int32_t height;
    std::uint16_t planes;
    std::uint16_t bits_per_pixel;
    std::uint32_t compression;
    std::uint32_t image_size;
    std::int32_t horizontal_pixels_per_meter;
    std::int32_t vertical_pixels_per_meter;
    std::uint32_t colors_used;
    std::uint32_t important_colors;
};
#pragma pack(pop)

static_assert(sizeof(BitmapFileHeader) == 14);
static_assert(sizeof(BitmapInfoHeader) == 40);

void write_bmp(const std::filesystem::path& path, const fluxcap::Frame& frame) {
    if (!frame || frame.native().format != FLUXCAP_PIXEL_FORMAT_BGRX8) {
        throw std::runtime_error("FluxCap returned an unsupported pixel format");
    }
    if (frame.width() == 0 || frame.height() == 0 || frame.data() == nullptr) {
        throw std::runtime_error("FluxCap returned an empty frame");
    }
    if (frame.width() > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        frame.height() > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error("Frame dimensions exceed the BMP format limits");
    }

    constexpr std::uint64_t header_bytes =
        sizeof(BitmapFileHeader) + sizeof(BitmapInfoHeader);
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(frame.width()) * 4u;
    const std::uint64_t image_bytes = row_bytes * frame.height();
    const std::uint64_t file_bytes = header_bytes + image_bytes;

    if (frame.stride() < row_bytes ||
        image_bytes > std::numeric_limits<std::uint32_t>::max() ||
        file_bytes > std::numeric_limits<std::uint32_t>::max() ||
        row_bytes > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("Frame is too large to write as a standard BMP");
    }

    BitmapFileHeader file_header{};
    file_header.signature = 0x4d42u;
    file_header.file_size = static_cast<std::uint32_t>(file_bytes);
    file_header.pixel_offset = static_cast<std::uint32_t>(header_bytes);

    BitmapInfoHeader info_header{};
    info_header.header_size = sizeof(BitmapInfoHeader);
    info_header.width = static_cast<std::int32_t>(frame.width());
    info_header.height = -static_cast<std::int32_t>(frame.height());
    info_header.planes = 1;
    info_header.bits_per_pixel = 32;
    info_header.compression = 0;
    info_header.image_size = static_cast<std::uint32_t>(image_bytes);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Unable to open the output BMP");
    }

    output.write(
        reinterpret_cast<const char*>(&file_header),
        static_cast<std::streamsize>(sizeof(file_header)));
    output.write(
        reinterpret_cast<const char*>(&info_header),
        static_cast<std::streamsize>(sizeof(info_header)));

    const auto bytes_per_row = static_cast<std::streamsize>(row_bytes);
    for (std::uint32_t y = 0; y < frame.height(); ++y) {
        const auto* row = frame.data() +
            static_cast<std::size_t>(y) * frame.stride();
        output.write(reinterpret_cast<const char*>(row), bytes_per_row);
    }

    if (!output) {
        throw std::runtime_error("Failed while writing the output BMP");
    }
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 2) {
        std::wcerr << L"Usage: fluxcap_grab [output.bmp]\n";
        return 2;
    }

    const std::filesystem::path output_path =
        argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path(L"capture.bmp");

    try {
        fluxcap::Session session;
        const auto frame = session.capture();
        write_bmp(output_path, frame);

        std::wcout << L"Wrote " << output_path << L" ("
                   << frame.width() << L"x" << frame.height() << L", 32-bit BMP)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Capture failed: " << error.what() << '\n';
        return 1;
    }
}
