#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kSourceWidth = 2560;
constexpr std::uint32_t kSourceHeight = 1600;
constexpr std::uint32_t kBytesPerPixel = 4;
constexpr std::uint32_t kCoordinateXBits = 12;
constexpr std::uint32_t kCoordinatePayloadBits = 24;

static_assert(kSourceWidth <= (1u << kCoordinateXBits));
static_assert(
    kSourceHeight <= (1u << (kCoordinatePayloadBits - kCoordinateXBits)));

struct Options final {
    std::uint32_t size = 320;
    std::uint32_t operations = 10'000;
    std::uint32_t warmup = 500;
    std::uint32_t rounds = 8;
    bool show_help = false;
};

struct Measurement final {
    double submit_seconds = 0.0;
    double complete_seconds = 0.0;
    std::uint64_t bytes_per_operation = 0;
};

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

std::uint32_t parse_number(std::wstring_view text, const char* option) {
    if (text.empty()) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }

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

std::wstring_view option_value(
    int& index,
    int argc,
    wchar_t* argv[],
    std::wstring_view argument,
    std::wstring_view name) {
    if (argument == name) {
        if (++index >= argc) {
            throw std::invalid_argument("benchmark option requires a value");
        }
        return argv[index];
    }

    const std::wstring prefix = std::wstring(name) + L"=";
    if (argument.starts_with(prefix)) {
        return argument.substr(prefix.size());
    }
    return {};
}

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.show_help = true;
        } else if (argument == L"--size" || argument.starts_with(L"--size=")) {
            options.size = parse_number(
                option_value(index, argc, argv, argument, L"--size"), "--size");
        } else if (
            argument == L"--operations" || argument.starts_with(L"--operations=")) {
            options.operations = parse_number(
                option_value(index, argc, argv, argument, L"--operations"),
                "--operations");
        } else if (argument == L"--warmup" || argument.starts_with(L"--warmup=")) {
            options.warmup = parse_number(
                option_value(index, argc, argv, argument, L"--warmup"), "--warmup");
        } else if (argument == L"--rounds" || argument.starts_with(L"--rounds=")) {
            options.rounds = parse_number(
                option_value(index, argc, argv, argument, L"--rounds"), "--rounds");
        } else {
            throw std::invalid_argument("unknown benchmark option");
        }
    }

    if (options.size != 320 && options.size != 640) {
        throw std::invalid_argument("--size must be 320 or 640");
    }
    if (options.operations == 0) {
        throw std::invalid_argument("--operations must be greater than zero");
    }
    if (options.rounds < 2) {
        throw std::invalid_argument("--rounds must be at least 2");
    }
    return options;
}

std::uint32_t coordinate_pixel(std::uint32_t x, std::uint32_t y) noexcept {
    // The low 24 bits are a one-to-one coordinate encoding for this source.
    return 0xa5000000u | (y << kCoordinateXBits) | x;
}

