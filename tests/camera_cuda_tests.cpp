#include "silentfilm/camera_capture.hpp"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void requireCuda(cudaError_t status)
{
    if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}

struct Image {
    std::vector<silentfilm::Pixel> pixels;
    unsigned char* device = nullptr;
    int width, height, x1, y1;
    bool reverse;

    Image(int w, int h, int x, int y, bool negative)
        : pixels(static_cast<std::size_t>(w + 3) * h, {-7, -7, -7, -7}),
          width(w), height(h), x1(x), y1(y), reverse(negative)
    {
        requireCuda(cudaMalloc(reinterpret_cast<void**>(&device), pixels.size() * sizeof(silentfilm::Pixel)));
    }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image() { if (device) cudaFree(device); }

    silentfilm::ImageView view(bool gpu = false) {
        auto* base = gpu ? device : reinterpret_cast<unsigned char*>(pixels.data());
        const auto stride = static_cast<std::ptrdiff_t>(width + 3) * static_cast<std::ptrdiff_t>(sizeof(silentfilm::Pixel));
        return {reverse ? base + (height - 1) * stride : base, reverse ? -stride : stride,
            x1, y1, x1 + width, y1 + height};
    }
    void upload(cudaStream_t stream) {
        requireCuda(cudaMemcpyAsync(device, pixels.data(), pixels.size() * sizeof(silentfilm::Pixel), cudaMemcpyHostToDevice, stream));
    }
};

/** Exercise full optics, reduced Gaussian levels, alpha, temporal interpolation and buffer boundaries. */
void runCase(int width, int height, int mode, bool hostStream)
{
    const bool reverse = mode % 2 != 0;
    Image first(width, height, -3, 4, reverse), second(width + 1, height + 2, -2, 3, !reverse);
    Image output(width + 6, height + 4, -5, 2, !reverse);
    auto expected = output.pixels;
    silentfilm::CameraOpticsParameters optics;
    optics.sourcePremultiplied = mode % 3 == 0;
    optics.outputPremultiplied = mode % 2 == 0;
    optics.edgeStart = 0.25f; optics.edgeEnd = 0.95f;
    optics.vignetteStrength = mode == 0 ? 0 : 0.3f;
    optics.edgeSoftness = mode >= 4 ? 0.9f : 0;
    optics.softness = mode >= 2 && mode != 6 ? 0.7f : 0;
    optics.focusDrift = mode >= 3 ? 0.8f : 0;
    optics.focusSignal = mode == 5 ? -1.0f : 0.3f;
    for (auto* image : {&first, &second}) {
        const auto view = image->view();
        for (int y = view.y1; y < view.y2; ++y) {
            for (int x = view.x1; x < view.x2; ++x) {
                const float alpha = static_cast<float>((x - view.x1 + y - view.y1) % 5) / 4;
                const float level = static_cast<float>((x * 7 + y * 13 + 10000) % 31) / 30;
                silentfilm::Pixel value{level, 0.2f + level * 0.6f, 1 - level, alpha};
                if (optics.sourcePremultiplied) { value.r *= alpha; value.g *= alpha; value.b *= alpha; }
                auto* address = view.data + (y - view.y1) * view.rowBytes + (x - view.x1) * sizeof(value);
                std::memcpy(address, &value, sizeof(value));
            }
        }
    }
    cudaStream_t stream = nullptr;
    if (hostStream) requireCuda(cudaStreamCreate(&stream));
    try {
        first.upload(stream); second.upload(stream); output.upload(stream);
        std::array<silentfilm::CameraExposureSample, 9> cpu{}, gpu{};
        std::array<silentfilm::CameraSampleTransform, 9> transforms{};
        const int sampleCount = mode == 0 ? 1 : 9;
        const float shake = mode == 0 ? 0 : 0.37f;
        for (int i = 0; i < sampleCount; ++i) {
            const float phase = 12.1f + i * 0.013f;
            const float mix = i % 3 == 0 ? 0 : static_cast<float>(i) / 9;
            const float weight = std::max(1e-4f, 1 - std::abs(static_cast<float>(i) / 8 - 0.5f) * 2);
            cpu[i] = {first.view(), phase, second.view(), mix, weight};
            gpu[i] = {first.view(true), phase, second.view(true), mix, weight};
            transforms[i] = silentfilm::cameraSampleTransform(phase, shake, 1925, width, height);
        }
        const auto outputView = output.view();
        auto expectedView = outputView;
        const auto offset = outputView.data - reinterpret_cast<unsigned char*>(output.pixels.data());
        expectedView.data = reinterpret_cast<unsigned char*>(expected.data()) + offset;
        const silentfilm::RenderWindow window{output.x1 + 1, output.y1 + 1,
            output.x1 + output.width - 1, output.y1 + output.height - 1};
        if (!silentfilm::renderCameraCaptureCpu(cpu.data(), sampleCount, expectedView, window,
            optics, 1.13f, shake, 1925, nullptr, nullptr)) throw std::runtime_error("CPU Camera reference failed");
        const auto status = silentfilm::renderCudaCamera(gpu.data(), transforms.data(), sampleCount,
            output.view(true), window, optics, 1.13f, shake > 0, stream, hostStream);
        if (status.code) throw std::runtime_error(status.message ? status.message : "CUDA Camera failed");
        requireCuda(cudaMemcpyAsync(output.pixels.data(), output.device, output.pixels.size() * sizeof(silentfilm::Pixel), cudaMemcpyDeviceToHost, stream));
        requireCuda(cudaStreamSynchronize(stream));
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const std::array<float, 4> a{expected[i].r, expected[i].g, expected[i].b, expected[i].a};
            const std::array<float, 4> b{output.pixels[i].r, output.pixels[i].g, output.pixels[i].b, output.pixels[i].a};
            for (int c = 0; c < 4; ++c) {
                if (!std::isfinite(b[c]) || std::abs(a[c] - b[c]) > 8e-5f) {
                    throw std::runtime_error("Camera CPU/CUDA mismatch: mode=" + std::to_string(mode)
                        + " width=" + std::to_string(width) + " pixel=" + std::to_string(i)
                        + " channel=" + std::to_string(c) + " CPU=" + std::to_string(a[c])
                        + " GPU=" + std::to_string(b[c]));
                }
            }
        }
    } catch (...) {
        cudaStreamSynchronize(stream);
        if (hostStream) cudaStreamDestroy(stream);
        throw;
    }
    if (hostStream) requireCuda(cudaStreamDestroy(stream));
}

} // namespace

int main()
{
    try {
        int count = 0;
        const auto status = cudaGetDeviceCount(&count);
        if (status == cudaErrorNoDevice || (status == cudaSuccess && count == 0)) {
            std::cout << "Camera CUDA parity: SKIP (no CUDA device)\n";
            return 77;
        }
        requireCuda(status);
        for (bool hostStream : {false, true}) {
            for (int mode = 0; mode <= 6; ++mode) {
                runCase(7, 5, mode, hostStream);
                runCase(257, 43, mode, hostStream);
            }
            runCase(1, 1, 4, hostStream);
            runCase(73, 2, 4, hostStream);
        }
        std::cout << "Camera CPU/CUDA parity: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
