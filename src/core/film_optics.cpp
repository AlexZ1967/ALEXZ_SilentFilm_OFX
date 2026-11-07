#include "silentfilm/film_optics.hpp"
#include "silentfilm/projector_pixel.hpp"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace silentfilm {
namespace {

bool cancelled(AbortCallback abort, void* context) { return abort && abort(context); }

std::mutex fftwPlannerMutex;

struct FftwPlanDeleter {
    void operator()(fftwf_plan_s* plan) const
    {
        if (!plan) return;
        const std::lock_guard<std::mutex> lock(fftwPlannerMutex);
        fftwf_destroy_plan(plan);
    }
};

Pixel readPixel(ImageView image, int x, int y)
{
    if (x < image.x1 || y < image.y1 || x >= image.x2 || y >= image.y2) return {};
    Pixel value{};
    const auto* address = image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel);
    std::memcpy(&value, address, sizeof(value));
    return value;
}

void writePixel(ImageView image, int x, int y, Pixel value)
{
    auto* address = image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel);
    std::memcpy(address, &value, sizeof(value));
}

bool diffuseAxis(const float* source, float* output, int width, int height, bool horizontal,
    float sigma, AbortCallback abort, void* context)
{
    const int count = horizontal ? width : height;
    if (count <= 1) {
        std::copy(source, source + static_cast<std::size_t>(width) * height, output);
        return true;
    }
    if (count > std::numeric_limits<int>::max() / 2) return false;
    const int transformSize = 2 * count;
    auto input = std::unique_ptr<float, decltype(&fftwf_free)>(fftwf_alloc_real(static_cast<std::size_t>(transformSize)), &fftwf_free);
    auto spectrum = std::unique_ptr<float, decltype(&fftwf_free)>(
        reinterpret_cast<float*>(fftwf_alloc_complex(static_cast<std::size_t>(transformSize / 2 + 1))), &fftwf_free);
    if (!input || !spectrum) return false;
    std::unique_ptr<fftwf_plan_s, FftwPlanDeleter> forward, inverse;
    {
        const std::lock_guard<std::mutex> lock(fftwPlannerMutex);
        auto* complexSpectrum = reinterpret_cast<fftwf_complex*>(spectrum.get());
        forward.reset(fftwf_plan_dft_r2c_1d(transformSize, input.get(), complexSpectrum, FFTW_ESTIMATE));
        inverse.reset(fftwf_plan_dft_c2r_1d(transformSize, complexSpectrum, input.get(), FFTW_ESTIMATE));
    }
    if (!forward || !inverse) return false;
    const int lines = horizontal ? height : width;
    for (int line = 0; line < lines; ++line) {
        if (cancelled(abort, context)) {
            return false;
        }
        for (int i = 0; i < count; ++i) {
            const auto first = horizontal ? static_cast<std::size_t>(line) * width + i
                : static_cast<std::size_t>(i) * width + line;
            const auto second = horizontal ? static_cast<std::size_t>(line) * width + (count - 1 - i)
                : static_cast<std::size_t>(count - 1 - i) * width + line;
            input.get()[i] = source[first];
            input.get()[count + i] = source[second];
        }
        fftwf_execute(forward.get());
        for (int k = 0; k <= transformSize / 2; ++k) {
            const float frequency = static_cast<float>(k) / transformSize;
            const float sine = std::sin(3.14159265358979323846f * frequency);
            const float response = std::exp(-2.0f * (sigma * sine) * (sigma * sine));
            spectrum.get()[2 * k] *= response;
            spectrum.get()[2 * k + 1] *= response;
        }
        fftwf_execute(inverse.get());
        for (int i = 0; i < count; ++i) {
            const auto target = horizontal ? static_cast<std::size_t>(line) * width + i
                : static_cast<std::size_t>(i) * width + line;
            output[target] = input.get()[i] / transformSize;
        }
    }
    return true;
}

float luma(float r, float g, float b, Transfer transfer)
{
    const float linear = film::decode(r, transfer) * 0.2126f
        + film::decode(g, transfer) * 0.7152f
        + film::decode(b, transfer) * 0.0722f;
    return linear;
}

} // namespace

