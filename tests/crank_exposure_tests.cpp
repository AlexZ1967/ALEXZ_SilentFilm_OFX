#include "silentfilm/crank_exposure.hpp"
#include "silentfilm/camera_capture.hpp"

#ifdef SILENTFILM_TEST_CUDA
#include <cuda_runtime_api.h>
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace silentfilm;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F action) {
    bool caught = false;
    try { action(); } catch (const std::runtime_error&) { caught = true; }
    require(caught, "Invalid exposure map/timing was accepted");
}

void maps(const std::filesystem::path& path)
{
    const std::vector<float> gains{0.5f, 1, 1.5f, 2};
    writeCrankExposureMap(path.string(), 25, gains, false);
    CrankExposureMapCache cache;
    const auto first = cache.load(path.string());
    require(first->gains == gains, "Gain map round trip lost precision");
    require(first->at(3, 0, 25) == 2 && first->at(0, 1, 25) == 1 && first->at(1, -1, 25) == 0.5f,
        "Frame/trim offset indexing is incorrect");
    rejected([&] { first->at(0, 0, 50); });
    rejected([&] { first->at(4, 0, 25); });
    rejected([&] { first->at(0, -1, 25); });
    writeCrankExposureMap(path.string(), 25, {2, 1, 0.5f}, false);
    require(cache.load(path.string()) == first, "Map changed under an active render");
    cache.clear();
    require(cache.load(path.string())->gains.size() == 3 && first->gains == gains, "Purge invalidated an active map snapshot");
    writeCrankExposureMap(path.string(), 25, gains, true);
    rejected([&] { readCrankExposureMap(path.string()); });
    for (const char* text : {"SILENTFILM_CRANK_EXPOSURE 2\nofx 25 1\n0 1\n",
         "SILENTFILM_CRANK_EXPOSURE 1\nofx 25 2\n0 1\n",
         "SILENTFILM_CRANK_EXPOSURE 1\nofx 25 1\n1 1\n",
         "SILENTFILM_CRANK_EXPOSURE 1\nofx 25 1\n0 -1\n",
         "SILENTFILM_CRANK_EXPOSURE 1\nofx 25 1\n0 1\nextra"}) {
        { std::ofstream out(path); out << text; }
        rejected([&] { readCrankExposureMap(path.string()); });
    }
    rejected([] { readCrankExposureMap(""); });
}

ImageView view(std::vector<Pixel>& pixels, int width, int height)
{
    return {reinterpret_cast<unsigned char*>(pixels.data()), static_cast<std::ptrdiff_t>(width * sizeof(Pixel)),
        0, 0, width, height};
}

/** Compare post-capture exposure against the original bake renderer, not just the shared pixel helper. */
void pixels()
{
    constexpr int width = 7, height = 5;
    const RenderWindow window{0, 0, width, height};
    std::vector<Pixel> source(width * height), captured(source.size()), expected(source.size()), actual(source.size());
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = {static_cast<float>(i % 11) / 10, 0.18f, 0.7f, static_cast<float>(i % 5) / 4};
    CameraOpticsParameters optics;
    optics.captureOnly = true;
    const CameraExposureSample sample{view(source, width, height), 0, {}, 0, 1};
    require(renderCameraCaptureCpu(&sample, 1, view(captured, width, height), window, optics, 1, 0, 1925, nullptr, nullptr), "Capture failed");
#ifdef SILENTFILM_TEST_CUDA
    const auto check = [](cudaError_t status) { if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status)); };
    struct Device {
        Pixel* input = nullptr; Pixel* output = nullptr; cudaStream_t stream = nullptr;
        ~Device() { cudaStreamSynchronize(stream); if (input) cudaFree(input); if (output) cudaFree(output); if (stream) cudaStreamDestroy(stream); }
    } device;
    check(cudaStreamCreate(&device.stream));
    check(cudaMalloc(reinterpret_cast<void**>(&device.input), captured.size() * sizeof(Pixel)));
    check(cudaMalloc(reinterpret_cast<void**>(&device.output), captured.size() * sizeof(Pixel)));
    check(cudaMemcpyAsync(device.input, captured.data(), captured.size() * sizeof(Pixel), cudaMemcpyHostToDevice, device.stream));
#endif
    for (float gain : {0.5f, 1.0f, 2.0f}) for (float blend : {0.0f, 0.25f, 0.5f, 1.0f}) {
        require(renderCameraCaptureCpu(&sample, 1, view(expected, width, height), window, optics,
            1 + blend * (gain - 1), 0, 1925, nullptr, nullptr), "Bake reference failed");
        require(renderCrankExposureCpu(view(captured, width, height), view(actual, width, height), window,
            gain, blend, false, false, nullptr, nullptr), "Exposure CPU failed");
        const auto compare = [&] {
            for (std::size_t i = 0; i < actual.size(); ++i) {
                const std::array<float, 4> a{actual[i].r, actual[i].g, actual[i].b, actual[i].a};
                const std::array<float, 4> b{expected[i].r, expected[i].g, expected[i].b, expected[i].a};
                for (int c = 0; c < 4; ++c) require(std::isfinite(a[c]) && std::abs(a[c] - b[c]) < 3e-6f,
                    "Post-capture exposure differs from the original bake");
                require(actual[i].a == captured[i].a, "Exposure changed alpha");
            }
        };
        compare();
        if (blend == 0) require(std::memcmp(actual.data(), captured.data(), actual.size() * sizeof(Pixel)) == 0, "Blend0 is not exact bypass");
#ifdef SILENTFILM_TEST_CUDA
        auto input = view(captured, width, height), output = view(actual, width, height);
        input.data = reinterpret_cast<unsigned char*>(device.input); output.data = reinterpret_cast<unsigned char*>(device.output);
        const auto status = renderCudaCrankExposure(input, output, window, gain, blend, false, false, device.stream, true);
        require(!status.code, "CUDA exposure failed");
        check(cudaMemcpyAsync(actual.data(), device.output, actual.size() * sizeof(Pixel), cudaMemcpyDeviceToHost, device.stream));
        check(cudaStreamSynchronize(device.stream));
        compare();
#endif
    }
    for (bool inputPremult : {false, true}) for (bool outputPremult : {false, true}) {
        Pixel input{0.5f, 0.25f, 0.7f, 0.4f};
        if (inputPremult) { input.r *= input.a; input.g *= input.a; input.b *= input.a; }
        const auto output = crankExposurePixel(input, 2, 0.5f, inputPremult, outputPremult);
        const float multiplier = outputPremult ? input.a : 1;
        require(std::abs(output.r - crankExposurePixel({0.5f, 0.25f, 0.7f, 0.4f}, 2, 0.5f, false, false).r * multiplier) < 1e-6f
            && output.a == input.a, "Premultiplication changed exposure/alpha");
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
#ifdef SILENTFILM_TEST_CUDA
        int count = 0; const auto status = cudaGetDeviceCount(&count);
        if (status == cudaErrorNoDevice || (status == cudaSuccess && count == 0)) return 77;
        require(status == cudaSuccess, "CUDA query failed");
#endif
        require(argc == 2, "Pass a test output directory");
        const auto path = std::filesystem::path(argv[1]) / ("crank-map-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sfexposure");
        std::filesystem::create_directories(path.parent_path());
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
        maps(path); pixels();
        std::cout << "Crank Exposure map/bake/CPU/CUDA: PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
