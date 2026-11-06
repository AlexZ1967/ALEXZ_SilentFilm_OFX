#include "silentfilm/optics.hpp"
#include "silentfilm/cuda_backend.hpp"
#include "silentfilm/probe.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace silentfilm {
namespace {

using Plane = std::vector<float>;

bool cancelled(AbortCallback abort, void* context)
{
    return abort && abort(context);
}

Pixel readPixel(ImageView image, int x, int y)
{
    if (x < image.x1 || y < image.y1 || x >= image.x2 || y >= image.y2) return {};
    Pixel pixel{};
    const auto* address = image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel);
    std::memcpy(&pixel, address, sizeof(pixel));
    return pixel;
}

float luma(Pixel pixel, bool premultiplied)
{
    if (premultiplied) {
        if (pixel.a <= 0) return 0;
        pixel.r /= pixel.a;
        pixel.g /= pixel.a;
        pixel.b /= pixel.a;
    }
    return pixel.r * 0.299f + pixel.g * 0.587f + pixel.b * 0.114f;
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
        const float yBegin = static_cast<float>(y) * sourceHeight / outputHeight;
        const float yEnd = static_cast<float>(y + 1) * sourceHeight / outputHeight;
        const int firstY = static_cast<int>(std::floor(yBegin));
        const int lastY = static_cast<int>(std::ceil(yEnd));
        for (int x = 0; x < outputWidth; ++x) {
            const float xBegin = static_cast<float>(x) * sourceWidth / outputWidth;
            const float xEnd = static_cast<float>(x + 1) * sourceWidth / outputWidth;
            const int firstX = static_cast<int>(std::floor(xBegin));
            const int lastX = static_cast<int>(std::ceil(xEnd));
            float sum = 0;
            for (int sy = firstY; sy < lastY; ++sy) {
                const float overlapY = std::max(0.0f, std::min(yEnd, static_cast<float>(sy + 1))
                    - std::max(yBegin, static_cast<float>(sy)));
                for (int sx = firstX; sx < lastX; ++sx) {
                    const float overlapX = std::max(0.0f, std::min(xEnd, static_cast<float>(sx + 1))
                        - std::max(xBegin, static_cast<float>(sx)));
                    sum += source[static_cast<std::size_t>(sy) * sourceWidth + sx] * overlapX * overlapY;
                }
            }
            output[static_cast<std::size_t>(y) * outputWidth + x] = sum / ((xEnd - xBegin) * (yEnd - yBegin));
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
    const float scale = std::min(1.0f, 3.0f / sigma);
    const int smallWidth = scale < 1 ? std::max(1, static_cast<int>(std::round(width * scale))) : width;
    const int smallHeight = scale < 1 ? std::max(1, static_cast<int>(std::round(height * scale))) : height;
    Plane reduced;
    if (scale < 1) {
        if (!areaResize(source, width, height, reduced, smallWidth, smallHeight, abort, context)) return false;
    } else {
        reduced = source;
    }

    const float smallSigma = sigma * scale;
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

bool renderCameraCpu(ImageView source, ImageView output, RenderWindow window,
    const CameraOpticsParameters& parameters, AbortCallback abort, void* context)
{
    const int width = source.x2 - source.x1;
    const int height = source.y2 - source.y1;
    if (width <= 0 || height <= 0) return true;
    const auto pixelCount = static_cast<std::size_t>(width) * height;
    Plane gray(pixelCount);
    for (int y = 0; y < height; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = 0; x < width; ++x) {
            gray[static_cast<std::size_t>(y) * width + x] = luma(readPixel(source, source.x1 + x, source.y1 + y), parameters.sourcePremultiplied);
        }
    }

    Plane accumulated(pixelCount, 0.0f);
    const float maximumSigma = static_cast<float>(width) / 40.0f * parameters.edgeSoftness;
    std::vector<float> levels{0.0f};
    if (maximumSigma > 0) {
        float sigma = std::min(0.5f, maximumSigma);
        while (sigma < maximumSigma) {
            levels.push_back(sigma);
            sigma *= 2.0f;
        }
        levels.push_back(maximumSigma);
    }

    for (std::size_t level = 0; level < levels.size(); ++level) {
        Plane blurred;
        if (level == 0) blurred = gray;
        else if (!gaussianBlur(gray, width, height, levels[level], blurred, abort, context)) return false;
        for (int y = 0; y < height; ++y) {
            if (cancelled(abort, context)) return false;
            const int absoluteY = source.y1 + y;
            for (int x = 0; x < width; ++x) {
                const int absoluteX = source.x1 + x;
                const float mask = optics::lensEdgeMask(absoluteX, absoluteY,
                    source.x1, source.y1, source.x2, source.y2, parameters);
                const float weight = levelWeight(levels, level, mask * maximumSigma);
                const auto index = static_cast<std::size_t>(y) * width + x;
                accumulated[index] += blurred[index] * weight;
            }
        }
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
                const float mask = optics::lensEdgeMask(x, y, source.x1, source.y1, source.x2, source.y2, parameters);
                float value = accumulated[index] * (1.0f - mask * parameters.vignetteStrength);
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
