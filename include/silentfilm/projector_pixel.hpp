#pragma once

#include "silentfilm/projector.hpp"

#if defined(__CUDACC__)
#define SILENTFILM_PROJECTOR_HD __host__ __device__
#else
#define SILENTFILM_PROJECTOR_HD
#endif

namespace silentfilm::projection {

SILENTFILM_PROJECTOR_HD inline Pixel read(ImageView image, int x, int y, bool premultiplied)
{
    x = x < image.x1 ? image.x1 : x >= image.x2 ? image.x2 - 1 : x;
    y = y < image.y1 ? image.y1 : y >= image.y2 ? image.y2 - 1 : y;
    const auto* row = image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes;
    auto value = *reinterpret_cast<const Pixel*>(row + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel));
    if (premultiplied) {
        if (value.a <= 0) value.r = value.g = value.b = 0;
        else { value.r /= value.a; value.g /= value.a; value.b /= value.a; }
    }
    value.r = ::fminf(1, ::fmaxf(0, value.r));
    value.g = ::fminf(1, ::fmaxf(0, value.g));
    value.b = ::fminf(1, ::fmaxf(0, value.b));
    return value;
}

SILENTFILM_PROJECTOR_HD inline Pixel blend(Pixel a, Pixel b, float t)
{
    return {a.r * (1 - t) + b.r * t, a.g * (1 - t) + b.g * t,
        a.b * (1 - t) + b.b * t, a.a * (1 - t) + b.a * t};
}

/** Match align_corners=False translation and bilinear border sampling of Comfy gate weave. */
SILENTFILM_PROJECTOR_HD inline Pixel transport(ImageView source, int x, int y, const ProjectorParameters& p)
{
    const float sx = ::fminf(static_cast<float>(source.x2 - 1), ::fmaxf(static_cast<float>(source.x1), x + p.frame.shiftX));
    const float sy = ::fminf(static_cast<float>(source.y2 - 1), ::fmaxf(static_cast<float>(source.y1), y + p.frame.shiftY));
    const int ix = static_cast<int>(::floorf(sx)), iy = static_cast<int>(::floorf(sy));
    const float fx = sx - ix, fy = sy - iy;
    if (fx == 0 && fy == 0) return read(source, ix, iy, p.sourcePremultiplied);
    return blend(blend(read(source, ix, iy, p.sourcePremultiplied), read(source, ix + 1, iy, p.sourcePremultiplied), fx),
        blend(read(source, ix, iy + 1, p.sourcePremultiplied), read(source, ix + 1, iy + 1, p.sourcePremultiplied), fx), fy);
}

/** Illuminate straight RGB before scattering; alpha belongs to the transported positive. */
SILENTFILM_PROJECTOR_HD inline Pixel illuminate(Pixel rgb, const ProjectorParameters& p)
{
    return {film::decode(rgb.r, p.transfer) * p.frame.lampGain,
        film::decode(rgb.g, p.transfer) * p.frame.lampGain,
        film::decode(rgb.b, p.transfer) * p.frame.lampGain, rgb.a};
}

SILENTFILM_PROJECTOR_HD inline Pixel screen(Pixel linear, const ProjectorParameters& p)
{
    const float r = film::encode(::fminf(1, ::fmaxf(0, linear.r)), p.transfer);
    const float g = film::encode(::fminf(1, ::fmaxf(0, linear.g)), p.transfer);
    const float b = film::encode(::fminf(1, ::fmaxf(0, linear.b)), p.transfer);
    const float factor = p.outputPremultiplied ? linear.a : 1;
    return {r * factor, g * factor, b * factor, linear.a};
}

} // namespace silentfilm::projection

#undef SILENTFILM_PROJECTOR_HD
