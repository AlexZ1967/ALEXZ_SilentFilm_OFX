#include "silentfilm/film_optics.hpp"
#include "silentfilm/projector_pixel.hpp"
#include "silentfilm/grain.hpp"

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
    std::vector<float> incident(count), returned;
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
        returned.resize(count);
        const float sigma = static_cast<float>(width) * halationSize * 0.01f;
        if (!diffuseFilmPlane(incident.data(), returned.data(), width, height, sigma, abort, context)) return false;
        const float divisor = 1.0f + halationStrength;
        for (std::size_t i = 0; i < count; ++i)
            incident[i] = std::max(0.0f, (incident[i] + halationStrength * returned[i]) / divisor);
    }
    const Transfer outputTransfer = parameters.transfer;
    std::vector<float> grain;
    const float grainAmplitude = parameters.grainStrength * std::min(1.0f, parameters.grainSize * width / 1920.0f);
    if (grainAmplitude > 0) {
        const auto lattices = emulsionGrainLattices(width, height, parameters);
        if (!emulsionGrainField(lattices, width, height, grain, abort, context)) return false;
    }
    const bool outputPremultiplied = parameters.outputPremultiplied;
    parameters.transfer = Transfer::Linear;
    parameters.sourcePremultiplied = false;
    parameters.outputPremultiplied = false;
    parameters.grainStrength = 0;
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
            const auto index = static_cast<std::size_t>(y - source.y1) * width + x - source.x1;
            float density = parameters.stage == FilmStage::Negative
                ? film::negativeDensity(value, parameters) : film::printDensity(value, parameters);
            if (!grain.empty()) density += grain[index] * grainAmplitude;
            float signal = parameters.stage == FilmStage::Negative ? film::transmittance(density)
                : film::viewPositive(density, Transfer::Linear);
            if (parameters.stage == FilmStage::Negative && parameters.previewPositive) {
                EmulsionParameters reference;
                signal = film::viewPositive(film::printDensity(signal, reference), Transfer::Linear);
            }
            Pixel result{signal, signal, signal, p.a};
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
} // namespace silentfilm
