#include "silentfilm/cuda_backend.hpp"
#include "silentfilm/probe.hpp"

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

using silentfilm::ImageView;
using silentfilm::Pixel;

/** Fail the standalone GPU test with the actual CUDA runtime diagnostic. */
void requireCuda(cudaError_t status)
{
    if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}

struct Buffer {
    unsigned char* data = nullptr;

    /** Allocate a small test-owned device buffer; production rendering allocates none. */
    explicit Buffer(std::size_t size)
    {
        void* pointer = nullptr;
        requireCuda(cudaMalloc(&pointer, size));
        data = static_cast<unsigned char*>(pointer);
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    /** Release the test allocation after the test stream has completed. */
    ~Buffer() { if (data) cudaFree(data); }
};

struct Stream {
    cudaStream_t value = nullptr;

    /** Create a test-owned stream to emulate the stream supplied by a host. */
    Stream() { requireCuda(cudaStreamCreate(&value)); }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /** Destroy the private test stream without modifying Resolve state. */
    ~Stream() { if (value) cudaStreamDestroy(value); }
};

/** Build matching host/device views with nonzero origins and optional negative strides. */
ImageView view(unsigned char* data, int x, int y, bool reverse)
{
    constexpr std::ptrdiff_t stride = 10 * sizeof(Pixel);
    return {reverse ? data + 4 * stride : data, reverse ? -stride : stride, x, y, x + 7, y + 5};
}

/** Read the CPU reference using the logical bounds of each independent input. */
Pixel sample(ImageView image, int x, int y)
{
    Pixel result{};
    if (x < image.x1 || x >= image.x2 || y < image.y1 || y >= image.y2) return result;
    const auto* address = image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel);
    std::memcpy(&result, address, sizeof(result));
    return result;
}

