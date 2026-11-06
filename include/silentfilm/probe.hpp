#pragma once

namespace silentfilm {

struct Pixel {
    float r, g, b, a;
};

static_assert(sizeof(Pixel) == 4 * sizeof(float), "RGBA pixels must contain four packed floats");

#if defined(__CUDACC__)
#define SILENTFILM_HOST_DEVICE __host__ __device__
#else
#define SILENTFILM_HOST_DEVICE
#endif

/** Blend diagnostic temporal samples and apply gain without clipping HDR values. */
SILENTFILM_HOST_DEVICE inline Pixel probePixel(Pixel current, Pixel previous, Pixel next, float gain, float temporalMix) noexcept
{
    if (temporalMix == 0.0f) {
        return {current.r * gain, current.g * gain, current.b * gain, current.a};
    }
    const float center = 1.0f - temporalMix * 0.5f;
    const float side = temporalMix * 0.25f;
    return {
        (current.r * center + previous.r * side + next.r * side) * gain,
        (current.g * center + previous.g * side + next.g * side) * gain,
        (current.b * center + previous.b * side + next.b * side) * gain,
        current.a * center + previous.a * side + next.a * side,
    };
}

#undef SILENTFILM_HOST_DEVICE

/** Convert diagnostic EV into an RGB multiplier once per render. */
float gainFromEV(double value) noexcept;

} // namespace silentfilm
