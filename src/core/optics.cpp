#include "silentfilm/optics.hpp"
#include "silentfilm/cuda_backend.hpp"
#include "silentfilm/probe.hpp"
#include "silentfilm/sensitometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace silentfilm {
namespace {

using Plane = std::vector<float>;

bool cancelled(AbortCallback abort, void* context)
{
    return abort && abort(context);
}

bool gaussianBlur(const Plane& source, int width, int height, float sigma,
    Plane& output, AbortCallback abort, void* context);
float levelWeight(const std::vector<float>& levels, std::size_t index, float sigma);

Pixel readPixel(ImageView image, int x, int y)
{
    if (x < image.x1 || y < image.y1 || x >= image.x2 || y >= image.y2) return {};
    Pixel pixel{};
    const auto* address = image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel);
    std::memcpy(&pixel, address, sizeof(pixel));
    return pixel;
}

Plane processCameraPlane(Plane signal, int width, int height, const Plane& edgeMask,
    const CameraOpticsParameters& parameters, AbortCallback abort, void* context)
{
    const auto pixelCount = signal.size();
    Plane accumulated;
    const float edgeMaximumSigma = static_cast<float>(width) / 40.0f * parameters.edgeSoftness;
    if (edgeMaximumSigma <= 0) accumulated = std::move(signal);
    else accumulated.assign(pixelCount, 0.0f);
    std::vector<float> edgeLevels{0.0f};
    if (edgeMaximumSigma > 0) {
        float sigma = std::min(0.5f, edgeMaximumSigma);
        while (sigma < edgeMaximumSigma) { edgeLevels.push_back(sigma); sigma *= 2.0f; }
        edgeLevels.push_back(edgeMaximumSigma);
    }

    for (std::size_t level = 0; edgeMaximumSigma > 0 && level < edgeLevels.size(); ++level) {
        Plane blurred;
        if (level == 0) blurred = signal;
        else if (!gaussianBlur(signal, width, height, edgeLevels[level], blurred, abort, context)) return {};
        for (int y = 0; y < height; ++y) {
            if (cancelled(abort, context)) return {};
            for (int x = 0; x < width; ++x) {
                const auto index = static_cast<std::size_t>(y) * width + x;
                const float weight = levelWeight(edgeLevels, level, edgeMask[index] * edgeMaximumSigma);
                accumulated[index] += blurred[index] * weight;
            }
        }
    }
    if (parameters.vignetteStrength > 0) {
        for (std::size_t index = 0; index < pixelCount; ++index)
            accumulated[index] *= 1.0f - edgeMask[index] * parameters.vignetteStrength;
    }
    if (parameters.softness > 0) {
        Plane softened;
        const float sigma = std::max(0.1f, static_cast<float>(width) / 1920.0f * parameters.softness * 4.0f);
        if (!gaussianBlur(accumulated, width, height, sigma, softened, abort, context)) return {};
        accumulated = std::move(softened);
    }
    if (parameters.focusDrift > 0) {
        const float focusMask = std::clamp((parameters.focusSignal + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float maximumSigma = static_cast<float>(width) / 1920.0f * parameters.focusDrift * 8.0f;
        const float sigmaTarget = maximumSigma * focusMask;
        if (sigmaTarget > 0.0f && maximumSigma > 0.0f) {
            std::vector<float> levels{0.0f};
            float sigma = std::min(0.5f, maximumSigma);
            while (sigma < maximumSigma) { levels.push_back(sigma); sigma *= 2.0f; }
            levels.push_back(maximumSigma);
            Plane focused(pixelCount, 0.0f);
            for (std::size_t level = 0; level < levels.size(); ++level) {
                const float weight = levelWeight(levels, level, sigmaTarget);
                if (weight == 0.0f) continue;
                Plane blurred;
                if (level == 0) blurred = accumulated;
                else if (!gaussianBlur(accumulated, width, height, levels[level], blurred, abort, context)) return {};
                for (std::size_t i = 0; i < pixelCount; ++i) focused[i] += blurred[i] * weight;
            }
            accumulated = std::move(focused);
        }
    }
    return accumulated;
}

int borderIndex(int index, int length, bool reflect)
{
    if (!reflect || length <= 1) return std::clamp(index, 0, length - 1);
    if (index < 0) return -index;
    if (index >= length) return 2 * length - index - 2;
    return index;
}

bool areaResize(const Plane& source, int sourceWidth, int sourceHeight,
    Plane& output, int outputWidth, int outputHeight, AbortCallback abort, void* context)
{
    output.resize(static_cast<std::size_t>(outputWidth) * outputHeight);
    for (int y = 0; y < outputHeight; ++y) {
        if (cancelled(abort, context)) return false;
        // PyTorch area использует adaptive average pooling с целыми границами корзин.
        const int firstY = static_cast<int>(static_cast<std::int64_t>(y) * sourceHeight / outputHeight);
        const int lastY = static_cast<int>((static_cast<std::int64_t>(y + 1) * sourceHeight + outputHeight - 1) / outputHeight);
        for (int x = 0; x < outputWidth; ++x) {
            const int firstX = static_cast<int>(static_cast<std::int64_t>(x) * sourceWidth / outputWidth);
            const int lastX = static_cast<int>((static_cast<std::int64_t>(x + 1) * sourceWidth + outputWidth - 1) / outputWidth);
            float sum = 0;
            for (int sy = firstY; sy < lastY; ++sy) {
                for (int sx = firstX; sx < lastX; ++sx) {
                    sum += source[static_cast<std::size_t>(sy) * sourceWidth + sx];
                }
            }
            output[static_cast<std::size_t>(y) * outputWidth + x] = sum / static_cast<float>((lastX - firstX) * (lastY - firstY));
        }
    }
    return true;
}

bool convolve(const Plane& source, Plane& output, int width, int height,
    const std::vector<float>& kernel, bool horizontal, bool reflect,
    AbortCallback abort, void* context)
{
    const int radius = static_cast<int>(kernel.size() / 2);
    output.resize(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = 0; x < width; ++x) {
            float sum = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int sx = horizontal ? borderIndex(x + offset, width, reflect) : x;
                const int sy = horizontal ? y : borderIndex(y + offset, height, reflect);
                sum += source[static_cast<std::size_t>(sy) * width + sx]
                    * kernel[static_cast<std::size_t>(offset + radius)];
            }
            output[static_cast<std::size_t>(y) * width + x] = sum;
        }
    }
    return true;
}

