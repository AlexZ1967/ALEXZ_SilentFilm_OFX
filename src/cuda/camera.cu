#include <cuda_runtime.h>

#include "silentfilm/camera_capture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace silentfilm {
namespace {

constexpr int maximumSamples = maximumCameraExposureSamples;

struct CapturePacket {
    ImageView images[maximumSamples];
    ImageView nextImages[maximumSamples];
    CameraSampleTransform transforms[maximumSamples];
    float temporalMix[maximumSamples]{};
    float weights[maximumSamples]{};
    int count = 0;
    bool shake = false, premultiplied = false;
};

struct GaussianKernel {
    float weights[33]{};
    int radius = 0;
};

__device__ Pixel* address(ImageView image, int x, int y)
{
    return reinterpret_cast<Pixel*>(image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel));
}

__device__ Pixel readStraight(ImageView image, int x, int y, bool premultiplied)
{
    x = max(image.x1, min(x, image.x2 - 1));
    y = max(image.y1, min(y, image.y2 - 1));
    Pixel value = *address(image, x, y);
    if (premultiplied) {
        if (value.a <= 0) value.r = value.g = value.b = 0;
        else { value.r /= value.a; value.g /= value.a; value.b /= value.a; }
    }
    value.r = fmaxf(0, fminf(1, value.r));
    value.g = fmaxf(0, fminf(1, value.g));
    value.b = fmaxf(0, fminf(1, value.b));
    return value;
}

__device__ Pixel blend(Pixel a, Pixel b, float t)
{
    return {a.r * (1 - t) + b.r * t, a.g * (1 - t) + b.g * t,
        a.b * (1 - t) + b.b * t, a.a * (1 - t) + b.a * t};
}

__device__ Pixel bilinear(ImageView image, double x, double y, bool premultiplied)
{
    x = fmax(static_cast<double>(image.x1), fmin(x, static_cast<double>(image.x2 - 1)));
    y = fmax(static_cast<double>(image.y1), fmin(y, static_cast<double>(image.y2 - 1)));
    const int x0 = static_cast<int>(floor(x)), y0 = static_cast<int>(floor(y));
    const int x1 = min(x0 + 1, image.x2 - 1), y1 = min(y0 + 1, image.y2 - 1);
    const float fx = static_cast<float>(x - x0), fy = static_cast<float>(y - y0);
    return blend(blend(readStraight(image, x0, y0, premultiplied), readStraight(image, x1, y0, premultiplied), fx),
        blend(readStraight(image, x0, y1, premultiplied), readStraight(image, x1, y1, premultiplied), fx), fy);
}

__device__ Pixel scaled(Pixel value, float weight)
{
    return {value.r * weight, value.g * weight, value.b * weight, value.a * weight};
}

__device__ void addWeighted(Pixel& sum, Pixel value, float weight)
{
    sum.r += value.r * weight; sum.g += value.g * weight; sum.b += value.b * weight;
    sum.a += value.a * weight;
}

/** Integrate one captured pixel with the shared CPU-generated affine transforms. */
__device__ Pixel captureValue(const CapturePacket& packet, int x, int y, int width, int height)
{
    const auto bounds = packet.images[0];
    const int ax = bounds.x1 + x, ay = bounds.y1 + y;
    const double cx = bounds.x1 + (width - 1) * 0.5, cy = bounds.y1 + (height - 1) * 0.5;
    Pixel sum{};
    for (int i = 0; i < packet.count; ++i) {
        Pixel value;
        if (packet.shake) {
            const auto transform = packet.transforms[i];
            const double dx = ax - cx, dy = ay - cy;
            const double sx = cx + transform.cosine * dx + transform.sine * dy - transform.shiftX;
            const double sy = cy - transform.sine * dx + transform.cosine * dy - transform.shiftY;
            value = bilinear(packet.images[i], sx, sy, packet.premultiplied);
            if (packet.temporalMix[i] > 0)
                value = blend(value, bilinear(packet.nextImages[i], sx, sy, packet.premultiplied), packet.temporalMix[i]);
        } else {
            value = readStraight(packet.images[i], ax, ay, packet.premultiplied);
            if (packet.temporalMix[i] > 0)
                value = blend(value, readStraight(packet.nextImages[i], ax, ay, packet.premultiplied), packet.temporalMix[i]);
        }
        addWeighted(sum, value, packet.weights[i]);
    }
    return sum;
}

