#pragma once

#include "silentfilm/probe.hpp"

#include <cmath>

#if defined(__CUDACC__)
#define SILENTFILM_OPTICS_HD __host__ __device__
#else
#define SILENTFILM_OPTICS_HD
#endif

namespace silentfilm {

struct CameraOpticsParameters {
    float vignetteStrength = 0;
    float edgeStart = 0.6f;
    float edgeEnd = 1.0f;
};

namespace optics {

/** Compute a circular mask using half the image width as the unit radius. */
SILENTFILM_OPTICS_HD inline float lensEdgeMask(int x, int y, int x1, int y1, int x2, int y2,
    const CameraOpticsParameters& parameters) noexcept
{
    const float width = static_cast<float>(x2 - x1);
    const float height = static_cast<float>(y2 - y1);
    const float centerOffsetX = (width - 1.0f) * 0.5f;
    const float halfWidth = ::fmaxf(centerOffsetX, 1.0f);
    const float halfHeight = (height - 1.0f) * 0.5f;
    const float centerX = static_cast<float>(x1) + centerOffsetX;
    const float centerY = static_cast<float>(y1) + halfHeight;
    const float dx = (static_cast<float>(x) - centerX) / halfWidth;
    const float dy = (static_cast<float>(y) - centerY) / halfWidth;
    const float radius = ::sqrtf(dx * dx + dy * dy);
    const float span = parameters.edgeEnd - parameters.edgeStart;
    const float transition = ::fminf(1.0f, ::fmaxf(0.0f, (radius - parameters.edgeStart) / span));
    return transition * transition * (3.0f - 2.0f * transition);
}

} // namespace optics

/** Apply static capture-lens falloff while preserving the host alpha channel. */
SILENTFILM_OPTICS_HD inline Pixel cameraPixel(Pixel source, int x, int y,
    int x1, int y1, int x2, int y2, const CameraOpticsParameters& parameters) noexcept
{
    if (parameters.vignetteStrength <= 0) return source;
    const float mask = optics::lensEdgeMask(x, y, x1, y1, x2, y2, parameters);
    const float gain = 1.0f - mask * parameters.vignetteStrength;
    return {source.r * gain, source.g * gain, source.b * gain, source.a};
}

} // namespace silentfilm

#undef SILENTFILM_OPTICS_HD
