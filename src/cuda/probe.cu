#include <cuda_runtime.h>

#include "silentfilm/cuda_backend.hpp"
#include "silentfilm/probe.hpp"

namespace silentfilm {
namespace {

/** Locate a device pixel using host-advertised bounds and signed row stride. */
__device__ float* address(ImageView image, int x, int y)
{
    if (x < image.x1 || y < image.y1 || x >= image.x2 || y >= image.y2) return nullptr;
    auto* row = image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes;
    return reinterpret_cast<float*>(row + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel));
}

/** Sample transparent black outside a temporal input's individual bounds. */
__device__ Pixel sample(ImageView image, int x, int y)
{
    const auto* pixel = address(image, x, y);
    return pixel ? Pixel{pixel[0], pixel[1], pixel[2], pixel[3]} : Pixel{};
}

/** Apply the same scalar math as the CPU core within the requested render window. */
__global__ void probeKernel(ImageView current, ImageView previous, ImageView next, ImageView output,
    RenderWindow window, float gain, float temporalMix)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2) return;
    auto* pixel = address(output, x, y);
    if (!pixel) return;
    const auto center = sample(current, x, y);
    const auto value = temporalMix == 0.0f
        ? probePixel(center, {}, {}, gain, 0)
        : probePixel(center, sample(previous, x, y), sample(next, x, y), gain, temporalMix);
    pixel[0] = value.r; pixel[1] = value.g; pixel[2] = value.b; pixel[3] = value.a;
}

/** Process film density per pixel without CPU readback or video-sized scratch storage. */
__global__ void emulsionKernel(ImageView source, ImageView output, RenderWindow window, EmulsionParameters parameters)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2) return;
    auto* pixel = address(output, x, y);
    if (!pixel) return;
    // За пределами источника не формируем плотность чёрного кадра: там прозрачная пустота.
    const auto* input = address(source, x, y);
    const int imageWidth = source.x2 - source.x1;
    const auto value = input ? emulsionPixel({input[0], input[1], input[2], input[3]}, parameters, x, y, imageWidth) : Pixel{};
    pixel[0] = value.r; pixel[1] = value.g; pixel[2] = value.b; pixel[3] = value.a;
}

/** Apply a width-relative vignette from absolute image coordinates without scratch storage. */
__global__ void cameraKernel(ImageView source, ImageView output, RenderWindow window, CameraOpticsParameters parameters)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2) return;
    auto* destination = address(output, x, y);
    if (!destination) return;
    const auto* input = address(source, x, y);
    const auto value = input
        ? cameraPixel({input[0], input[1], input[2], input[3]}, x, y,
            source.x1, source.y1, source.x2, source.y2, parameters)
        : Pixel{};
    destination[0] = value.r; destination[1] = value.g; destination[2] = value.b; destination[3] = value.a;
}

} // namespace

CudaStatus renderCudaProbe(ImageView current, ImageView previous, ImageView next, ImageView output,
    RenderWindow window, float gain, float temporalMix, void* stream, bool hostStreamProvided) noexcept
{
    if (window.x2 <= window.x1 || window.y2 <= window.y1) return {0, nullptr};
    const dim3 threads(16, 16);
    const dim3 blocks((static_cast<unsigned>(window.x2 - window.x1) + threads.x - 1) / threads.x,
                      (static_cast<unsigned>(window.y2 - window.y1) + threads.y - 1) / threads.y);
    const auto cudaStream = static_cast<cudaStream_t>(stream);
    probeKernel<<<blocks, threads, 0, cudaStream>>>(current, previous, next, output, window, gain, temporalMix);
    auto status = cudaPeekAtLastError();
    // С host stream завершением и временем жизни GPU-буферов управляет Resolve.
    // Без переданного stream завершаем собственную работу до возврата из action.
    if (status == cudaSuccess && !hostStreamProvided) status = cudaStreamSynchronize(cudaStream);
    return {static_cast<int>(status), status == cudaSuccess ? nullptr : cudaGetErrorString(status)};
}

CudaStatus renderCudaEmulsion(ImageView source, ImageView output, RenderWindow window,
    EmulsionParameters parameters, void* stream, bool hostStreamProvided) noexcept
{
    if (window.x2 <= window.x1 || window.y2 <= window.y1) return {0, nullptr};
    const dim3 threads(16, 16);
    const dim3 blocks((static_cast<unsigned>(window.x2 - window.x1) + threads.x - 1) / threads.x,
                      (static_cast<unsigned>(window.y2 - window.y1) + threads.y - 1) / threads.y);
    const auto cudaStream = static_cast<cudaStream_t>(stream);
    emulsionKernel<<<blocks, threads, 0, cudaStream>>>(source, output, window, parameters);
    auto status = cudaPeekAtLastError();
    if (status == cudaSuccess && !hostStreamProvided) status = cudaStreamSynchronize(cudaStream);
    return {static_cast<int>(status), status == cudaSuccess ? nullptr : cudaGetErrorString(status)};
}

CudaStatus renderCudaCamera(ImageView source, ImageView output, RenderWindow window,
    CameraOpticsParameters parameters, void* stream, bool hostStreamProvided) noexcept
{
    if (window.x2 <= window.x1 || window.y2 <= window.y1) return {0, nullptr};
    const dim3 threads(16, 16);
    const dim3 blocks((static_cast<unsigned>(window.x2 - window.x1) + threads.x - 1) / threads.x,
                      (static_cast<unsigned>(window.y2 - window.y1) + threads.y - 1) / threads.y);
    const auto cudaStream = static_cast<cudaStream_t>(stream);
    cameraKernel<<<blocks, threads, 0, cudaStream>>>(source, output, window, parameters);
    auto status = cudaPeekAtLastError();
    if (status == cudaSuccess && !hostStreamProvided) status = cudaStreamSynchronize(cudaStream);
    return {static_cast<int>(status), status == cudaSuccess ? nullptr : cudaGetErrorString(status)};
}

} // namespace silentfilm