/** Fuse temporal integration, affine shake, unpremultiplication and the shared edge mask. */
__global__ void captureKernel(CapturePacket packet, Pixel* captured, float* mask,
    int width, int height, CameraOpticsParameters parameters)
{
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= width || y >= height) return;
    const auto bounds = packet.images[0];
    const int ax = bounds.x1 + x, ay = bounds.y1 + y;
    const auto index = static_cast<std::size_t>(y) * width + x;
    captured[index] = captureValue(packet, x, y, width, height);
    mask[index] = parameters.vignetteStrength > 0 || parameters.edgeSoftness > 0
        ? optics::lensEdgeMask(ax, ay, bounds.x1, bounds.y1, bounds.x2, bounds.y2, parameters) : 0.0f;
}

/** Match PyTorch area/adaptive-average reduction for broad Gaussian kernels. */
__global__ void areaKernel(const Pixel* source, Pixel* output, int sw, int sh, int ow, int oh)
{
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= ow || y >= oh) return;
    const int xb = static_cast<int>(static_cast<long long>(x) * sw / ow);
    const int xe = static_cast<int>((static_cast<long long>(x + 1) * sw + ow - 1) / ow);
    const int yb = static_cast<int>(static_cast<long long>(y) * sh / oh);
    const int ye = static_cast<int>((static_cast<long long>(y + 1) * sh + oh - 1) / oh);
    Pixel sum{};
    for (int sy = yb; sy < ye; ++sy) {
        for (int sx = xb; sx < xe; ++sx) {
            addWeighted(sum, source[static_cast<std::size_t>(sy) * sw + sx], 1.0f);
        }
    }
    output[static_cast<std::size_t>(y) * ow + x] = scaled(sum, 1.0f / static_cast<float>((xe - xb) * (ye - yb)));
}

__device__ int border(int index, int length, bool reflect)
{
    if (!reflect || length <= 1) return max(0, min(index, length - 1));
    const int period = 2 * (length - 1);
    int position = index % period;
    if (position < 0) position += period;
    return position < length ? position : period - position;
}

/** Process RGB together so each blur level shares addressing and kernel weights. */
__global__ void convolutionKernel(const Pixel* source, Pixel* output, int width, int height,
    GaussianKernel kernel, bool horizontal, bool reflect)
{
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    extern __shared__ Pixel tile[];
    const int tileWidth = static_cast<int>(blockDim.x) + (horizontal ? 2 * kernel.radius : 0);
    const int tileHeight = static_cast<int>(blockDim.y) + (horizontal ? 0 : 2 * kernel.radius);
    const int thread = static_cast<int>(threadIdx.y * blockDim.x + threadIdx.x);
    for (int i = thread; i < tileWidth * tileHeight; i += static_cast<int>(blockDim.x * blockDim.y)) {
        const int sx = border(static_cast<int>(blockIdx.x * blockDim.x) + i % tileWidth
            - (horizontal ? kernel.radius : 0), width, reflect);
        const int sy = border(static_cast<int>(blockIdx.y * blockDim.y) + i / tileWidth
            - (horizontal ? 0 : kernel.radius), height, reflect);
        tile[i] = source[static_cast<std::size_t>(sy) * width + sx];
    }
    __syncthreads();
    if (x >= width || y >= height) return;
    Pixel sum{};
    for (int offset = -kernel.radius; offset <= kernel.radius; ++offset) {
        const int sx = static_cast<int>(threadIdx.x) + (horizontal ? offset + kernel.radius : 0);
        const int sy = static_cast<int>(threadIdx.y) + (horizontal ? 0 : offset + kernel.radius);
        addWeighted(sum, tile[sy * tileWidth + sx], kernel.weights[offset + kernel.radius]);
    }
    output[static_cast<std::size_t>(y) * width + x] = sum;
}