bool resizeBilinear(const Plane& source, int sourceWidth, int sourceHeight,
    Plane& output, int outputWidth, int outputHeight, AbortCallback abort, void* context)
{
    output.resize(static_cast<std::size_t>(outputWidth) * outputHeight);
    const float scaleX = static_cast<float>(sourceWidth) / outputWidth;
    const float scaleY = static_cast<float>(sourceHeight) / outputHeight;
    for (int y = 0; y < outputHeight; ++y) {
        if (cancelled(abort, context)) return false;
        const float sourceY = std::clamp((static_cast<float>(y) + 0.5f) * scaleY - 0.5f, 0.0f, static_cast<float>(sourceHeight - 1));
        const int y0 = static_cast<int>(std::floor(sourceY));
        const int y1 = std::min(y0 + 1, sourceHeight - 1);
        const float fy = sourceY - y0;
        for (int x = 0; x < outputWidth; ++x) {
            const float sourceX = std::clamp((static_cast<float>(x) + 0.5f) * scaleX - 0.5f, 0.0f, static_cast<float>(sourceWidth - 1));
            const int x0 = static_cast<int>(std::floor(sourceX));
            const int x1 = std::min(x0 + 1, sourceWidth - 1);
            const float fx = sourceX - x0;
            const float top = source[static_cast<std::size_t>(y0) * sourceWidth + x0] * (1 - fx)
                + source[static_cast<std::size_t>(y0) * sourceWidth + x1] * fx;
            const float bottom = source[static_cast<std::size_t>(y1) * sourceWidth + x0] * (1 - fx)
                + source[static_cast<std::size_t>(y1) * sourceWidth + x1] * fx;
            output[static_cast<std::size_t>(y) * outputWidth + x] = top * (1 - fy) + bottom * fy;
        }
    }
    return true;
}

