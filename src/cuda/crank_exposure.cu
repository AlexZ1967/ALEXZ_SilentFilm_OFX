#include <cuda_runtime.h>
#include "silentfilm/crank_exposure.hpp"

namespace silentfilm {
namespace {
__global__ void expose(ImageView source, ImageView output, RenderWindow window, float gain, float blend,
    bool sourcePremultiplied, bool outputPremultiplied)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2 || x < output.x1 || y < output.y1 || x >= output.x2 || y >= output.y2) return;
    Pixel pixel{};
    if (x >= source.x1 && x < source.x2 && y >= source.y1 && y < source.y2) {
        pixel = *reinterpret_cast<const Pixel*>(source.data + (static_cast<std::ptrdiff_t>(y) - source.y1) * source.rowBytes
            + (static_cast<std::ptrdiff_t>(x) - source.x1) * static_cast<std::ptrdiff_t>(sizeof(Pixel)));
        pixel = crankExposurePixel(pixel, gain, blend, sourcePremultiplied, outputPremultiplied);
    }
    *reinterpret_cast<Pixel*>(output.data + (static_cast<std::ptrdiff_t>(y) - output.y1) * output.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - output.x1) * static_cast<std::ptrdiff_t>(sizeof(Pixel))) = pixel;
}
}
CudaStatus renderCudaCrankExposure(ImageView source, ImageView output, RenderWindow window, float gain, float blend,
    bool sourcePremultiplied, bool outputPremultiplied, void* stream, bool hostStreamProvided) noexcept
{
    if (window.x2 <= window.x1 || window.y2 <= window.y1) return {0, nullptr};
    const auto queue = static_cast<cudaStream_t>(stream);
    const dim3 grid((static_cast<unsigned>(window.x2 - window.x1) + 15) / 16,
        (static_cast<unsigned>(window.y2 - window.y1) + 15) / 16);
    expose<<<grid, dim3(16, 16), 0, queue>>>(source, output, window, gain, blend, sourcePremultiplied, outputPremultiplied);
    auto status = cudaPeekAtLastError();
    if (status == cudaSuccess && !hostStreamProvided) status = cudaStreamSynchronize(queue);
    return {static_cast<int>(status), status == cudaSuccess ? nullptr : cudaGetErrorString(status)};
}
} // namespace silentfilm