bool diffuseFilmPlane(const float* source, float* output, int width, int height, float sigma,
    AbortCallback abort, void* context)
{
    if (!source || !output || width <= 0 || height <= 0 || !std::isfinite(sigma) || sigma < 0) return false;
    if (sigma == 0) {
        std::copy(source, source + static_cast<std::size_t>(width) * height, output);
        return true;
    }
    std::vector<float> horizontal(static_cast<std::size_t>(width) * height);
    return diffuseAxis(source, horizontal.data(), width, height, true, sigma, abort, context)
        && diffuseAxis(horizontal.data(), output, width, height, false, sigma, abort, context);
}

bool renderEmulsionCpu(ImageView source, ImageView output, RenderWindow window,
    EmulsionParameters parameters, float halationStrength, float halationSize,
    AbortCallback abort, void* context)
{
    const int width = source.x2 - source.x1, height = source.y2 - source.y1;
    if (width <= 0 || height <= 0) return true;
    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<float> incident(count), returned(count);
    for (int y = 0; y < height; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = 0; x < width; ++x) {
            auto p = readPixel(source, source.x1 + x, source.y1 + y);
            if (parameters.sourcePremultiplied) {
                if (p.a <= 0) p.r = p.g = p.b = 0;
                else { p.r /= p.a; p.g /= p.a; p.b /= p.a; }
            }
            float value;
            if (parameters.stage == FilmStage::Negative) {
                value = luma(p.r, p.g, p.b, parameters.transfer);
            } else {
                value = p.r * 0.299f + p.g * 0.587f + p.b * 0.114f;
            }
            incident[static_cast<std::size_t>(y) * width + x] = value;
        }
    }
    if (halationStrength > 0) {
        const float sigma = static_cast<float>(width) * halationSize * 0.01f;
        if (!diffuseFilmPlane(incident.data(), returned.data(), width, height, sigma, abort, context)) return false;
        const float divisor = 1.0f + halationStrength;
        for (std::size_t i = 0; i < count; ++i)
            incident[i] = std::max(0.0f, (incident[i] + halationStrength * returned[i]) / divisor);
    }
    const Transfer outputTransfer = parameters.transfer;
    const bool outputPremultiplied = parameters.outputPremultiplied;
    parameters.transfer = Transfer::Linear;
    parameters.sourcePremultiplied = false;
    parameters.outputPremultiplied = false;
    const int x1 = std::max(window.x1, output.x1), x2 = std::min(window.x2, output.x2);
    const int y1 = std::max(window.y1, output.y1), y2 = std::min(window.y2, output.y2);
    for (int y = y1; y < y2; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = x1; x < x2; ++x) {
            if (x < source.x1 || x >= source.x2 || y < source.y1 || y >= source.y2) {
                writePixel(output, x, y, {});
                continue;
            }
            const auto p = readPixel(source, x, y);
            const float value = incident[static_cast<std::size_t>(y - source.y1) * width + (x - source.x1)];
            Pixel result = emulsionPixel({value, value, value, p.a}, parameters, x, y, width);
            if (parameters.stage == FilmStage::Print || parameters.previewPositive) {
                result.r = result.g = result.b = film::encode(result.r, outputTransfer);
            }
            if (outputPremultiplied) {
                result.r *= p.a; result.g *= p.a; result.b *= p.a;
            }
            writePixel(output, x, y, result);
        }
    }
    return true;
}

bool renderProjectorCpu(ImageView source, ImageView output, RenderWindow window,
    const ProjectorParameters& p, AbortCallback abort, void* context)
{
    const int width = source.x2 - source.x1, height = source.y2 - source.y1;
    if (width <= 0 || height <= 0) return true;
    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<Pixel> incident;
    std::vector<float> blurred;
    if (p.glowStrength > 0) {
        incident.resize(count); blurred.resize(count * 3);
        for (int y = 0; y < height; ++y) {
            if (cancelled(abort, context)) return false;
            for (int x = 0; x < width; ++x)
                incident[static_cast<std::size_t>(y) * width + x] = projection::illuminate(
                    projection::transport(source, source.x1 + x, source.y1 + y, p), p);
        }
        const float sigma = static_cast<float>(width) * p.glowSize * 0.01f;
        std::vector<float> plane(count), result(count);
        for (int channel = 0; channel < 3; ++channel) {
            for (std::size_t i = 0; i < count; ++i)
                plane[i] = channel == 0 ? incident[i].r : channel == 1 ? incident[i].g : incident[i].b;
            if (!diffuseFilmPlane(plane.data(), result.data(), width, height, sigma, abort, context)) return false;
            std::copy(result.begin(), result.end(), blurred.begin() + channel * count);
        }
    }
    const int x1 = std::max(window.x1, output.x1), x2 = std::min(window.x2, output.x2);
    const int y1 = std::max(window.y1, output.y1), y2 = std::min(window.y2, output.y2);
    for (int y = y1; y < y2; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = x1; x < x2; ++x) {
            if (x < source.x1 || x >= source.x2 || y < source.y1 || y >= source.y2) {
                writePixel(output, x, y, {}); continue;
            }
            Pixel linear;
            if (p.glowStrength > 0) {
                const auto i = static_cast<std::size_t>(y - source.y1) * width + x - source.x1;
                linear = incident[i];
                linear.r = linear.r * (1 - p.glowStrength) + blurred[i] * p.glowStrength;
                linear.g = linear.g * (1 - p.glowStrength) + blurred[count + i] * p.glowStrength;
                linear.b = linear.b * (1 - p.glowStrength) + blurred[2 * count + i] * p.glowStrength;
            } else linear = projection::illuminate(projection::transport(source, x, y, p), p);
            writePixel(output, x, y, projection::screen(linear, p));
        }
    }
    return true;
}