ComPtr<ID3D11Texture2D> create_source(ID3D11Device* device) {
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(kSourceWidth) * kSourceHeight);
    for (std::uint32_t y = 0; y < kSourceHeight; ++y) {
        for (std::uint32_t x = 0; x < kSourceWidth; ++x) {
            pixels[static_cast<std::size_t>(y) * kSourceWidth + x] =
                coordinate_pixel(x, y);
        }
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = kSourceWidth;
    description.Height = kSourceHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = kSourceWidth * sizeof(std::uint32_t);

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, &initial, &texture);
    if (FAILED(hr)) {
        fail("source texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

ComPtr<ID3D11Texture2D> create_default_texture(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, nullptr, &texture);
    if (FAILED(hr)) {
        fail("default texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

ComPtr<ID3D11Texture2D> create_staging_texture(
    ID3D11Device* device,
    std::uint32_t size) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = size;
    description.Height = size;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&description, nullptr, &texture);
    if (FAILED(hr)) {
        fail("staging texture creation failed: " + std::to_string(hr));
    }
    return texture;
}

ComPtr<ID3D11Query> create_event_query(ID3D11Device* device) {
    D3D11_QUERY_DESC description{};
    description.Query = D3D11_QUERY_EVENT;

    ComPtr<ID3D11Query> query;
    const HRESULT hr = device->CreateQuery(&description, &query);
    if (FAILED(hr)) {
        fail("D3D11 event query creation failed: " + std::to_string(hr));
    }
    return query;
}

void wait_for_event(ID3D11DeviceContext* context, ID3D11Query* query) {
    context->Flush();
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    for (;;) {
        BOOL complete = FALSE;
        const HRESULT hr = context->GetData(
            query, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && complete != FALSE) return;
        if (FAILED(hr)) {
            fail("D3D11 event query read failed: " + std::to_string(hr));
        }
        if (Clock::now() >= deadline) {
            fail("D3D11 event query timed out after 60 seconds");
        }
        std::this_thread::yield();
    }
}

void submit_path_a(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    ID3D11Texture2D* mailbox,
    ID3D11Texture2D* output,
    const D3D11_BOX& full_box,
    const D3D11_BOX& roi_box,
    std::uint32_t operations) noexcept {
    for (std::uint32_t index = 0; index < operations; ++index) {
        context->CopySubresourceRegion(
            mailbox, 0, 0, 0, 0, source, 0, &full_box);
        context->CopySubresourceRegion(
            output, 0, 0, 0, 0, mailbox, 0, &roi_box);
    }
}

void submit_path_b(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    ID3D11Texture2D* output,
    const D3D11_BOX& roi_box,
    std::uint32_t operations) noexcept {
    for (std::uint32_t index = 0; index < operations; ++index) {
        context->CopySubresourceRegion(
            output, 0, 0, 0, 0, source, 0, &roi_box);
    }
}

Measurement measure_path_a(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    ID3D11Texture2D* mailbox,
    ID3D11Texture2D* output,
    const D3D11_BOX& full_box,
    const D3D11_BOX& roi_box,
    const Options& options,
    std::uint64_t bytes_per_operation) {
    auto query = create_event_query(device);
    submit_path_a(
        context, source, mailbox, output, full_box, roi_box, options.warmup);
    context->End(query.Get());
    wait_for_event(context, query.Get());

    const auto begin = Clock::now();
    submit_path_a(
        context, source, mailbox, output, full_box, roi_box, options.operations);
    const auto submitted = Clock::now();
    context->End(query.Get());
    wait_for_event(context, query.Get());
    const auto complete = Clock::now();

    const HRESULT removed = device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        fail("device removed while measuring path A: " + std::to_string(removed));
    }
    return {
        std::chrono::duration<double>(submitted - begin).count(),
        std::chrono::duration<double>(complete - begin).count(),
        bytes_per_operation};
}

Measurement measure_path_b(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    ID3D11Texture2D* output,
    const D3D11_BOX& roi_box,
    const Options& options,
    std::uint64_t bytes_per_operation) {
    auto query = create_event_query(device);
    submit_path_b(context, source, output, roi_box, options.warmup);
    context->End(query.Get());
    wait_for_event(context, query.Get());

    const auto begin = Clock::now();
    submit_path_b(context, source, output, roi_box, options.operations);
    const auto submitted = Clock::now();
    context->End(query.Get());
    wait_for_event(context, query.Get());
    const auto complete = Clock::now();

    const HRESULT removed = device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        fail("device removed while measuring path B: " + std::to_string(removed));
    }
    return {
        std::chrono::duration<double>(submitted - begin).count(),
        std::chrono::duration<double>(complete - begin).count(),
        bytes_per_operation};
}

void verify_output(
    ID3D11DeviceContext* context,
    ID3D11Query* query,
    ID3D11Texture2D* output,
    ID3D11Texture2D* staging,
    std::uint32_t crop_x,
    std::uint32_t crop_y,
    std::uint32_t size,
    const char* path_name) {
    context->CopyResource(staging, output);
    context->End(query);
    wait_for_event(context, query);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fail(std::string(path_name) + " staging map failed: " + std::to_string(hr));
    }

    bool valid = true;
    std::uint32_t mismatch_x = 0;
    std::uint32_t mismatch_y = 0;
    std::uint32_t actual = 0;
    std::uint32_t expected = 0;
    for (std::uint32_t y = 0; y < size && valid; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch);
        for (std::uint32_t x = 0; x < size; ++x) {
            expected = coordinate_pixel(crop_x + x, crop_y + y);
            if (row[x] != expected) {
                valid = false;
                mismatch_x = x;
                mismatch_y = y;
                actual = row[x];
                break;
            }
        }
    }
    context->Unmap(staging, 0);

    if (!valid) {
        fail(std::string(path_name) + " pixel mismatch at ("
            + std::to_string(mismatch_x) + "," + std::to_string(mismatch_y)
            + "): expected=" + std::to_string(expected)
            + ", actual=" + std::to_string(actual));
    }
}

double operations_per_second(
    const Measurement& measurement,
    std::uint32_t operations,
    bool completed) noexcept {
    const double duration = completed
        ? measurement.complete_seconds
        : measurement.submit_seconds;
    return static_cast<double>(operations) / duration;
}

