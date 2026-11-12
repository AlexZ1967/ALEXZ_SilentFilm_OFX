#pragma once

#include "silentfilm/film_optics.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace silentfilm {

#if defined(__CUDACC__)
#define SILENTFILM_CRANK_HD __host__ __device__
#else
#define SILENTFILM_CRANK_HD
#endif

SILENTFILM_CRANK_HD inline float crankExposureChannel(float value, float factor)
{
    return film::encode(::fminf(1.0f, ::fmaxf(0.0f, film::decode(value, Transfer::SRGB) * factor)), Transfer::SRGB);
}

struct CrankExposureMap {
    double frameRate = 25;
    bool baked = false;
    std::vector<float> gains;
    /** Resolve one output-frame gain, rejecting unsupported retiming or an incorrect offset. */
    float at(double relativeFrame, int offset, double hostFrameRate) const;
};

/** Persist exact capture gains with explicit bake state and output timing. */
void writeCrankExposureMap(const std::string& path, double fps, const std::vector<float>& gains, bool baked);
/** Read and validate a versioned map; baked maps cannot be applied a second time. */
CrankExposureMap readCrankExposureMap(const std::string& path);

class CrankExposureMapCache {
    std::mutex mutex_;
    std::string path_;
    std::shared_ptr<const CrankExposureMap> map_;
public:
    /** Retain an immutable per-instance map, independent of frame request order. */
    std::shared_ptr<const CrankExposureMap> load(const std::string& path);
    /** Drop cached data without invalidating readers already rendering. */
    void clear();
};

/** Apply SDR exposure in linear light and preserve alpha and neutral pixels exactly. */
SILENTFILM_CRANK_HD inline Pixel crankExposurePixel(Pixel p, float gain, float blend, bool sourcePremultiplied, bool outputPremultiplied)
{
    if (gain == 1 || blend == 0) {
        if (sourcePremultiplied == outputPremultiplied) return p;
    }
    if (sourcePremultiplied) {
        if (p.a > 0) { p.r /= p.a; p.g /= p.a; p.b /= p.a; }
        else p.r = p.g = p.b = 0;
    }
    if (gain != 1 && blend != 0) {
        const float factor = 1 + blend * (gain - 1);
        p.r = crankExposureChannel(p.r, factor); p.g = crankExposureChannel(p.g, factor); p.b = crankExposureChannel(p.b, factor);
    }
    if (outputPremultiplied) { p.r *= p.a; p.g *= p.a; p.b *= p.a; }
    return p;
}

bool renderCrankExposureCpu(ImageView source, ImageView output, RenderWindow window, float gain, float blend,
    bool sourcePremultiplied, bool outputPremultiplied, AbortCallback abort, void* context);
CudaStatus renderCudaCrankExposure(ImageView source, ImageView output, RenderWindow window, float gain, float blend,
    bool sourcePremultiplied, bool outputPremultiplied, void* stream, bool hostStreamProvided) noexcept;

} // namespace silentfilm

#undef SILENTFILM_CRANK_HD