__global__ void resizeKernel(const Pixel* source, Pixel* output, int sw, int sh, int ow, int oh)
{
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= ow || y >= oh) return;
    const float px = fmaxf(0, fminf((x + 0.5f) * (static_cast<float>(sw) / ow) - 0.5f, sw - 1.0f));
    const float py = fmaxf(0, fminf((y + 0.5f) * (static_cast<float>(sh) / oh) - 0.5f, sh - 1.0f));
    const int x0 = static_cast<int>(floorf(px)), y0 = static_cast<int>(floorf(py));
    const int x1 = min(x0 + 1, sw - 1), y1 = min(y0 + 1, sh - 1);
    output[static_cast<std::size_t>(y) * ow + x] = blend(
        blend(source[static_cast<std::size_t>(y0) * sw + x0], source[static_cast<std::size_t>(y0) * sw + x1], px - x0),
        blend(source[static_cast<std::size_t>(y1) * sw + x0], source[static_cast<std::size_t>(y1) * sw + x1], px - x0), py - y0);
}

/** Accumulate one edge or focus level; skip all unused focus levels on the host. */
__global__ void accumulateKernel(const Pixel* blurred, Pixel* output, const float* mask,
    std::size_t count, float constantWeight, float maximumSigma,
    float previous, float current, float next, int level, int levelCount, bool reset)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= count) return;
    float weight = constantWeight;
    if (constantWeight < 0) {
        const float sigma = mask[i] * maximumSigma;
        if (levelCount == 1) weight = 1;
        else if (level == 0) weight = fmaxf(0, fminf(1, 1 - sigma / next));
        else {
            weight = fmaxf(0, fminf(1, (sigma - previous) / (current - previous)));
            if (level + 1 < levelCount) weight = fminf(weight, fmaxf(0, fminf(1, (next - sigma) / (next - current))));
        }
    }
    Pixel sum = reset ? Pixel{} : output[i];
    addWeighted(sum, blurred[i], weight);
    output[i] = sum;
}

__global__ void vignetteKernel(Pixel* signal, const float* mask, std::size_t count, float strength)
{
    const auto i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < count) {
        const float weight = 1 - mask[i] * strength;
        signal[i].r *= weight; signal[i].g *= weight; signal[i].b *= weight;
    }
}

__device__ float expose(float value, float gain)
{
    if (gain == 1.0f) return fminf(1, fmaxf(0, value));
    return film::encode(fminf(1, fmaxf(0, film::decode(value, Transfer::SRGB) * gain)), Transfer::SRGB);
}

/** Fuse exposure encoding, monochrome conversion and output premultiplication. */
__global__ void outputKernel(const Pixel* signal, const Pixel* captured, ImageView bounds,
    ImageView output, RenderWindow window, float exposureGain, bool premultiplied)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2 || x < output.x1 || y < output.y1 || x >= output.x2 || y >= output.y2) return;
    Pixel result{};
    if (x >= bounds.x1 && y >= bounds.y1 && x < bounds.x2 && y < bounds.y2) {
        const auto i = static_cast<std::size_t>(y - bounds.y1) * (bounds.x2 - bounds.x1) + (x - bounds.x1);
        const auto rgb = signal[i];
        float value = expose(rgb.r, exposureGain) * 0.299f + expose(rgb.g, exposureGain) * 0.587f
            + expose(rgb.b, exposureGain) * 0.114f;
        const float alpha = captured[i].a;
        if (premultiplied) value *= alpha;
        result = {value, value, value, alpha};
    }
    *address(output, x, y) = result;
}

