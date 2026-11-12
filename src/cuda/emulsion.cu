#include <cuda_runtime.h>

#include "silentfilm/film_optics.hpp"
#include "silentfilm/grain.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace silentfilm {
namespace {

__device__ Pixel* address(ImageView image, int x, int y)
{
    return reinterpret_cast<Pixel*>(image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel));
}

__device__ float incident(Pixel value, EmulsionParameters p)
{
    if (p.sourcePremultiplied) {
        if (value.a <= 0) value.r = value.g = value.b = 0;
        else { value.r /= value.a; value.g /= value.a; value.b /= value.a; }
    }
    if (p.stage == FilmStage::Print) return value.r * 0.299f + value.g * 0.587f + value.b * 0.114f;
    return film::decode(value.r, p.transfer) * 0.2126f + film::decode(value.g, p.transfer) * 0.7152f
        + film::decode(value.b, p.transfer) * 0.0722f;
}

__global__ void prepare(ImageView source, Pixel* output, int width, int height, EmulsionParameters p)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= static_cast<std::size_t>(width) * height) return;
    const auto rgb = *address(source, source.x1 + static_cast<int>(i % width), source.y1 + static_cast<int>(i / width));
    const float value = incident(rgb, p);
    output[i] = {value, value, value, rgb.a};
}

/** Generate each Gaussian lattice value once before spatial interpolation. */
__global__ void gaussianLattice(float* output, int width, int height, std::uint32_t seed, std::uint32_t lane)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < static_cast<std::size_t>(width) * height)
        output[i] = emulsionGrainNormal(static_cast<int>(i % width), static_cast<int>(i / width), seed, lane);
}

/** Expand the same low-resolution lattice with bilinear align_corners=False coordinates. */
__global__ void noise(const float* lattice, float* output, int width, int height, int sw, int sh)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= static_cast<std::size_t>(width) * height) return;
    const int x = static_cast<int>(i % width), y = static_cast<int>(i / width);
    const float px = fmaxf(0, fminf((x + 0.5f) * (static_cast<float>(sw) / width) - 0.5f, sw - 1.0f));
    const float py = fmaxf(0, fminf((y + 0.5f) * (static_cast<float>(sh) / height) - 0.5f, sh - 1.0f));
    const int x0 = static_cast<int>(floorf(px)), y0 = static_cast<int>(floorf(py));
    const int x1 = min(x0 + 1, sw - 1), y1 = min(y0 + 1, sh - 1);
    const float fx = px - x0, fy = py - y0;
    const float a = lattice[static_cast<std::size_t>(y0) * sw + x0] * (1 - fx) + lattice[static_cast<std::size_t>(y0) * sw + x1] * fx;
    const float b = lattice[static_cast<std::size_t>(y1) * sw + x0] * (1 - fx) + lattice[static_cast<std::size_t>(y1) * sw + x1] * fx;
    output[i] = a * (1 - fy) + b * fy;
}

/** Use deterministic double block reductions for population mean/variance. */
__global__ void moments(const float* values, std::size_t count, double* partial)
{
    __shared__ double sums[256], squares[256];
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const double value = i < count ? values[i] : 0;
    sums[threadIdx.x] = value; squares[threadIdx.x] = value * value;
    __syncthreads();
    for (int stride = 128; stride > 0; stride /= 2) {
        if (threadIdx.x < stride) { sums[threadIdx.x] += sums[threadIdx.x + stride]; squares[threadIdx.x] += squares[threadIdx.x + stride]; }
        __syncthreads();
    }
    if (threadIdx.x == 0) { partial[2 * blockIdx.x] = sums[0]; partial[2 * blockIdx.x + 1] = squares[0]; }
}

