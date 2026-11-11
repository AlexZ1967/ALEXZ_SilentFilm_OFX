#pragma once

#include "silentfilm/probe.hpp"

#include <cmath>

namespace silentfilm {

struct CameraOpticsParameters {
    float vignetteStrength = 0;
    float edgeSoftness = 0;
    float edgeStart = 0.6f;
    float edgeEnd = 1.0f;
    float softness = 0;
    float focusDrift = 0;
    float focusSignal = 0;
    bool sourcePremultiplied = false;
    bool outputPremultiplied = false;
    // Промежуточный RGB после выдержки, до оптики/экспозиции/монохрома.
    bool captureOnly = false;
};

struct ImageView;
struct RenderWindow;
using AbortCallback = bool (*)(void* context);

namespace optics {

/** Compute a circular mask using half the image width as the unit radius. */
#if defined(__CUDACC__)
__host__ __device__
#endif
inline float lensEdgeMask(int x, int y, int x1, int y1, int x2, int y2,
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

/** Apply monochrome capture optics, variable edge blur, vignette and alpha conversion. */
bool renderCameraCpu(ImageView source, ImageView output, RenderWindow window,
    const CameraOpticsParameters& parameters, float exposureGain, AbortCallback abort, void* context);

} // namespace silentfilm
