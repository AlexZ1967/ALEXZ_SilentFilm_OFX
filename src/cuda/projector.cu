#include <cuda.h>
#include <cuda_runtime.h>
#include <cufft.h>

#include "silentfilm/projector_pixel.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace silentfilm {
namespace {

void requireCuda(cudaError_t status)
{
    if (status != cudaSuccess) throw CudaStatus{static_cast<int>(status), cudaGetErrorString(status)};
}

void requireFft(cufftResult status)
{
    if (status != CUFFT_SUCCESS) throw CudaStatus{10000 + static_cast<int>(status), "Projector cuFFT operation failed"};
}

struct Plans {
    CUcontext context = nullptr;
    cudaStream_t stream = nullptr;
    int width = 0, height = 0;
    cufftHandle handles[4]{};
    std::size_t workspace = 0;
    cudaEvent_t completed = nullptr;

    void destroy() noexcept {
        for (auto& plan : handles) { if (plan) cufftDestroy(plan); plan = 0; }
        if (completed) cudaEventDestroy(completed);
        completed = nullptr;
    }

    void initialize() {
        requireCuda(cudaEventCreateWithFlags(&completed, cudaEventDisableTiming));
        for (int i = 0; i < 4; ++i) {
            requireFft(cufftCreate(&handles[i]));
            requireFft(cufftSetAutoAllocation(handles[i], 0));
            const int length = 2 * (i < 2 ? height : width);
            const int batch = 3 * (i < 2 ? width : height);
            std::size_t bytes = 0;
            requireFft(cufftMakePlan1d(handles[i], length, i % 2 == 0 ? CUFFT_R2C : CUFFT_C2R, batch, &bytes));
            workspace = std::max(workspace, bytes);
            requireFft(cufftSetStream(handles[i], stream));
        }
    }
};

struct FrameStorage {
    void* data = nullptr;
    cudaStream_t stream;
    explicit FrameStorage(cudaStream_t queue) : stream(queue) {}
    ~FrameStorage() { if (data) cudaFreeAsync(data, stream); }
};

struct MarkUse {
    Plans& plans;
    ~MarkUse() { cudaEventRecord(plans.completed, plans.stream); }
};

__device__ Pixel* address(ImageView image, int x, int y)
{
    return reinterpret_cast<Pixel*>(image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel));
}

__global__ void directKernel(ImageView source, ImageView output, RenderWindow window, ProjectorParameters p)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2 || x < output.x1 || x >= output.x2 || y < output.y1 || y >= output.y2) return;
    Pixel result{};
    if (x >= source.x1 && x < source.x2 && y >= source.y1 && y < source.y2)
        result = projection::screen(projection::illuminate(projection::transport(source, x, y, p), p), p);
    *address(output, x, y) = result;
}

/** Preserve the sharp linear source and transported alpha for final glow blending. */
__global__ void illuminateKernel(ImageView source, Pixel* incident, int width, int height, ProjectorParameters p)
{
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x < width && y < height) incident[static_cast<std::size_t>(y) * width + x] = projection::illuminate(
        projection::transport(source, source.x1 + x, source.y1 + y, p), p);
}

/** Build batched reflected lines without padding to a different frequency grid. */
__global__ void mirrorKernel(const Pixel* incident, const float* vertical, float* real,
    int width, int height, bool horizontal, std::size_t count)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= 6 * count) return;
    const int length = horizontal ? width : height;
    const int lines = horizontal ? height : width;
    const auto batch = i / (2 * length);
    const int channel = static_cast<int>(batch / lines), line = static_cast<int>(batch % lines);
    int position = static_cast<int>(i % (2 * length));
    if (position >= length) position = 2 * length - position - 1;
    const auto pixel = horizontal ? static_cast<std::size_t>(line) * width + position
        : static_cast<std::size_t>(position) * width + line;
    if (horizontal) real[i] = vertical[static_cast<std::size_t>(channel) * count + pixel];
    else {
        const auto rgb = incident[pixel];
        real[i] = channel == 0 ? rgb.r : channel == 1 ? rgb.g : rgb.b;
    }
}