__global__ void statistics(const double* partial, unsigned blocks, std::size_t count, double* result)
{
    __shared__ double sums[256], squares[256];
    double sum = 0, square = 0;
    for (unsigned i = threadIdx.x; i < blocks; i += 256) { sum += partial[2 * i]; square += partial[2 * i + 1]; }
    sums[threadIdx.x] = sum; squares[threadIdx.x] = square;
    __syncthreads();
    for (int stride = 128; stride > 0; stride /= 2) {
        if (threadIdx.x < stride) { sums[threadIdx.x] += sums[threadIdx.x + stride]; squares[threadIdx.x] += squares[threadIdx.x + stride]; }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        const double mean = sums[0] / count;
        result[0] = mean;
        result[1] = 1 / fmax(1e-6, sqrt(fmax(0.0, squares[0] / count - mean * mean)));
    }
}

__device__ float normalized(float value, const double* stats)
{
    return static_cast<float>((value - stats[0]) * stats[1]);
}

__global__ void cluster(float* fields, std::size_t count, const double* stats)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const float fine = normalized(fields[i], stats), mid = normalized(fields[count + i], stats + 2);
    const float coarse = normalized(fields[2 * count + i], stats + 4), envelope = normalized(fields[3 * count + i], stats + 6);
    fields[i] = fine * (0.78f + 0.48f / (1 + expf(-1.3f * envelope))) + mid * 0.42f + coarse * 0.24f;
}

__global__ void develop(ImageView source, ImageView output, RenderWindow window, EmulsionParameters p,
    const Pixel* exposed, const float* grain, const double* stats, float amplitude)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2 || x < output.x1 || y < output.y1 || x >= output.x2 || y >= output.y2) return;
    Pixel result{};
    if (x >= source.x1 && y >= source.y1 && x < source.x2 && y < source.y2) {
        const auto rgb = *address(source, x, y);
        const auto i = static_cast<std::size_t>(y - source.y1) * (source.x2 - source.x1) + x - source.x1;
        const float value = exposed ? fmaxf(0, exposed[i].r) : incident(rgb, p);
        float density = p.stage == FilmStage::Negative ? film::negativeDensity(value, p) : film::printDensity(value, p);
        if (grain) density += tanhf(normalized(grain[i], stats) * 1.25f) * amplitude;
        float signal = p.stage == FilmStage::Negative ? film::transmittance(density) : film::viewPositive(density, p.transfer);
        if (p.stage == FilmStage::Negative && p.previewPositive) {
            EmulsionParameters reference;
            signal = film::viewPositive(film::printDensity(signal, reference), p.transfer);
        }
        if (p.outputPremultiplied) signal *= rgb.a;
        result = {signal, signal, signal, rgb.a};
    }
    *address(output, x, y) = result;
}
} // namespace

