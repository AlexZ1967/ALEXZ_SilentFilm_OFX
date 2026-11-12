#include "silentfilm/film_optics.hpp"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void requireCuda(cudaError_t status) { if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status)); }

struct Buffer {
    unsigned char* device = nullptr;
    std::vector<silentfilm::Pixel> host;
    int width, height, x, y;
    bool reverse;
    Buffer(int w, int h, int xx, int yy, bool negative)
        : host(static_cast<std::size_t>(w + 3) * h, {-7, -7, -7, -7}), width(w), height(h), x(xx), y(yy), reverse(negative) {
        requireCuda(cudaMalloc(reinterpret_cast<void**>(&device), host.size() * sizeof(silentfilm::Pixel)));
    }
    ~Buffer() { if (device) cudaFree(device); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    silentfilm::ImageView view(bool gpu) {
        auto* base = gpu ? device : reinterpret_cast<unsigned char*>(host.data());
        const auto stride = static_cast<std::ptrdiff_t>(width + 3) * static_cast<std::ptrdiff_t>(sizeof(silentfilm::Pixel));
        return {reverse ? base + (height - 1) * stride : base, reverse ? -stride : stride, x, y, x + width, y + height};
    }
};

/** Compare cuFFT and FFTW optics including plan reuse, unusual FFT lengths and transported alpha. */
void run(int width, int height, bool suppliedStream, bool reverse)
{
    Buffer source(width, height, -3, 5, reverse), output(width + 4, height + 4, -5, 3, !reverse);
    auto sourceView = source.view(false);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const float value = static_cast<float>((x * 7 + y * 3) % 17) / 16;
        const float alpha = static_cast<float>((x + y) % 5) / 4;
        silentfilm::Pixel pixel{value, 1 - value, 0.25f + value * 0.5f, alpha};
        if (reverse) { pixel.r *= alpha; pixel.g *= alpha; pixel.b *= alpha; }
        std::memcpy(sourceView.data + y * sourceView.rowBytes + x * sizeof(pixel), &pixel, sizeof(pixel));
    }
    cudaStream_t stream = nullptr;
    if (suppliedStream) requireCuda(cudaStreamCreate(&stream));
    try {
        // Cache уничтожается до stream; исходный context потока восстанавливается после cleanup.
        silentfilm::ProjectorCudaCache cache;
        requireCuda(cudaMemcpyAsync(source.device, source.host.data(), source.host.size() * sizeof(silentfilm::Pixel), cudaMemcpyHostToDevice, stream));
        for (auto transfer : {silentfilm::Transfer::Linear, silentfilm::Transfer::SRGB, silentfilm::Transfer::Gamma24}) {
            for (float glow : {0.0f, 0.25f, 1.0f}) {
                std::fill(output.host.begin(), output.host.end(), silentfilm::Pixel{-7, -7, -7, -7});
                auto expected = output.host;
                auto cpuOutput = output.view(false);
                const auto offset = cpuOutput.data - reinterpret_cast<unsigned char*>(output.host.data());
                cpuOutput.data = reinterpret_cast<unsigned char*>(expected.data()) + offset;
                requireCuda(cudaMemcpyAsync(output.device, output.host.data(), output.host.size() * sizeof(silentfilm::Pixel), cudaMemcpyHostToDevice, stream));
                silentfilm::ProjectorParameters p;
                p.transfer = transfer; p.glowStrength = glow; p.glowSize = glow == 1 ? 5 : 1;
                p.frame = {reverse ? -0.73f : 0.47f, 0.35f, glow == 1 ? 2.1f : 0.87f};
                p.sourcePremultiplied = reverse; p.outputPremultiplied = !reverse;
                const silentfilm::RenderWindow window{output.x + 1, output.y + 1,
                    output.x + output.width - 1, output.y + output.height - 1};
                if (!silentfilm::renderProjectorCpu(sourceView, cpuOutput, window, p, nullptr, nullptr))
                    throw std::runtime_error("Projector CPU reference failed");
                const auto status = cache.render(source.view(true), output.view(true), window, p, stream, suppliedStream);
                if (status.code) throw std::runtime_error(status.message ? status.message : "Projector GPU failed");
                requireCuda(cudaMemcpyAsync(output.host.data(), output.device, output.host.size() * sizeof(silentfilm::Pixel), cudaMemcpyDeviceToHost, stream));
                requireCuda(cudaStreamSynchronize(stream));
                for (std::size_t i = 0; i < expected.size(); ++i) {
                    const auto a = expected[i], b = output.host[i];
                    const std::array<float, 4> actual{b.r, b.g, b.b, b.a}, reference{a.r, a.g, a.b, a.a};
                    for (int c = 0; c < 4; ++c) if (!std::isfinite(actual[c]) || std::abs(actual[c] - reference[c]) > 1.5e-4f)
                        throw std::runtime_error("Projector CPU/CUDA mismatch: width=" + std::to_string(width)
                            + " pixel=" + std::to_string(i) + " channel=" + std::to_string(c));
                }
            }
        }
    } catch (...) {
        cudaStreamSynchronize(stream);
        if (suppliedStream) cudaStreamDestroy(stream);
        throw;
    }
    if (suppliedStream) requireCuda(cudaStreamDestroy(stream));
}