/** Apply the same discrete heat-kernel spectrum as the FFTW/Comfy implementation. */
__global__ void spectralKernel(cufftComplex* spectrum, int length, float sigma, std::size_t count)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const int frequency = static_cast<int>(i % (length + 1));
    const float f = static_cast<float>(frequency) / (2 * length);
    const float sine = sinf(3.14159265358979323846f * f);
    const float response = expf(-2 * (sigma * sine) * (sigma * sine));
    spectrum[i].x *= response; spectrum[i].y *= response;
}

__global__ void verticalKernel(const float* real, float* vertical, int width, int height, std::size_t count)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= 3 * count) return;
    const int channel = static_cast<int>(i / count);
    const auto pixel = i % count;
    const int x = static_cast<int>(pixel % width), y = static_cast<int>(pixel / width);
    vertical[i] = real[(static_cast<std::size_t>(channel) * width + x) * (2 * height) + y] / (2 * height);
}

/** Crop the reflected inverse FFT, normalize, blend the sharp source and encode the screen. */
__global__ void outputKernel(const Pixel* incident, const float* real, ImageView source,
    ImageView output, RenderWindow window, ProjectorParameters p)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2 || x < output.x1 || x >= output.x2 || y < output.y1 || y >= output.y2) return;
    Pixel value{};
    if (x >= source.x1 && x < source.x2 && y >= source.y1 && y < source.y2) {
        const int width = source.x2 - source.x1, height = source.y2 - source.y1;
        const int xx = x - source.x1, yy = y - source.y1;
        const auto count = static_cast<std::size_t>(width) * height;
        value = incident[static_cast<std::size_t>(yy) * width + xx];
        const auto offset = static_cast<std::size_t>(yy) * (2 * width) + xx;
        const float weight = p.glowStrength / (2 * width);
        value.r = value.r * (1 - p.glowStrength) + real[offset] * weight;
        value.g = value.g * (1 - p.glowStrength) + real[2 * count + offset] * weight;
        value.b = value.b * (1 - p.glowStrength) + real[4 * count + offset] * weight;
        value = projection::screen(value, p);
    }
    *address(output, x, y) = value;
}

dim3 grid(int width, int height) { return dim3((static_cast<unsigned>(width) + 15) / 16, (static_cast<unsigned>(height) + 15) / 16); }
unsigned blocks(std::size_t count) { return static_cast<unsigned>((count + 255) / 256); }

} // namespace

struct ProjectorCudaCache::Impl {
    std::mutex mutex;
    std::vector<std::unique_ptr<Plans>> plans;

    Plans& get(int width, int height, cudaStream_t stream) {
        CUcontext current = nullptr;
        const auto status = cuCtxGetCurrent(&current);
        if (status != CUDA_SUCCESS || !current) throw CudaStatus{20000 + static_cast<int>(status), "Missing host CUDA context for Projector"};
        for (auto& entry : plans) if (entry->context == current && entry->stream == stream
            && entry->width == width && entry->height == height) return *entry;
        plans.reserve(plans.size() + 1);
        auto entry = std::make_unique<Plans>();
        entry->context = current; entry->stream = stream; entry->width = width; entry->height = height;
        try { entry->initialize(); }
        catch (...) { entry->destroy(); throw; }
        plans.push_back(std::move(entry));
        return *plans.back();
    }

    ~Impl() {
        if (plans.empty()) return;
        // Только наш cleanup-поток привязывает context; context/device потока хоста не меняется.
        // Ждём события последнего использования планов, не весь CUDA device.
        try {
            std::thread cleanup([this] {
                for (auto& entry : plans) {
                    if (cuCtxPushCurrent(entry->context) != CUDA_SUCCESS) continue;
                    if (entry->completed) cudaEventSynchronize(entry->completed);
                    entry->destroy();
                    CUcontext previous = nullptr;
                    cuCtxPopCurrent(&previous);
                }
            });
            cleanup.join();
        } catch (...) {
            // При невозможности создать поток нельзя переключать context хоста.
            // Оставшиеся driver-ресурсы освободятся при уничтожении его context.
        }
    }
};

ProjectorCudaCache::ProjectorCudaCache() : impl_(std::make_unique<Impl>()) {}
ProjectorCudaCache::~ProjectorCudaCache() = default;