bool gaussianBlur(const Plane& source, int width, int height, float sigma,
    Plane& output, AbortCallback abort, void* context)
{
    const double scale = std::min(1.0, 3.0 / sigma);
    const int smallWidth = scale < 1 ? optics::blurDimension(width, scale) : width;
    const int smallHeight = scale < 1 ? optics::blurDimension(height, scale) : height;
    Plane reduced;
    if (scale < 1) {
        if (!areaResize(source, width, height, reduced, smallWidth, smallHeight, abort, context)) return false;
    } else {
        reduced = source;
    }

    const float smallSigma = static_cast<float>(sigma * scale);
    const int radius = static_cast<int>(std::ceil(3.0f * smallSigma));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    float total = 0;
    for (int offset = -radius; offset <= radius; ++offset) {
        const float normalized = static_cast<float>(offset) / smallSigma;
        const float weight = std::exp(-0.5f * normalized * normalized);
        kernel[static_cast<std::size_t>(offset + radius)] = weight;
        total += weight;
    }
    for (auto& weight : kernel) weight /= total;

    const bool reflect = std::min(smallWidth, smallHeight) > radius;
    Plane horizontal, vertical;
    if (!convolve(reduced, horizontal, smallWidth, smallHeight, kernel, true, reflect, abort, context)
        || !convolve(horizontal, vertical, smallWidth, smallHeight, kernel, false, reflect, abort, context)) return false;
    if (scale < 1) return resizeBilinear(vertical, smallWidth, smallHeight, output, width, height, abort, context);
    output = std::move(vertical);
    return true;
}

float levelWeight(const std::vector<float>& levels, std::size_t index, float sigma)
{
    if (levels.size() == 1) return 1.0f;
    if (index == 0) return std::clamp(1.0f - sigma / levels[1], 0.0f, 1.0f);
    const float current = levels[index];
    float weight = std::clamp((sigma - levels[index - 1]) / (current - levels[index - 1]), 0.0f, 1.0f);
    if (index + 1 < levels.size()) {
        weight = std::min(weight, std::clamp((levels[index + 1] - sigma)
            / (levels[index + 1] - current), 0.0f, 1.0f));
    }
    return weight;
}

} // namespace

void CameraFocusCache::clear()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<float>{}.swap(signals_);
}

float CameraFocusCache::at(std::size_t frame, std::size_t count, std::uint32_t seed)
{
    if (count <= 1) return 0;
    if (count > 10000000) throw std::invalid_argument("Unsupported camera focus clip length");
    const std::lock_guard<std::mutex> lock(mutex_);
    if (signals_.size() != count || seed_ != seed) {
        // Публикуем готовый сигнал целиком: ошибка выделения не оставляет частичный кэш.
        std::vector<float> values(count);
        float previous = 0;
        double sum = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const float noise = film::gaussianLattice(static_cast<int>(i), 0, seed ^ 0x464f4355u, 64u);
            previous = previous * 0.48f + noise * 0.52f;
            values[i] = previous;
            sum += previous;
        }
        const double mean = sum / count;
        double square = 0;
        for (const auto value : values) { const double difference = value - mean; square += difference * difference; }
        const double deviation = std::sqrt(square / count);
        for (auto& value : values)
            value = static_cast<float>(std::tanh((value - mean) / (deviation > 1e-8 ? deviation : 1.0)));
        signals_ = std::move(values);
        seed_ = seed;
    }
    return signals_[std::min(frame, count - 1)];
}

bool renderCameraOpticsCpu(ImageView source, ImageView output, RenderWindow window,
    const CameraOpticsParameters& parameters, AbortCallback abort, void* context)
{
    const int width = source.x2 - source.x1, height = source.y2 - source.y1;
    if (width <= 0 || height <= 0) return true;
    const auto count = static_cast<std::size_t>(width) * height;
    Plane signal(count), mask(count, 0.0f);
    for (int y = 0; y < height; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = 0; x < width; ++x) {
            auto pixel = readPixel(source, source.x1 + x, source.y1 + y);
            if (parameters.sourcePremultiplied) {
                if (pixel.a <= 0) pixel.r = pixel.g = pixel.b = 0;
                else { pixel.r /= pixel.a; pixel.g /= pixel.a; pixel.b /= pixel.a; }
            }
            const auto index = static_cast<std::size_t>(y) * width + x;
            // Линейные пространственные операции коммутируют с ЧБ-взвешиванием.
            signal[index] = std::clamp(pixel.r, 0.0f, 1.0f) * 0.299f
                + std::clamp(pixel.g, 0.0f, 1.0f) * 0.587f + std::clamp(pixel.b, 0.0f, 1.0f) * 0.114f;
            if (parameters.edgeSoftness > 0 || parameters.vignetteStrength > 0)
                mask[index] = optics::lensEdgeMask(x, y, 0, 0, width, height, parameters);
        }
    }
    signal = processCameraPlane(std::move(signal), width, height, mask, parameters, abort, context);
    if (signal.size() != count) return false;
    for (int y = std::max(window.y1, output.y1); y < std::min(window.y2, output.y2); ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = std::max(window.x1, output.x1); x < std::min(window.x2, output.x2); ++x) {
            Pixel pixel{};
            if (x >= source.x1 && x < source.x2 && y >= source.y1 && y < source.y2) {
                const float alpha = readPixel(source, x, y).a;
                float value = signal[static_cast<std::size_t>(y - source.y1) * width + (x - source.x1)];
                if (parameters.outputPremultiplied) value *= alpha;
                pixel = {value, value, value, alpha};
            }
            auto* address = output.data + (static_cast<std::ptrdiff_t>(y) - output.y1) * output.rowBytes
                + (static_cast<std::ptrdiff_t>(x) - output.x1) * sizeof(Pixel);
            std::memcpy(address, &pixel, sizeof(pixel));
        }
    }
    return true;
}