/** Exercise bounded plan ownership, purge/rebuild and cleanup after the host destroys its stream. */
void lifecycle()
{
    const auto check = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto driver = [](CUresult status) { if (status != CUDA_SUCCESS) throw std::runtime_error("CUDA context lifecycle call failed"); };
    Buffer source(37, 23, 0, 0, false), output(37, 23, 0, 0, false);
    struct Stream {
        cudaStream_t value = nullptr;
        Stream() { requireCuda(cudaStreamCreate(&value)); }
        ~Stream() { if (value) cudaStreamDestroy(value); }
        void destroy() { requireCuda(cudaStreamDestroy(value)); value = nullptr; }
    } stream;
    requireCuda(cudaMemsetAsync(source.device, 0, source.host.size() * sizeof(silentfilm::Pixel), stream.value));
    silentfilm::ProjectorCudaCache cache;
    silentfilm::ProjectorParameters p;
    p.transfer = silentfilm::Transfer::Linear; p.glowStrength = 0.5f;
    const auto render = [&](silentfilm::ProjectorCudaCache& target, int width) {
        auto input = source.view(true), destination = output.view(true);
        input.x2 = destination.x2 = width;
        const auto status = target.render(input, destination, {0, 0, width, 23}, p, stream.value, true);
        check(!status.code, status.message ? status.message : "Lifecycle render failed");
    };
    CUcontext original = nullptr, after = nullptr;
    driver(cuCtxGetCurrent(&original));
    for (std::size_t i = 0; i < silentfilm::ProjectorCudaCache::maximumPlanSets + 4; ++i) {
        render(cache, 7 + static_cast<int>(i));
        check(cache.cachedPlanCount() <= silentfilm::ProjectorCudaCache::maximumPlanSets, "cuFFT cache grew beyond its limit");
    }
    check(cache.cachedPlanCount() == silentfilm::ProjectorCudaCache::maximumPlanSets, "Lifecycle fixture did not fill the plan cache");
    driver(cuCtxGetCurrent(&after));
    check(after == original, "Eviction changed the caller CUDA context");

    // DestroyInstance/PurgeCaches могут прийти на потоке без текущего CUDA context.
    CUcontext removed = nullptr;
    driver(cuCtxPopCurrent(&removed));
    struct RestoreContext {
        CUcontext value;
        ~RestoreContext() { if (value) cuCtxPushCurrent(value); }
    } restore{removed};
    const auto cleared = cache.clear();
    check(!cleared.code, cleared.message ? cleared.message : "Purge failed without a current context");
    driver(cuCtxGetCurrent(&after));
    check(after == nullptr, "Purge failed to restore an empty caller context stack");
    driver(cuCtxPushCurrent(removed)); restore.value = nullptr;
    check(cache.cachedPlanCount() == 0, "Purge retained FFT plans");
    check(!cache.clear().code, "Repeated purge is not idempotent");
    render(cache, 17);
    check(cache.cachedPlanCount() == 1, "Render after purge failed to rebuild plans");
    auto pending = std::make_unique<silentfilm::ProjectorCudaCache>();
    render(*pending, 19);
    // Без предварительной синхронизации: cleanup использует своё событие, не мёртвый stream.
    stream.destroy();
    check(!cache.clear().code, "Purge used a destroyed host stream");
    pending.reset();
    driver(cuCtxGetCurrent(&after));
    check(after == original, "Destruction changed the caller CUDA context");
}

} // namespace

int main()
{
    try {
        int count = 0;
        const auto status = cudaGetDeviceCount(&count);
        if (status == cudaErrorNoDevice || (status == cudaSuccess && count == 0)) {
            std::cout << "Projector CUDA parity: SKIP (no CUDA device)\n"; return 77;
        }
        requireCuda(status);
        lifecycle();
        for (bool supplied : {false, true}) for (bool reverse : {false, true}) {
            run(1, 1, supplied, reverse); run(7, 5, supplied, reverse);
            run(33, 19, supplied, reverse); run(257, 43, supplied, reverse);
        }
        std::cout << "Projector CPU/CUDA parity: PASS\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