double payload_gigabytes_per_second(
    const Measurement& measurement,
    std::uint32_t operations) noexcept {
    const double bytes = static_cast<double>(measurement.bytes_per_operation)
        * static_cast<double>(operations);
    return bytes / measurement.complete_seconds / 1'000'000'000.0;
}

double median(std::vector<double> values) {
    if (values.empty()) {
        fail("cannot summarize an empty measurement set");
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2u;
    if ((values.size() & 1u) != 0u) {
        return values[middle];
    }
    return (values[middle - 1u] + values[middle]) * 0.5;
}

Measurement median_measurement(const std::vector<Measurement>& measurements) {
    if (measurements.empty()) {
        fail("cannot summarize an empty measurement set");
    }

    std::vector<double> submit_seconds;
    std::vector<double> complete_seconds;
    submit_seconds.reserve(measurements.size());
    complete_seconds.reserve(measurements.size());
    for (const Measurement& measurement : measurements) {
        submit_seconds.push_back(measurement.submit_seconds);
        complete_seconds.push_back(measurement.complete_seconds);
    }

    return {
        median(std::move(submit_seconds)),
        median(std::move(complete_seconds)),
        measurements.front().bytes_per_operation};
}

double median_paired_speedup(
    const std::vector<Measurement>& path_a,
    const std::vector<Measurement>& path_b,
    bool completed) {
    if (path_a.size() != path_b.size() || path_a.empty()) {
        fail("paired measurement sets are inconsistent");
    }

    std::vector<double> speedups;
    speedups.reserve(path_a.size());
    for (std::size_t index = 0; index < path_a.size(); ++index) {
        const double path_a_seconds = completed
            ? path_a[index].complete_seconds
            : path_a[index].submit_seconds;
        const double path_b_seconds = completed
            ? path_b[index].complete_seconds
            : path_b[index].submit_seconds;
        speedups.push_back(path_a_seconds / path_b_seconds);
    }
    return median(std::move(speedups));
}

void print_measurement(
    const char* name,
    const char* description,
    const Measurement& measurement,
    std::uint32_t operations) {
    const double bytes = static_cast<double>(measurement.bytes_per_operation);
    std::cout << name << " - " << description << '\n'
              << "  Copied payload/op: " << measurement.bytes_per_operation
              << " bytes (" << (bytes / 1'000'000.0) << " MB)\n"
              << "  CPU submit (median batch): "
              << operations_per_second(measurement, operations, false)
              << " ops/s (" << (measurement.submit_seconds * 1'000.0) << " ms)\n"
              << "  D3D11 event-query complete (median batch): "
              << operations_per_second(measurement, operations, true)
              << " ops/s (" << (measurement.complete_seconds * 1'000.0) << " ms)\n"
              << "  Effective copied-payload bandwidth (median batch): "
              << payload_gigabytes_per_second(measurement, operations)
              << " GB/s\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.show_help) {
            std::cout
                << "Usage: fluxcap_gpu_ingress_bench [--size=320|640] "
                   "[--operations=N] [--warmup=N] [--rounds=N]\n"
                   "Runs raw D3D11 CopySubresourceRegion topology measurements; "
                   "--rounds must be at least 2.\n"
                   "Options also accept a space before their value.\n";
            return 0;
        }

        constexpr D3D_FEATURE_LEVEL feature_levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL feature_level{};
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels,
            static_cast<UINT>(sizeof(feature_levels) / sizeof(feature_levels[0])),
            D3D11_SDK_VERSION,
            &device,
            &feature_level,
            &context);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                feature_levels + 1,
                1,
                D3D11_SDK_VERSION,
                &device,
                &feature_level,
                &context);
        }
        if (FAILED(hr)) {
            fail("D3D11CreateDevice failed: " + std::to_string(hr));
        }

        auto source = create_source(device.Get());
        auto full_mailbox = create_default_texture(
            device.Get(), kSourceWidth, kSourceHeight);
        auto output_a = create_default_texture(device.Get(), options.size, options.size);
        auto output_b = create_default_texture(device.Get(), options.size, options.size);
        auto staging = create_staging_texture(device.Get(), options.size);
        auto verification_query = create_event_query(device.Get());

        const std::uint32_t crop_x = (kSourceWidth - options.size) / 2u;
        const std::uint32_t crop_y = (kSourceHeight - options.size) / 2u;
        const D3D11_BOX full_box{0, 0, 0, kSourceWidth, kSourceHeight, 1};
        const D3D11_BOX roi_box{
            crop_x,
            crop_y,
            0,
            crop_x + options.size,
            crop_y + options.size,
            1};

        const std::uint64_t full_frame_bytes =
            static_cast<std::uint64_t>(kSourceWidth) * kSourceHeight * kBytesPerPixel;
        const std::uint64_t roi_bytes =
            static_cast<std::uint64_t>(options.size) * options.size * kBytesPerPixel;
        std::vector<Measurement> path_a_samples;
        std::vector<Measurement> path_b_samples;
        path_a_samples.reserve(options.rounds);
        path_b_samples.reserve(options.rounds);
        for (std::uint32_t round = 0; round < options.rounds; ++round) {
            Measurement path_a;
            Measurement path_b;
            const bool path_a_first = (round & 1u) == 0u;
            if (path_a_first) {
                path_a = measure_path_a(
                    device.Get(),
                    context.Get(),
                    source.Get(),
                    full_mailbox.Get(),
                    output_a.Get(),
                    full_box,
                    roi_box,
                    options,
                    full_frame_bytes + roi_bytes);
                path_b = measure_path_b(
                    device.Get(),
                    context.Get(),
                    source.Get(),
                    output_b.Get(),
                    roi_box,
                    options,
                    roi_bytes);
            } else {
                path_b = measure_path_b(
                    device.Get(),
                    context.Get(),
                    source.Get(),
                    output_b.Get(),
                    roi_box,
                    options,
                    roi_bytes);
                path_a = measure_path_a(
                    device.Get(),
                    context.Get(),
                    source.Get(),
                    full_mailbox.Get(),
                    output_a.Get(),
                    full_box,
                    roi_box,
                    options,
                    full_frame_bytes + roi_bytes);
            }
            path_a_samples.push_back(path_a);
            path_b_samples.push_back(path_b);
        }

        const Measurement path_a = median_measurement(path_a_samples);
        const Measurement path_b = median_measurement(path_b_samples);

        verify_output(
            context.Get(),
            verification_query.Get(),
            output_a.Get(),
            staging.Get(),
            crop_x,
            crop_y,
            options.size,
            "path A");
        verify_output(
            context.Get(),
            verification_query.Get(),
            output_b.Get(),
            staging.Get(),
            crop_x,
            crop_y,
            options.size,
            "path B");

        const double submit_speedup = median_paired_speedup(
            path_a_samples, path_b_samples, false);
        const double complete_speedup = median_paired_speedup(
            path_a_samples, path_b_samples, true);

        std::cout << std::fixed << std::setprecision(2)
                  << "Raw D3D11 CopySubresourceRegion topology microbenchmark\n"
                  << "Synthetic source: " << kSourceWidth << 'x' << kSourceHeight
                  << " BGRA8, center ROI: " << options.size << 'x' << options.size
                  << ", operations=" << options.operations
                  << ", warmup=" << options.warmup
                  << ", rounds=" << options.rounds << "\n"
                  << "Order alternates A->B / B->A by round; reported batch times "
                     "are medians across rounds.\n"
                  << "Scope: synthetic resident textures on one D3D11 immediate "
                     "context. Operations are not WGC frames or unique captures.\n"
                  << "Not measured: WGC acquisition, FluxCap ingress/ring/mailbox "
                     "scheduling, cross-process handoff, scaling, color conversion, "
                     "encoding, or presentation.\n"
                  << "These numbers are raw copy-topology results, not an end-to-end "
                     "WGC or FluxCap ingress speedup.\n"
                  << "Payload counts each API copy once; it is not physical read+write "
                     "traffic and does not account for GPU caches.\n\n";
        print_measurement(
            "Path A",
            "full source -> full mailbox, then mailbox ROI -> fixed output",
            path_a,
            options.operations);
        std::cout << '\n';
        print_measurement(
            "Path B",
            "source ROI -> fixed output",
            path_b,
            options.operations);
        std::cout << "\nPath B speedup over A (median paired round ratio)\n"
                  << "  CPU submit: " << submit_speedup << "x\n"
                  << "  Event-query batch complete: " << complete_speedup << "x\n"
                  << "  Copied-payload reduction: "
                  << (static_cast<double>(path_a.bytes_per_operation)
                      / static_cast<double>(path_b.bytes_per_operation))
                  << "x\n"
                  << "Correctness: PASS (unique 32-bit coordinate patterns; all "
                  << options.size << 'x' << options.size
                  << " pixels verified for both paths)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "D3D11 copy-topology benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