CudaStatus renderCudaFilm(ImageView source, ImageView output, RenderWindow window, EmulsionParameters p,
    ProjectorCudaCache& diffusion, void* streamPointer, bool hostStreamProvided) noexcept
{
    const int width = source.x2 - source.x1, height = source.y2 - source.y1;
    if (window.x2 <= window.x1 || window.y2 <= window.y1) return {0, nullptr};
    if (!source.data || !output.data || width <= 0 || height <= 0) return {1, "Invalid CUDA film image"};
    const auto stream = static_cast<cudaStream_t>(streamPointer);
    struct Storage {
        void* memory = nullptr; cudaStream_t stream;
        cudaError_t release() noexcept {
            if (!memory) return cudaSuccess;
            const auto status = cudaFreeAsync(memory, stream);
            if (status == cudaSuccess) memory = nullptr;
            return status;
        }
        ~Storage() {
            const auto status = release();
            if (status != cudaSuccess) std::fprintf(stderr, "[ALEXZSilentFilmOFX] Emulsion scratch cleanup code=%d\n", static_cast<int>(status));
        }
    } storage{nullptr, stream};
    try {
        const auto count = static_cast<std::size_t>(width) * height;
        if (count > std::numeric_limits<std::size_t>::max() / 128) return {2, "CUDA film image exceeds memory limits"};
        const auto blocks = static_cast<unsigned>((count + 255) / 256);
        const float amplitude = p.grainStrength * std::min(1.0f, p.grainSize * width / 1920.0f);
        const auto alignedFields = (5 * count + 1) & ~std::size_t{1};
        const std::size_t grainBytes = amplitude > 0 ? alignedFields * sizeof(float) + (8 + 2 * blocks) * sizeof(double) : 0;
        const std::size_t exposedBytes = p.halationStrength > 0 ? 2 * count * sizeof(Pixel) : 0;
        const auto bytes = grainBytes + exposedBytes;
        if (bytes) {
            const auto status = cudaMallocAsync(&storage.memory, bytes, stream);
            if (status != cudaSuccess) return {static_cast<int>(status), cudaGetErrorString(status)};
        }
        auto* fields = amplitude > 0 ? static_cast<float*>(storage.memory) : nullptr;
        auto* stats = fields ? reinterpret_cast<double*>(fields + alignedFields) : nullptr;
        if (fields) {
            auto* partial = stats + 8;
            const double size = std::max(1.0, static_cast<double>(p.grainSize) * width / 1920.0);
            const double scales[4]{size, 2 * size, 4 * size, 6 * size};
            const auto seed = p.grainSeed ^ film::hash(p.grainFrame);
            for (int lane = 0; lane < 4; ++lane) {
                const int sw = optics::blurDimension(width, 1 / scales[lane]), sh = optics::blurDimension(height, 1 / scales[lane]);
                auto* lattice = fields + 4 * count;
                const auto latticeBlocks = static_cast<unsigned>((static_cast<std::size_t>(sw) * sh + 255) / 256);
                gaussianLattice<<<latticeBlocks, 256, 0, stream>>>(lattice, sw, sh, seed, lane * 16);
                noise<<<blocks, 256, 0, stream>>>(lattice, fields + lane * count, width, height, sw, sh);
                moments<<<blocks, 256, 0, stream>>>(fields + lane * count, count, partial);
                statistics<<<1, 256, 0, stream>>>(partial, blocks, count, stats + 2 * lane);
            }
            cluster<<<blocks, 256, 0, stream>>>(fields, count, stats);
            moments<<<blocks, 256, 0, stream>>>(fields, count, partial);
            statistics<<<1, 256, 0, stream>>>(partial, blocks, count, stats);
        }
        Pixel* exposed = nullptr;
        if (p.halationStrength > 0) {
            auto* incidentImage = reinterpret_cast<Pixel*>(static_cast<unsigned char*>(storage.memory) + grainBytes);
            exposed = incidentImage + count;
            prepare<<<blocks, 256, 0, stream>>>(source, incidentImage, width, height, p);
            const ImageView incidentView{reinterpret_cast<unsigned char*>(incidentImage), static_cast<std::ptrdiff_t>(width * sizeof(Pixel)),
                source.x1, source.y1, source.x2, source.y2};
            auto exposedView = incidentView;
            exposedView.data = reinterpret_cast<unsigned char*>(exposed);
            ProjectorParameters halo;
            halo.transfer = Transfer::Linear; halo.glowStrength = p.halationStrength / (1 + p.halationStrength); halo.glowSize = p.halationSize;
            const auto status = diffusion.render(incidentView, exposedView, {source.x1, source.y1, source.x2, source.y2}, halo, streamPointer, true, true);
            if (status.code) return status;
        }
        const dim3 grid((static_cast<unsigned>(window.x2 - window.x1) + 15) / 16, (static_cast<unsigned>(window.y2 - window.y1) + 15) / 16);
        develop<<<grid, dim3(16, 16), 0, stream>>>(source, output, window, p, exposed, fields, stats, amplitude);
        auto status = cudaPeekAtLastError();
        const auto released = storage.release();
        if (status == cudaSuccess) status = released;
        if (status == cudaSuccess && !hostStreamProvided) status = cudaStreamSynchronize(stream);
        return {static_cast<int>(status), status == cudaSuccess ? nullptr : cudaGetErrorString(status)};
    } catch (...) { return {static_cast<int>(cudaErrorUnknown), "CUDA spatial emulsion failed"}; }
}
} // namespace silentfilm