/** Render cameras without spatial blur in a single pass with no scratch allocation. */
__global__ void directKernel(CapturePacket packet, ImageView output, RenderWindow window,
    CameraOpticsParameters parameters, float gain)
{
    const int x = window.x1 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = window.y1 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= window.x2 || y >= window.y2 || x < output.x1 || y < output.y1 || x >= output.x2 || y >= output.y2) return;
    const auto bounds = packet.images[0];
    Pixel result{};
    if (x >= bounds.x1 && x < bounds.x2 && y >= bounds.y1 && y < bounds.y2) {
        const auto rgb = captureValue(packet, x - bounds.x1, y - bounds.y1, bounds.x2 - bounds.x1, bounds.y2 - bounds.y1);
        if (parameters.captureOnly) {
            result = {expose(rgb.r, gain), expose(rgb.g, gain), expose(rgb.b, gain), rgb.a};
            if (parameters.outputPremultiplied) {
                result.r *= result.a; result.g *= result.a; result.b *= result.a;
            }
            *address(output, x, y) = result;
            return;
        }
        const float mask = parameters.vignetteStrength > 0
            ? optics::lensEdgeMask(x, y, bounds.x1, bounds.y1, bounds.x2, bounds.y2, parameters) : 0;
        const float falloff = 1 - mask * parameters.vignetteStrength;
        float value = expose(rgb.r * falloff, gain) * 0.299f + expose(rgb.g * falloff, gain) * 0.587f
            + expose(rgb.b * falloff, gain) * 0.114f;
        if (parameters.outputPremultiplied) value *= rgb.a;
        result = {value, value, value, rgb.a};
    }
    *address(output, x, y) = result;
}

dim3 grid(int width, int height)
{
    return dim3((static_cast<unsigned>(width) + 15) / 16, (static_cast<unsigned>(height) + 15) / 16);
}

void gaussian(const Pixel* source, Pixel* output, Pixel* smallA, Pixel* smallB,
    int width, int height, float sigma, cudaStream_t stream)
{
    const double scale = std::min(1.0, 3.0 / sigma);
    const int sw = scale < 1 ? optics::blurDimension(width, scale) : width;
    const int sh = scale < 1 ? optics::blurDimension(height, scale) : height;
    const float smallSigma = static_cast<float>(sigma * scale);
    GaussianKernel kernel;
    kernel.radius = static_cast<int>(std::ceil(3.0f * smallSigma));
    float total = 0;
    for (int offset = -kernel.radius; offset <= kernel.radius; ++offset) {
        const float n = static_cast<float>(offset) / smallSigma;
        const float weight = std::exp(-0.5f * n * n);
        kernel.weights[offset + kernel.radius] = weight;
        total += weight;
    }
    for (int i = 0; i <= 2 * kernel.radius; ++i) kernel.weights[i] /= total;
    const Pixel* reduced = source;
    if (scale < 1) {
        areaKernel<<<grid(sw, sh), dim3(16, 16), 0, stream>>>(source, smallA, width, height, sw, sh);
        reduced = smallA;
    }
    const bool reflect = std::min(sw, sh) > kernel.radius;
    const auto sharedBytes = static_cast<std::size_t>(16 * (16 + 2 * kernel.radius)) * sizeof(Pixel);
    convolutionKernel<<<grid(sw, sh), dim3(16, 16), sharedBytes, stream>>>(reduced, smallB, sw, sh, kernel, true, reflect);
    Pixel* vertical = scale < 1 ? smallA : output;
    convolutionKernel<<<grid(sw, sh), dim3(16, 16), sharedBytes, stream>>>(smallB, vertical, sw, sh, kernel, false, reflect);
    if (scale < 1) resizeKernel<<<grid(width, height), dim3(16, 16), 0, stream>>>(smallA, output, sw, sh, width, height);
}