bool renderCameraCpu(ImageView source, ImageView output, RenderWindow window,
    const CameraOpticsParameters& parameters, float exposureGain, AbortCallback abort, void* context)
{
    const int width = source.x2 - source.x1;
    const int height = source.y2 - source.y1;
    if (width <= 0 || height <= 0) return true;
    const auto pixelCount = static_cast<std::size_t>(width) * height;
    std::array<Plane, 3> channels{Plane(pixelCount), Plane(pixelCount), Plane(pixelCount)};
    for (int y = 0; y < height; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = 0; x < width; ++x) {
            auto pixel = readPixel(source, source.x1 + x, source.y1 + y);
            if (parameters.sourcePremultiplied) {
                if (pixel.a <= 0) pixel.r = pixel.g = pixel.b = 0;
                else { pixel.r /= pixel.a; pixel.g /= pixel.a; pixel.b /= pixel.a; }
            }
            const auto index = static_cast<std::size_t>(y) * width + x;
            channels[0][index] = pixel.r; channels[1][index] = pixel.g; channels[2][index] = pixel.b;
        }
    }
    Plane edgeMask(pixelCount, 0.0f);
    if (parameters.vignetteStrength > 0 || parameters.edgeSoftness > 0) {
        for (int y = 0; y < height; ++y) {
            if (cancelled(abort, context)) return false;
            for (int x = 0; x < width; ++x) {
                edgeMask[static_cast<std::size_t>(y) * width + x] = optics::lensEdgeMask(
                    source.x1 + x, source.y1 + y, source.x1, source.y1, source.x2, source.y2, parameters);
            }
        }
    }
    for (auto& channel : channels) {
        channel = processCameraPlane(std::move(channel), width, height, edgeMask,
            parameters, abort, context);
        if (channel.size() != pixelCount) return false;
    }

    const int xBegin = std::max(window.x1, output.x1);
    const int xEnd = std::min(window.x2, output.x2);
    const int yBegin = std::max(window.y1, output.y1);
    const int yEnd = std::min(window.y2, output.y2);
    for (int y = yBegin; y < yEnd; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = xBegin; x < xEnd; ++x) {
            Pixel result{};
            const auto input = readPixel(source, x, y);
            if (x >= source.x1 && y >= source.y1 && x < source.x2 && y < source.y2) {
                const auto index = static_cast<std::size_t>(y - source.y1) * width + (x - source.x1);
                const auto encodeExposure = [exposureGain](float value) {
                    const float linear = std::clamp(film::decode(value, Transfer::SRGB) * exposureGain, 0.0f, 1.0f);
                    return film::encode(linear, Transfer::SRGB);
                };
                const float red = encodeExposure(channels[0][index]);
                const float green = encodeExposure(channels[1][index]);
                const float blue = encodeExposure(channels[2][index]);
                float value = red * 0.299f + green * 0.587f + blue * 0.114f;
                if (parameters.outputPremultiplied) value *= input.a;
                result = {value, value, value, input.a};
            }
            auto* destination = output.data + (static_cast<std::ptrdiff_t>(y) - output.y1) * output.rowBytes
                + (static_cast<std::ptrdiff_t>(x) - output.x1) * sizeof(Pixel);
            std::memcpy(destination, &result, sizeof(result));
        }
    }
    return true;
}

} // namespace silentfilm
