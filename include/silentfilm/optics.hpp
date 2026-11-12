#pragma once

#include "silentfilm/probe.hpp"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

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

/** Round a reduced dimension to nearest-even, matching Python round independently of the FP mode. */
inline int blurDimension(int length, double scale) noexcept
{
    const double value = length * scale;
    const auto lower = static_cast<int>(::floor(value));
    const double fraction = value - lower;
    const int rounded = lower + (fraction > 0.5 || (fraction == 0.5 && lower % 2 != 0) ? 1 : 0);
    return rounded > 0 ? rounded : 1;
}

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

/** Cache clip-normalized AR(1) focus independently of the order of frame requests. */
class CameraFocusCache {
    std::mutex mutex_;
    std::vector<float> signals_;
    std::uint32_t seed_ = 0;
public:
    /** Release reconstructible signals without changing future frame values. */
    void clear();
    float at(std::size_t frame, std::size_t frameCount, std::uint32_t seed);
};

/** Apply current-frame SDR camera optics with a single monochrome working plane and unchanged alpha. */
bool renderCameraOpticsCpu(ImageView source, ImageView output, RenderWindow window,
    const CameraOpticsParameters& parameters, AbortCallback abort, void* context);

/** Apply monochrome capture optics, variable edge blur, vignette and alpha conversion. */
bool renderCameraCpu(ImageView source, ImageView output, RenderWindow window,
    const CameraOpticsParameters& parameters, float exposureGain, AbortCallback abort, void* context);

} // namespace silentfilm
