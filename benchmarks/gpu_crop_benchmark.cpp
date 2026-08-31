#include <fluxcap/gpu.hpp>

#include <windows.h>
#include <wrl/client.h>

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
namespace gpu = fluxcap::gpu;

struct Options final {
    std::uint32_t size = 320;
    std::uint32_t operations = 25'000;
    std::uint32_t warmup = 2'000;
    bool show_help = false;
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
            continue;
        }

        if (argument == L"--size" || argument.starts_with(L"--size=")) {
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
    if (options.operations > 100'000 || options.warmup > 100'000) {
        throw std::invalid_argument("--operations and --warmup must not exceed 100000");
    }
    return options;
}

ComPtr<ID3D11Texture2D> create_source(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height) {
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t blue = x & 0xffu;
            const std::uint32_t green = y & 0xffu;
            const std::uint32_t red = (x ^ y) & 0xffu;
            pixels[static_cast<std::size_t>(y) * width + x] =
                blue | (green << 8u) | (red << 16u) | 0xff000000u;
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
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial_data{};
    initial_data.pSysMem = pixels.data();
    initial_data.SysMemPitch = width * sizeof(std::uint32_t);

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(
        &description, &initial_data, &texture);
    if (FAILED(hr)) {
        fail("source texture creation failed: " + std::to_string(hr));
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
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    for (;;) {
        BOOL complete = FALSE;
        const HRESULT hr = context->GetData(
            query, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && complete != FALSE) return;
        if (FAILED(hr)) {
            fail("D3D11 event query read failed: " + std::to_string(hr));
        }
        if (Clock::now() >= deadline) {
            fail("D3D11 event query timed out after 30 seconds");
        }
        std::this_thread::yield();
    }
}

double seconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double>(duration).count();
}

void submit_crops(
    gpu::GpuCrop& crop,
    ID3D11Texture2D* source,
    std::uint32_t operations,
    const char* phase) {
    for (std::uint32_t index = 0; index < operations; ++index) {
        const gpu::GpuError result = crop.process(source);
        if (!result) {
            fail(std::string(phase) + " crop failed: " + result.what());
        }
    }
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        constexpr std::uint32_t source_width = 2560;
        constexpr std::uint32_t source_height = 1600;
        const Options options = parse_options(argc, argv);
        if (options.show_help) {
            std::cout
                << "Usage: fluxcap_gpu_crop_bench [--size=320|640] "
                   "[--operations=N] [--warmup=N]\n"
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

        auto source = create_source(device.Get(), source_width, source_height);
        gpu::GpuCropConfig crop_config;
        crop_config.input_width = source_width;
        crop_config.input_height = source_height;
        crop_config.width = options.size;
        crop_config.height = options.size;
        crop_config.x = (source_width - options.size) / 2u;
        crop_config.y = (source_height - options.size) / 2u;
        crop_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        crop_config.bind_flags = D3D11_BIND_SHADER_RESOURCE;

        gpu::GpuCrop crop;
        const gpu::GpuError created = gpu::GpuCrop::create(
            device.Get(), crop_config, crop);
        if (!created) {
            fail(std::string("GPU crop creation failed: ") + created.what());
        }

        auto event_query = create_event_query(device.Get());
        submit_crops(crop, source.Get(), options.warmup, "warmup");
        context->End(event_query.Get());
        wait_for_event(context.Get(), event_query.Get());

        const auto batch_begin = Clock::now();
        submit_crops(crop, source.Get(), options.operations, "measured");
        const auto submit_end = Clock::now();
        context->End(event_query.Get());
        wait_for_event(context.Get(), event_query.Get());
        const auto batch_complete = Clock::now();

        const double submit_seconds = seconds(submit_end - batch_begin);
        const double complete_seconds = seconds(batch_complete - batch_begin);
        const double operation_count = static_cast<double>(options.operations);
        const double payload_bytes = operation_count
            * static_cast<double>(options.size)
            * static_cast<double>(options.size)
            * 4.0;
        const double decimal_gigabytes = payload_bytes / 1'000'000'000.0;
        const double gibibytes = payload_bytes / (1024.0 * 1024.0 * 1024.0);

        std::cout << std::fixed << std::setprecision(2)
                  << "FluxCap GpuCrop saturation benchmark\n"
                  << "Source: " << source_width << 'x' << source_height
                  << " BGRA8, crop: " << options.size << 'x' << options.size
                  << ", operations=" << options.operations
                  << ", warmup=" << options.warmup << "\n"
                  << "Synthetic source and one output texture are reused; operations are "
                     "not unique captured frames.\n\n"
                  << "CPU submit: " << (operation_count / submit_seconds)
                  << " ops/s (" << (submit_seconds * 1'000.0) << " ms)\n"
                  << "GPU batch complete (D3D11 event query): "
                  << (operation_count / complete_seconds)
                  << " ops/s (" << (complete_seconds * 1'000.0) << " ms)\n"
                  << "Effective copied payload bandwidth: "
                  << (decimal_gigabytes / complete_seconds) << " GB/s ("
                  << (gibibytes / complete_seconds) << " GiB/s)\n"
                  << "Copied payload: " << decimal_gigabytes << " GB; generation="
                  << crop.generation() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU crop benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