bool renderDefectsCpu(ImageView source, ImageView output, RenderWindow window,
    const DefectParameters& p, AbortCallback abort, void* context)
{
    const int width = source.x2 - source.x1, height = source.y2 - source.y1;
    if (width <= 0 || height <= 0) return true;
    const int copyX1 = std::max(window.x1, output.x1), copyX2 = std::min(window.x2, output.x2);
    const int copyY1 = std::max(window.y1, output.y1), copyY2 = std::min(window.y2, output.y2);
    if (p.strength <= 0) {
        for (int y = copyY1; y < copyY2; ++y) {
            if (cancelled(abort, context)) return false;
            for (int x = copyX1; x < copyX2; ++x) {
                const auto pixel = readPixel(source, x, y);
                if (p.sourcePremultiplied == p.outputPremultiplied) writePixel(output, x, y, pixel);
                else if (p.outputPremultiplied) writePixel(output, x, y, {pixel.r * pixel.a, pixel.g * pixel.a, pixel.b * pixel.a, pixel.a});
                else if (pixel.a > 0) writePixel(output, x, y, {pixel.r / pixel.a, pixel.g / pixel.a, pixel.b / pixel.a, pixel.a});
                else writePixel(output, x, y, {});
            }
        }
        return true;
    }
    const int frame = static_cast<int>(std::floor(p.time * p.frameRate));
    const int event = static_cast<int>(std::floor(static_cast<double>(frame) / std::max(1, p.persistence)));
    const int wholeParticles = std::clamp(static_cast<int>(std::floor(p.amount)), 0, 64);
    const float fractionalParticle = p.amount - wholeParticles;
    const int particles = std::min(64, wholeParticles + (fractionalParticle > 0 && wholeParticles < 64 ? 1 : 0));
    const float radius = std::max(0.35f, width * p.size * 0.01f * 0.5f);
    const bool negative = p.material == DefectMaterial::Negative;
    const bool physicalInput = negative || p.material == DefectMaterial::Printing;
    const bool encodedMaterial = !physicalInput;
    const float baseClear = negative ? 0.79432823f : 1.0f;
    const auto hash = [](std::uint32_t value) {
        value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15;
        value *= 0x846ca68bu; value ^= value >> 16; return value;
    };
    const auto unit = [&hash](std::uint32_t value) {
        return static_cast<float>(hash(value) >> 8) * (1.0f / 16777216.0f);
    };
    constexpr std::uint32_t algorithmVersion = 1u;
    const std::uint32_t base = p.seed ^ hash(static_cast<std::uint32_t>(event))
        ^ (algorithmVersion * 0x27d4eb2du);
    const float x1 = static_cast<float>(source.x1), y1 = static_cast<float>(source.y1);
    const int rx1 = std::max(window.x1, output.x1), rx2 = std::min(window.x2, output.x2);
    const int ry1 = std::max(window.y1, output.y1), ry2 = std::min(window.y2, output.y2);
    for (int y = ry1; y < ry2; ++y) {
        if (cancelled(abort, context)) return false;
        for (int x = rx1; x < rx2; ++x) {
            if (x < source.x1 || x >= source.x2 || y < source.y1 || y >= source.y2) {
                writePixel(output, x, y, {}); continue;
            }
            auto original = readPixel(source, x, y), pixel = original;
            if (p.sourcePremultiplied) {
                if (pixel.a <= 0) pixel.r = pixel.g = pixel.b = 0;
                else { pixel.r /= pixel.a; pixel.g /= pixel.a; pixel.b /= pixel.a; }
            }
            if (encodedMaterial) {
                pixel.r = film::decode(pixel.r, p.transfer);
                pixel.g = film::decode(pixel.g, p.transfer);
                pixel.b = film::decode(pixel.b, p.transfer);
            }
            float coverage = 0;
            const float px = x - x1 + 0.5f, py = y - y1 + 0.5f;
            if (p.pattern == DefectPattern::Development) {
                const float cell = std::max(2.0f, width * p.size * 0.01f);
                const int gx = static_cast<int>(std::floor(px / cell)), gy = static_cast<int>(std::floor(py / cell));
                const float fx = px / cell - gx, fy = py / cell - gy;
                const auto valueAt = [&](int ix, int iy) { return unit(base ^ (static_cast<std::uint32_t>(ix) * 0x9e3779b9u)
                    ^ (static_cast<std::uint32_t>(iy) * 0x85ebca6bu)) * 2.0f - 1.0f; };
                const float a = valueAt(gx, gy), b = valueAt(gx + 1, gy), c = valueAt(gx, gy + 1), d = valueAt(gx + 1, gy + 1);
                const float smoothX = fx * fx * (3 - 2 * fx), smoothY = fy * fy * (3 - 2 * fy);
                const float top = a + (b - a) * smoothX, bottom = c + (d - c) * smoothX;
                const float field = top + (bottom - top) * smoothY;
                const float density = p.strength * (0.35f * field + 0.3f * (field + 1) * 0.5f);
                const float multiplier = std::pow(10.0f, -density);
                pixel.r *= multiplier; pixel.g *= multiplier; pixel.b *= multiplier;
            } else {
                for (int i = 0; i < particles; ++i) {
                    const std::uint32_t key = base ^ (static_cast<std::uint32_t>(i) * 0xc2b2ae35u);
                    if (i >= wholeParticles && unit(key ^ 0xa511e9b3u) >= fractionalParticle) continue;
                    const float cx = unit(key) * width, cy = unit(key ^ 0x68bc21ebu) * height;
                    const float sizeScale = 0.7f + 0.7f * unit(key ^ 0x02e5be93u);
                    const float dx = px - cx, dy = py - cy;
                    float mark = 0;
                    if (p.pattern == DefectPattern::Scratches) {
                        const float drift = (unit(key ^ 0x51ed270bu) - 0.5f) * width * 0.016f;
                        const float center = cx + drift * (py / std::max(1.0f, static_cast<float>(height)) - 0.5f);
                        mark = std::clamp(0.5f + radius * sizeScale - std::abs(px - center), 0.0f, 1.0f);
                    } else {
                        const float r = std::sqrt(dx * dx + dy * dy) / (radius * sizeScale);
                        if (p.pattern == DefectPattern::Stains) mark = std::exp(-1.5f * r * r);
                        else mark = std::clamp(1.5f - r, 0.0f, 1.0f);
                    }
                    coverage = std::max(coverage, mark);
                }
                coverage *= p.strength;
                if (negative && p.pattern == DefectPattern::Scratches) {
                    pixel.r += (std::max(baseClear, pixel.r) - pixel.r) * coverage;
                    pixel.g += (std::max(baseClear, pixel.g) - pixel.g) * coverage;
                    pixel.b += (std::max(baseClear, pixel.b) - pixel.b) * coverage;
                } else {
                    const float multiplier = 1.0f - coverage;
                    pixel.r *= multiplier; pixel.g *= multiplier; pixel.b *= multiplier;
                }
            }
            if (encodedMaterial) {
                pixel.r = film::encode(pixel.r, p.transfer);
                pixel.g = film::encode(pixel.g, p.transfer);
                pixel.b = film::encode(pixel.b, p.transfer);
            }
            if (p.outputPremultiplied) {
                pixel.r *= pixel.a; pixel.g *= pixel.a; pixel.b *= pixel.a;
            }
            writePixel(output, x, y, {pixel.r, pixel.g, pixel.b, original.a});
        }
    }
    return true;
}

} // namespace silentfilm