CudaStatus ProjectorCudaCache::render(ImageView source, ImageView output, RenderWindow window,
    const ProjectorParameters& p, void* streamPointer, bool hostStreamProvided) noexcept
{
    try {
        if (window.x2 <= window.x1 || window.y2 <= window.y1) return {0, nullptr};
        const int width = source.x2 - source.x1, height = source.y2 - source.y1;
        if (!source.data || !output.data || width <= 0 || height <= 0)
            return {static_cast<int>(cudaErrorInvalidValue), "Invalid Projector image"};
        const auto stream = static_cast<cudaStream_t>(streamPointer);
        if (p.glowStrength <= 0) {
            directKernel<<<grid(window.x2 - window.x1, window.y2 - window.y1), dim3(16, 16), 0, stream>>>(source, output, window, p);
            requireCuda(cudaPeekAtLastError());
            if (!hostStreamProvided) requireCuda(cudaStreamSynchronize(stream));
            return {0, nullptr};
        }
        if (width > std::numeric_limits<int>::max() / 3 || height > std::numeric_limits<int>::max() / 3)
            return {static_cast<int>(cudaErrorInvalidValue), "Projector image dimensions exceed cuFFT batch limits"};
        const auto count = static_cast<std::size_t>(width) * height;
        if (count > std::numeric_limits<std::size_t>::max() / 128)
            return {static_cast<int>(cudaErrorMemoryAllocation), "Projector scratch allocation overflow"};
        const auto spectrumCount = 3 * (count + std::max(width, height));
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        auto& plan = impl_->get(width, height, stream);
        MarkUse mark{plan};
        const auto realOffset = (3 * count + 1) & ~std::size_t{1};
        const std::size_t imageBytes = count * sizeof(Pixel) + (realOffset + 6 * count) * sizeof(float)
            + spectrumCount * sizeof(cufftComplex);
        const auto alignedBytes = (imageBytes + 255) & ~std::size_t{255};
        if (plan.workspace > std::numeric_limits<std::size_t>::max() - alignedBytes)
            return {static_cast<int>(cudaErrorMemoryAllocation), "Projector scratch allocation overflow"};
        FrameStorage storage(stream);
        requireCuda(cudaMallocAsync(&storage.data, alignedBytes + std::max<std::size_t>(plan.workspace, 1), stream));
        auto* incident = static_cast<Pixel*>(storage.data);
        auto* vertical = reinterpret_cast<float*>(incident + count);
        auto* real = vertical + realOffset;
        auto* spectrum = reinterpret_cast<cufftComplex*>(real + 6 * count);
        void* workspace = static_cast<unsigned char*>(storage.data) + alignedBytes;
        for (auto handle : plan.handles) requireFft(cufftSetWorkArea(handle, workspace));
        illuminateKernel<<<grid(width, height), dim3(16, 16), 0, stream>>>(source, incident, width, height, p);
        const float sigma = static_cast<float>(width) * p.glowSize * 0.01f;
        for (int axis = 0; axis < 2; ++axis) {
            const bool horizontal = axis == 1;
            const int length = horizontal ? width : height;
            const auto elements = static_cast<std::size_t>(3) * (horizontal ? height : width) * (length + 1);
            mirrorKernel<<<blocks(6 * count), 256, 0, stream>>>(incident, vertical, real, width, height, horizontal, count);
            requireCuda(cudaPeekAtLastError());
            requireFft(cufftExecR2C(plan.handles[2 * axis], real, spectrum));
            spectralKernel<<<blocks(elements), 256, 0, stream>>>(spectrum, length, sigma, elements);
            requireCuda(cudaPeekAtLastError());
            requireFft(cufftExecC2R(plan.handles[2 * axis + 1], spectrum, real));
            if (!horizontal) verticalKernel<<<blocks(3 * count), 256, 0, stream>>>(real, vertical, width, height, count);
        }
        outputKernel<<<grid(window.x2 - window.x1, window.y2 - window.y1), dim3(16, 16), 0, stream>>>(incident, real, source, output, window, p);
        requireCuda(cudaPeekAtLastError());
        if (!hostStreamProvided) requireCuda(cudaStreamSynchronize(stream));
        return {0, nullptr};
    } catch (const CudaStatus& status) { return status; }
    catch (const std::bad_alloc&) { return {static_cast<int>(cudaErrorMemoryAllocation), "Projector plan cache allocation failed"}; }
    catch (...) { return {static_cast<int>(cudaErrorUnknown), "Projector CUDA processing failed"}; }
}

} // namespace silentfilm
