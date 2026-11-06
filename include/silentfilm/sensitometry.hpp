#pragma once

#include "silentfilm/probe.hpp"

#include <cmath>

#if defined(__CUDACC__)
#define SILENTFILM_FILM_HD __host__ __device__
#else
#define SILENTFILM_FILM_HD
#endif

namespace silentfilm {

enum class FilmStage : int { Negative, Print };
enum class Transfer : int { Linear, SRGB, Gamma24 };

struct EmulsionParameters {
    FilmStage stage = FilmStage::Negative;
    Transfer transfer = Transfer::Linear;
    float exposureEV = 0;
    float contrast = 1;
    float shadowSoftness = 0;
    float highlightSoftness = 0;
    float fog = 0;
    bool previewPositive = false;
    bool sourcePremultiplied = false;
    bool outputPremultiplied = false;
};

namespace film {

constexpr float blackFloor = 1e-4f;
constexpr float negativeGamma = 0.65f;
constexpr float negativeBase = 0.1f;
constexpr float negativeWhite = 2.7f;
constexpr float printBase = 0.02f;

/** Bound the intentionally normalized SDR material domain. */
SILENTFILM_FILM_HD inline float clamp(float value, float low, float high) noexcept
{
    return ::fminf(high, ::fmaxf(low, value));
}

/** Decode an explicitly selected SDR transfer without guessing the host color space. */
SILENTFILM_FILM_HD inline float decode(float value, Transfer transfer) noexcept
{
    if (transfer == Transfer::Linear) return value;
    if (transfer == Transfer::Gamma24) return ::powf(::fmaxf(value, 0.0f), 2.4f);
    return value <= 0.04045f ? value / 12.92f : ::powf((value + 0.055f) / 1.055f, 2.4f);
}

/** Encode the fixed positive viewing result into the selected output transfer. */
SILENTFILM_FILM_HD inline float encode(float value, Transfer transfer) noexcept
{
    if (transfer == Transfer::Linear) return value;
    if (transfer == Transfer::Gamma24) return ::powf(::fmaxf(value, 0.0f), 1.0f / 2.4f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * ::powf(value, 1.0f / 2.4f) - 0.055f;
}

/** Blend smooth toe and shoulder density limits around the straight-line response. */
SILENTFILM_FILM_HD inline float softLimits(float density, float floor, float ceiling,
    float lowKnee, float highKnee, float lowStrength, float highStrength) noexcept
{
    if (lowStrength > 0 && density < lowKnee) {
        const float span = lowKnee - floor;
        const float curved = lowKnee + span * ::expm1f((density - lowKnee) / span);
        density += lowStrength * (curved - density);
    }
    if (highStrength > 0 && density > highKnee) {
        const float span = ceiling - highKnee;
        const float curved = highKnee - span * ::expm1f(-(density - highKnee) / span);
        density += highStrength * (curved - density);
    }
    return density;
}

/** Convert optical density into bounded physical film transmittance. */
SILENTFILM_FILM_HD inline float transmittance(float density) noexcept
{
    return ::powf(10.0f, -clamp(density, 0.0f, 12.0f));
}

/** Expose and develop negative stock around the calibrated 18-percent gray pivot. */
SILENTFILM_FILM_HD inline float negativeDensity(float linear, const EmulsionParameters& parameters) noexcept
{
    const float grayLog = ::log10f(blackFloor + (1 - blackFloor) * 0.18f);
    const float grayDensity = negativeWhite + negativeGamma * grayLog;
    const float logExposure = ::log10f(blackFloor + (1 - blackFloor) * clamp(linear, 0, 1))
        + parameters.exposureEV * ::log10f(2.0f);
    const float density = grayDensity + negativeGamma * parameters.contrast * (logExposure - grayLog);
    return softLimits(density, negativeBase, negativeWhite,
        negativeWhite + negativeGamma * ::log10f(blackFloor + (1 - blackFloor) * 0.03f),
        negativeWhite + negativeGamma * ::log10f(blackFloor + (1 - blackFloor) * 0.5f),
        parameters.shadowSoftness, parameters.highlightSoftness) + 0.25f * parameters.fog;
}

/** Expose positive stock through the actual negative transmission and develop it. */
SILENTFILM_FILM_HD inline float printDensity(float transmission, const EmulsionParameters& parameters) noexcept
{
    const float grayLog = ::log10f(blackFloor + (1 - blackFloor) * 0.18f);
    const float grayDensity = printBase - grayLog;
    const float logExposure = ::log10f(::fmaxf(transmission, 1e-12f)) + parameters.exposureEV * ::log10f(2.0f);
    const float nominalDensity = printBase + (logExposure + negativeWhite) / negativeGamma;
    const float density = grayDensity + parameters.contrast * (nominalDensity - grayDensity);
    return softLimits(density, printBase, printBase - ::log10f(blackFloor),
        printBase - ::log10f(blackFloor + (1 - blackFloor) * 0.5f),
        printBase - ::log10f(blackFloor + (1 - blackFloor) * 0.03f),
        parameters.highlightSoftness, parameters.shadowSoftness) + 0.25f * parameters.fog;
}

/** Apply fixed reference viewing calibration with no image-dependent normalization. */
SILENTFILM_FILM_HD inline float viewPositive(float density, Transfer transfer) noexcept
{
    const float linear = (transmittance(density) * ::powf(10.0f, printBase) - blackFloor) / (1 - blackFloor);
    return clamp(encode(clamp(linear, 0, 1), transfer), 0, 1);
}

} // namespace film

/** Process a monochrome film stage while respecting the host alpha representation. */
SILENTFILM_FILM_HD inline Pixel emulsionPixel(Pixel source, const EmulsionParameters& parameters) noexcept
{
    const float alpha = source.a;
    if (parameters.sourcePremultiplied) {
        if (alpha <= 0) return {0, 0, 0, alpha};
        source.r /= alpha; source.g /= alpha; source.b /= alpha;
    }
    float value = 0;
    if (parameters.stage == FilmStage::Negative) {
        const float linear = film::decode(source.r, parameters.transfer) * 0.2126f
            + film::decode(source.g, parameters.transfer) * 0.7152f
            + film::decode(source.b, parameters.transfer) * 0.0722f;
        value = film::transmittance(film::negativeDensity(linear, parameters));
        if (parameters.previewPositive) {
            EmulsionParameters reference;
            value = film::viewPositive(film::printDensity(value, reference), parameters.transfer);
        }
    } else {
        // Пропускание негатива не декодируется как sRGB/Log, даже если так настроен viewer.
        const float transmission = source.r * 0.299f + source.g * 0.587f + source.b * 0.114f;
        value = film::viewPositive(film::printDensity(transmission, parameters), parameters.transfer);
    }
    if (parameters.outputPremultiplied) value *= alpha;
    return {value, value, value, alpha};
}

} // namespace silentfilm

#undef SILENTFILM_FILM_HD