/** Compare padded device buffers including untouched pixels outside the render window. */
void runCase(bool reverse, bool suppliedStream, float mix, const silentfilm::EmulsionParameters* film = nullptr)
{
    constexpr std::size_t count = 50;
    constexpr std::size_t bytes = count * sizeof(Pixel);
    std::vector<Pixel> current(count), previous(count), next(count);
    std::vector<Pixel> expected(count, Pixel{-77, -77, -77, -77}), actual(count);
    for (std::size_t index = 0; index < count; ++index) {
        const float value = static_cast<float>(index) / 20;
        current[index] = {value, -value, value * 2, 0.4f};
        previous[index] = {value + 1, value * 0.5f, value, 0.2f};
        next[index] = {value - 0.3f, value * 2, -value, 0.8f};
        if (film) {
            const float level = static_cast<float>(index) / (count - 1);
            current[index] = {level, level * 0.7f, 0.1f + level * 0.9f, static_cast<float>(index % 5) / 4};
            if (film->stage == silentfilm::FilmStage::Print) {
                silentfilm::EmulsionParameters negative;
                negative.transfer = film->transfer;
                negative.outputPremultiplied = film->sourcePremultiplied;
                current[index] = silentfilm::emulsionPixel(current[index], negative);
            } else if (film->sourcePremultiplied) {
                current[index].r *= current[index].a;
                current[index].g *= current[index].a;
                current[index].b *= current[index].a;
            }
        }
    }
    const int sourceX = film ? 4 : 5, sourceY = film ? 6 : 7;
    const auto cpuCurrent = view(reinterpret_cast<unsigned char*>(current.data()), sourceX, sourceY, reverse);
    const auto cpuPrevious = view(reinterpret_cast<unsigned char*>(previous.data()), 6, 8, reverse);
    const auto cpuNext = view(reinterpret_cast<unsigned char*>(next.data()), 4, 6, reverse);
    const auto cpuOutput = view(reinterpret_cast<unsigned char*>(expected.data()), 5, 7, reverse);
    const silentfilm::RenderWindow window{6, 8, 12, 12};
    const float gain = silentfilm::gainFromEV(0.75);
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const bool sourcePresent = x >= cpuCurrent.x1 && x < cpuCurrent.x2 && y >= cpuCurrent.y1 && y < cpuCurrent.y2;
            const auto value = film
                ? (sourcePresent ? silentfilm::emulsionPixel(sample(cpuCurrent, x, y), *film) : Pixel{})
                : silentfilm::probePixel(sample(cpuCurrent, x, y), sample(cpuPrevious, x, y), sample(cpuNext, x, y), gain, mix);
            auto* destination = cpuOutput.data + (y - cpuOutput.y1) * cpuOutput.rowBytes + (x - cpuOutput.x1) * sizeof(Pixel);
            std::memcpy(destination, &value, sizeof(value));
        }
    }
    Buffer gpuCurrent(bytes), gpuPrevious(bytes), gpuNext(bytes), gpuOutput(bytes);
    const std::vector<Pixel> initial(count, Pixel{-77, -77, -77, -77});
    requireCuda(cudaMemcpy(gpuCurrent.data, current.data(), bytes, cudaMemcpyHostToDevice));
    requireCuda(cudaMemcpy(gpuPrevious.data, previous.data(), bytes, cudaMemcpyHostToDevice));
    requireCuda(cudaMemcpy(gpuNext.data, next.data(), bytes, cudaMemcpyHostToDevice));
    requireCuda(cudaMemcpy(gpuOutput.data, initial.data(), bytes, cudaMemcpyHostToDevice));
    Stream stream;
    void* queue = suppliedStream ? static_cast<void*>(stream.value) : nullptr;
    const auto status = film
        ? silentfilm::renderCudaEmulsion(view(gpuCurrent.data, sourceX, sourceY, reverse), view(gpuOutput.data, 5, 7, reverse), window, *film, queue, suppliedStream)
        : silentfilm::renderCudaProbe(view(gpuCurrent.data, sourceX, sourceY, reverse), view(gpuPrevious.data, 6, 8, reverse),
            view(gpuNext.data, 4, 6, reverse), view(gpuOutput.data, 5, 7, reverse), window, gain, mix, queue, suppliedStream);
    if (status.code != 0) throw std::runtime_error(status.message);
    // Только тест ждёт собственный stream для сравнения; host render этого не делает.
    if (suppliedStream) requireCuda(cudaStreamSynchronize(stream.value));
    requireCuda(cudaMemcpy(actual.data(), gpuOutput.data, bytes, cudaMemcpyDeviceToHost));
    for (std::size_t index = 0; index < count; ++index) {
        const float got[] = {actual[index].r, actual[index].g, actual[index].b, actual[index].a};
        const float wanted[] = {expected[index].r, expected[index].g, expected[index].b, expected[index].a};
        for (int channel = 0; channel < 4; ++channel) {
            const float tolerance = film && channel == 3 ? 0.0f
                : (film && (film->stage == silentfilm::FilmStage::Print || film->previewPositive) ? 5e-4f : 1e-5f);
            if (!std::isfinite(got[channel]) || std::abs(got[channel] - wanted[channel]) > tolerance) {
                std::ostringstream message;
                message << "CPU/CUDA mismatch: pixel=" << index << " channel=" << channel << " got=" << got[channel]
                    << " expected=" << wanted[channel] << " reverse=" << reverse << " hostStream=" << suppliedStream;
                if (film) message << " filmStage=" << static_cast<int>(film->stage) << " transfer=" << static_cast<int>(film->transfer)
                    << " contrast=" << film->contrast << " preview=" << film->previewPositive;
                throw std::runtime_error(message.str());
            }
        }
        if (film && (film->stage == silentfilm::FilmStage::Print || film->previewPositive) && expected[index].a > 0) {
            const float divisor = film->outputPremultiplied ? expected[index].a : 1;
            const float actualLinear = silentfilm::film::decode(actual[index].r / divisor, film->transfer);
            const float expectedLinear = silentfilm::film::decode(expected[index].r / divisor, film->transfer);
            if (std::abs(actualLinear - expectedLinear) > 2e-5f) throw std::runtime_error("CPU/CUDA film mismatch in linear light");
        }
    }
}

} // namespace

int main()
{
    try {
        int count = 0;
        const auto status = cudaGetDeviceCount(&count);
        if (status == cudaErrorNoDevice || (status == cudaSuccess && count == 0)) {
            std::cout << "CUDA parity: SKIP (no CUDA device)\n";
            return 77;
        }
        requireCuda(status);
        for (const bool reverse : {false, true}) {
            for (const bool suppliedStream : {false, true}) {
                for (const float mix : {0.0f, 0.35f, 1.0f}) runCase(reverse, suppliedStream, mix);
                for (const auto transfer : {silentfilm::Transfer::Linear, silentfilm::Transfer::SRGB, silentfilm::Transfer::Gamma24}) {
                    for (const auto stage : {silentfilm::FilmStage::Negative, silentfilm::FilmStage::Print}) {
                        silentfilm::EmulsionParameters film;
                        film.stage = stage;
                        film.transfer = transfer;
                        runCase(reverse, suppliedStream, 0, &film);
                        film.exposureEV = 0.3f; film.contrast = 1.2f; film.shadowSoftness = 0.4f;
                        film.highlightSoftness = 0.7f; film.fog = 0.12f;
                        film.sourcePremultiplied = reverse;
                        film.outputPremultiplied = suppliedStream;
                        runCase(reverse, suppliedStream, 0, &film);
                        if (stage == silentfilm::FilmStage::Negative) {
                            film.previewPositive = true;
                            runCase(reverse, suppliedStream, 0, &film);
                        }
                    }
                }
            }
        }
        std::cout << "SilentFilm CPU/CUDA parity: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