struct BlurLevels {
    std::array<float, 64> values{};
    int count = 1;
    explicit BlurLevels(float maximum) {
        if (maximum <= 0) return;
        float sigma = std::min(0.5f, maximum);
        while (sigma < maximum && count + 1 < static_cast<int>(values.size())) {
            values[count++] = sigma;
            sigma *= 2;
        }
        values[count++] = maximum;
    }
    float weight(int level, float sigma) const {
        if (count == 1) return 1;
        if (level == 0) return std::clamp(1 - sigma / values[1], 0.0f, 1.0f);
        float w = std::clamp((sigma - values[level - 1]) / (values[level] - values[level - 1]), 0.0f, 1.0f);
        if (level + 1 < count) w = std::min(w, std::clamp((values[level + 1] - sigma)
            / (values[level + 1] - values[level]), 0.0f, 1.0f));
        return w;
    }
};

} // namespace

CudaStatus renderCudaCamera(const CameraExposureSample* samples, const CameraSampleTransform* transforms,
    int sampleCount, ImageView output, RenderWindow window, const CameraOpticsParameters& parameters,
    float exposureGain, bool shakeEnabled, void* stream, bool hostStreamProvided) noexcept
{
    if (!samples || !transforms || sampleCount < 1 || sampleCount > maximumSamples)
        return {static_cast<int>(cudaErrorInvalidValue), "Invalid CUDA camera samples"};
    if (window.x2 <= window.x1 || window.y2 <= window.y1) return {0, nullptr};
    const auto bounds = samples[0].image;
    const int width = bounds.x2 - bounds.x1, height = bounds.y2 - bounds.y1;
    if (width <= 0 || height <= 0) return {static_cast<int>(cudaErrorInvalidValue), "Empty CUDA camera source"};
    const auto count = static_cast<std::size_t>(width) * height;
    if (count > std::numeric_limits<std::size_t>::max() / (6 * sizeof(Pixel) + sizeof(float)))
        return {static_cast<int>(cudaErrorMemoryAllocation), "CUDA camera image is too large"};
    CapturePacket packet{};
    packet.count = sampleCount; packet.shake = shakeEnabled; packet.premultiplied = parameters.sourcePremultiplied;
    float totalWeight = 0;
    for (int i = 0; i < sampleCount; ++i) totalWeight += samples[i].weight;
    if (!(totalWeight > 0)) return {static_cast<int>(cudaErrorInvalidValue), "Invalid CUDA camera weights"};
    for (int i = 0; i < sampleCount; ++i) {
        if (!samples[i].image.data || samples[i].image.x2 <= samples[i].image.x1 || samples[i].image.y2 <= samples[i].image.y1)
            return {static_cast<int>(cudaErrorInvalidValue), "Invalid CUDA camera source bounds"};
        packet.images[i] = samples[i].image;
        if (samples[i].temporalMix > 0 && (!samples[i].nextImage.data
            || samples[i].nextImage.x2 <= samples[i].nextImage.x1 || samples[i].nextImage.y2 <= samples[i].nextImage.y1))
            return {static_cast<int>(cudaErrorInvalidValue), "Invalid CUDA camera interpolation image"};
        packet.nextImages[i] = samples[i].nextImage;
        packet.temporalMix[i] = samples[i].temporalMix;
        packet.weights[i] = samples[i].weight / totalWeight;
        packet.transforms[i] = transforms[i];
    }
    const auto cudaStream = static_cast<cudaStream_t>(stream);
    const float edgeMaximum = static_cast<float>(width) / 40.0f * parameters.edgeSoftness;
    const float focusMaximum = static_cast<float>(width) / 1920.0f * parameters.focusDrift * 8.0f;
    const float focusTarget = focusMaximum * std::clamp((parameters.focusSignal + 1) * 0.5f, 0.0f, 1.0f);
    const bool focusActive = parameters.focusDrift > 0 && focusTarget > 0;
    if (parameters.captureOnly || (edgeMaximum <= 0 && parameters.softness <= 0 && !focusActive)) {
        directKernel<<<grid(window.x2 - window.x1, window.y2 - window.y1), dim3(16, 16), 0, cudaStream>>>(
            packet, output, window, parameters, exposureGain);
        auto status = cudaPeekAtLastError();
        if (status == cudaSuccess && !hostStreamProvided) status = cudaStreamSynchronize(cudaStream);
        return {static_cast<int>(status), status == cudaSuccess ? nullptr : cudaGetErrorString(status)};
    }
    // Stream-ordered pool: нет cudaFree/cudaDeviceSynchronize и нет передачи пикселей через CPU.
    void* storage = nullptr;
    const int scratchFrames = 5 + (focusActive ? 1 : 0);
    auto status = cudaMallocAsync(&storage, count * (scratchFrames * sizeof(Pixel) + sizeof(float)), cudaStream);
    if (status != cudaSuccess) return {static_cast<int>(status), cudaGetErrorString(status)};
    auto* captured = static_cast<Pixel*>(storage);
    Pixel* cursor = captured + count;
    Pixel* spare = cursor; cursor += count;
    Pixel* state = edgeMaximum > 0 ? spare : captured;
    Pixel* focused = nullptr;
    if (focusActive) { focused = cursor; cursor += count; }
    Pixel* blurred = cursor;
    Pixel* smallA = blurred + count;
    Pixel* smallB = smallA + count;
    auto* mask = reinterpret_cast<float*>(smallB + count);
    const unsigned blocks = static_cast<unsigned>((count + 255) / 256);
    captureKernel<<<grid(width, height), dim3(16, 16), 0, cudaStream>>>(packet, captured, mask, width, height, parameters);
    const BlurLevels edge(edgeMaximum);
    for (int level = 0; edgeMaximum > 0 && level < edge.count; ++level) {
        const Pixel* input = captured;
        if (level > 0) {
            gaussian(captured, blurred, smallA, smallB, width, height, edge.values[level], cudaStream);
            input = blurred;
        }
        accumulateKernel<<<blocks, 256, 0, cudaStream>>>(input, state, mask, count, -1, edgeMaximum,
            level > 0 ? edge.values[level - 1] : 0, edge.values[level],
            level + 1 < edge.count ? edge.values[level + 1] : 0, level, edge.count, level == 0);
    }
    if (parameters.vignetteStrength > 0)
        vignetteKernel<<<blocks, 256, 0, cudaStream>>>(state, mask, count, parameters.vignetteStrength);
    if (parameters.softness > 0) {
        const float sigma = std::max(0.1f, static_cast<float>(width) / 1920.0f * parameters.softness * 4.0f);
        gaussian(state, blurred, smallA, smallB, width, height, sigma, cudaStream);
        if (state == captured) { state = blurred; blurred = spare; }
        else std::swap(state, blurred);
    }
    if (focusActive) {
        const BlurLevels focus(focusMaximum);
        bool reset = true;
        for (int level = 0; level < focus.count; ++level) {
            const float weight = focus.weight(level, focusTarget);
            if (weight == 0) continue;
            const Pixel* input = state;
            if (level > 0) {
                gaussian(state, blurred, smallA, smallB, width, height, focus.values[level], cudaStream);
                input = blurred;
            }
            accumulateKernel<<<blocks, 256, 0, cudaStream>>>(input, focused, mask, count, weight, 0,
                0, 0, 0, 0, 1, reset);
            reset = false;
        }
        state = focused;
    }
    outputKernel<<<grid(window.x2 - window.x1, window.y2 - window.y1), dim3(16, 16), 0, cudaStream>>>(
        state, captured, bounds, output, window, exposureGain, parameters.outputPremultiplied);
    status = cudaPeekAtLastError();
    const auto releaseStatus = cudaFreeAsync(storage, cudaStream);
    if (status == cudaSuccess) status = releaseStatus;
    if (status == cudaSuccess && !hostStreamProvided) status = cudaStreamSynchronize(cudaStream);
    return {static_cast<int>(status), status == cudaSuccess ? nullptr : cudaGetErrorString(status)};
}

} // namespace silentfilm
