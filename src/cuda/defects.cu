#include <cuda_runtime.h>
#include <cstdio>

#include "silentfilm/defects_pixel.hpp"

namespace silentfilm {
namespace {

static_assert(sizeof(DefectFrame) + 2 * sizeof(ImageView) + sizeof(RenderWindow) < 4096,
    "Defect geometry must fit the CUDA launch parameter budget");

__device__ Pixel* address(ImageView image, int x, int y)
{
    return reinterpret_cast<Pixel*>(image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel));
}

__global__ void prepareDevelopment(float* values, const DefectFrame frame)
{
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index < frame.development.count) values[index] = defect::developmentGridValue(frame, index);
}

__global__ void renderKernel(ImageView source, ImageView output, RenderWindow window, const DefectFrame frame)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2 || x < output.x1 || x >= output.x2 || y < output.y1 || y >= output.y2) return;
    Pixel pixel{};
    if (x >= source.x1 && y >= source.y1 && x < source.x2 && y < source.y2)
        pixel = defect::apply(*address(source, x, y), x - static_cast<float>(source.x1) + 0.5f,
            y - static_cast<float>(source.y1) + 0.5f, source.y2 - source.y1, frame);
    *address(output, x, y) = pixel;
}
} // namespace

CudaStatus renderCudaDefects(ImageView source, ImageView output, RenderWindow window,
    const DefectParameters& parameters, void* streamPointer, bool hostStreamProvided) noexcept
{
    if (window.x2 <= window.x1 || window.y2 <= window.y1) return {0, nullptr};
    if (!source.data || !output.data || source.x2 <= source.x1 || source.y2 <= source.y1)
        return {static_cast<int>(cudaErrorInvalidValue), "Invalid CUDA defect image"};
    auto frame = prepareDefects(parameters, source.x2 - source.x1, source.y2 - source.y1);
    const auto stream = static_cast<cudaStream_t>(streamPointer);
    const dim3 grid((static_cast<unsigned>(window.x2 - window.x1) + 15) / 16,
        (static_cast<unsigned>(window.y2 - window.y1) + 15) / 16);
    float* values = nullptr;
    auto status = cudaSuccess;
    if (frame.development.count > 0) {
        status = cudaMallocAsync(reinterpret_cast<void**>(&values), static_cast<std::size_t>(frame.development.count) * sizeof(float), stream);
        if (status == cudaSuccess) {
            frame.development.values = values;
            prepareDevelopment<<<(frame.development.count + 255) / 256, 256, 0, stream>>>(values, frame);
            status = cudaPeekAtLastError();
        }
    }
    if (status == cudaSuccess) {
        renderKernel<<<grid, dim3(16, 16), 0, stream>>>(source, output, window, frame);
        status = cudaPeekAtLastError();
    }
    if (values) {
        const auto cleanup = cudaFreeAsync(values, stream);
        if (cleanup != cudaSuccess) std::fprintf(stderr, "[SilentFilm Defects] development grid cleanup: %s\n", cudaGetErrorString(cleanup));
        if (status == cudaSuccess) status = cleanup;
    }
    if (!hostStreamProvided) {
        const auto completion = cudaStreamSynchronize(stream);
        if (status == cudaSuccess) status = completion;
    }
    return {static_cast<int>(status), status == cudaSuccess ? nullptr : cudaGetErrorString(status)};
}
} // namespace silentfilm
