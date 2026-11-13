#pragma once

#include "silentfilm/film_optics.hpp"

#if defined(__CUDACC__)
#define SILENTFILM_DEFECT_HD __host__ __device__
#else
#define SILENTFILM_DEFECT_HD
#endif

namespace silentfilm {

struct DefectParticle {
    float x = 0, y = 0, radius = 0, drift = 0;
    float cosine = 1, sine = 0, aspect = 1, phase = 0;
    float opacity = 1, gapTop = 1, gapBottom = 1, support = 0;
};

/** Bounded per-frame coarse field; pointers are local to the CPU action or CUDA stream. */
struct DefectDevelopmentField {
    const float* values = nullptr;
    int width = 1, height = 1, columns = 2, rows = 2;
    int mottlingCount = 0, count = 0;
    std::uint32_t knot = 0;
    float weight = 0;
};

/** Small immutable per-frame geometry; never contains a host image pointer. */
struct DefectFrame {
    DefectParameters parameters;
    DefectParticle particles[64]{};
    int count = 0;
    float cell = 2;
    std::uint32_t base = 0;
    float effectiveAmount = 0;
    DefectDevelopmentField development;
};

/** Evaluate particle placement once per frame instead of once per pixel. */
DefectFrame prepareDefects(const DefectParameters& parameters, int width, int height);

namespace defect {

/** Rasterize the Comfy speck/stain and scratch equations for prepared geometry. */
SILENTFILM_DEFECT_HD inline float particleCoverage(float px, float py, int height,
    const DefectParticle& particle, const DefectParameters& p) noexcept
{
    if (p.pattern == DefectPattern::Scratches) {
        const float t = py / ::fmaxf(1, static_cast<float>(height));
        if (px < ::floorf(particle.y - particle.support - 2) + 0.5f
            || px >= ::ceilf(particle.y + particle.support + 2) + 0.5f) return 0;
        const float center = particle.x + particle.drift * (t - 0.5f);
        const float line = film::clamp(particle.radius + 0.5f - ::fabsf(px - center), 0, 1);
        const float gaps = film::clamp((particle.gapTop + (particle.gapBottom - particle.gapTop) * t) * 2.5f, 0, 1);
        return line * ((1 - p.irregularity) + p.irregularity * gaps) * particle.opacity;
    }
    const float dx = px - particle.x, dy = py - particle.y;
    if (::fabsf(dx) > particle.support + 2 || ::fabsf(dy) > particle.support + 2) return 0;
    if (px < ::floorf(particle.x - particle.support - 2) + 0.5f
        || px >= ::ceilf(particle.x + particle.support + 2) + 0.5f
        || py < ::floorf(particle.y - particle.support - 2) + 0.5f
        || py >= ::ceilf(particle.y + particle.support + 2) + 0.5f) return 0;
    const float u = dx * particle.cosine + dy * particle.sine;
    const float v = (-dx * particle.sine + dy * particle.cosine) / particle.aspect;
    const float theta = ::atan2f(v, u);
    const float edge = particle.radius * (1 + p.irregularity
        * (0.16f * ::sinf(3 * theta + particle.phase) + 0.09f * ::cosf(5 * theta - particle.phase)));
    const float distanceSquared = u * u + v * v;
    const float mark = p.pattern == DefectPattern::Stains
        ? ::expf(-1.5f * distanceSquared / (::fmaxf(edge, 0.1f) * ::fmaxf(edge, 0.1f)))
        : film::clamp(edge + 0.5f - ::sqrtf(distanceSquared), 0, 1);
    return mark * particle.opacity;
}

SILENTFILM_DEFECT_HD inline bool neutral(const DefectParameters& p) noexcept
{
    return p.blend <= 0 || p.strength <= 0 || (p.pattern != DefectPattern::Development && p.amount <= 0);
}

SILENTFILM_DEFECT_HD inline Pixel convertAlpha(Pixel pixel, const DefectParameters& p) noexcept
{
    if (p.sourcePremultiplied == p.outputPremultiplied) return pixel;
    if (p.outputPremultiplied) return {pixel.r * pixel.a, pixel.g * pixel.a, pixel.b * pixel.a, pixel.a};
    if (pixel.a > 0) return {pixel.r / pixel.a, pixel.g / pixel.a, pixel.b / pixel.a, pixel.a};
    return {0, 0, 0, pixel.a};
}

SILENTFILM_DEFECT_HD inline float unit(std::uint32_t key) noexcept
{
    return static_cast<float>(film::hash(key) >> 8) * (1.0f / 16777216.0f);
}

SILENTFILM_DEFECT_HD inline float lattice(std::uint32_t base, int x, int y) noexcept
{
    return unit(base ^ (static_cast<std::uint32_t>(x) * 0x9e3779b9u)
        ^ (static_cast<std::uint32_t>(y) * 0x85ebca6bu)) * 2 - 1;
}

/** Native deterministic coarse knots; the interpolation model matches Comfy, not its Python RNG. */
SILENTFILM_DEFECT_HD inline float developmentKnot(std::uint32_t seed, bool streaks,
    std::uint32_t knot, int index, bool stationary = false) noexcept
{
    return 2 * unit(seed ^ (streaks ? 0x53545245u : 0x4d4f5454u)
        ^ film::hash(knot ^ (stationary ? 0x53544154u : 0x54494d45u))
        ^ (static_cast<std::uint32_t>(index) * 0x9e3779b9u)) - 1;
}

/** Blend coarse grids in time before spatial interpolation, as in the reference tensor operations. */
SILENTFILM_DEFECT_HD inline float developmentNode(const DefectFrame& frame, bool streaks, int index) noexcept
{
    const auto& p = frame.parameters;
    const auto& grid = frame.development;
    const float fixed = p.irregularity < 1 ? developmentKnot(p.seed, streaks, 0, index, true) : 0;
    if (p.irregularity == 0) return fixed;
    const float a = developmentKnot(p.seed, streaks, grid.knot, index);
    const float b = developmentKnot(p.seed, streaks, grid.knot + 1, index);
    const float moving = a + (b - a) * grid.weight;
    return p.irregularity < 1 ? fixed + (moving - fixed) * p.irregularity : moving;
}

/** Generate one packed grid sample on CPU or GPU without an image-sized mask. */
SILENTFILM_DEFECT_HD inline float developmentGridValue(const DefectFrame& frame, int index) noexcept
{
    const bool streaks = index >= frame.development.mottlingCount;
    return developmentNode(frame, streaks, streaks ? index - frame.development.mottlingCount : index);
}

SILENTFILM_DEFECT_HD inline float cubicWeight(float distance) noexcept
{
    const float x = ::fabsf(distance);
    // Bicubic PyTorch использует кубическое ядро Keys с a=-0.75.
    return x <= 1 ? ((1.25f * x - 2.25f) * x) * x + 1
        : x < 2 ? ((-0.75f * x + 3.75f) * x - 6) * x + 3 : 0;
}

/** Reproduce bicubic align_corners=False, including replicated edge samples and post-filter clamp. */
SILENTFILM_DEFECT_HD inline float developmentBicubic(float px, float py, bool streaks,
    const DefectFrame& frame) noexcept
{
    const auto& grid = frame.development;
    const int rows = streaks ? 2 : grid.rows;
    const float sx = px * (static_cast<float>(grid.columns) / grid.width) - 0.5f;
    const float sy = py * (static_cast<float>(rows) / grid.height) - 0.5f;
    const int ix = static_cast<int>(::floorf(sx)), iy = static_cast<int>(::floorf(sy));
    const float tx = sx - ix, ty = sy - iy;
    const float wx[4] = {cubicWeight(tx + 1), cubicWeight(tx), cubicWeight(1 - tx), cubicWeight((1 - tx) + 1)};
    const float wy[4] = {cubicWeight(ty + 1), cubicWeight(ty), cubicWeight(1 - ty), cubicWeight((1 - ty) + 1)};
    float result = 0;
#ifdef __CUDA_ARCH__
#pragma unroll
#endif
    for (int j = 0; j < 4; ++j) {
        const int y = iy + j - 1 < 0 ? 0 : iy + j - 1 >= rows ? rows - 1 : iy + j - 1;
        float line = 0;
#ifdef __CUDA_ARCH__
#pragma unroll
#endif
        for (int i = 0; i < 4; ++i) {
            const int x = ix + i - 1 < 0 ? 0 : ix + i - 1 >= grid.columns ? grid.columns - 1 : ix + i - 1;
            const int index = y * grid.columns + x;
            const float value = grid.values ? grid.values[(streaks ? grid.mottlingCount : 0) + index]
                : developmentNode(frame, streaks, index);
            line += value * wx[i];
        }
        result += line * wy[j];
    }
    return film::clamp(result, -1, 1);
}

SILENTFILM_DEFECT_HD inline float developmentField(float px, float py, const DefectFrame& frame) noexcept
{
    const auto mode = frame.parameters.development;
    if (mode == DefectDevelopment::Streaks) return developmentBicubic(px, py, true, frame);
    const float mottling = developmentBicubic(px, py, false, frame);
    return mode == DefectDevelopment::Mixed ? mottling * 0.65f + developmentBicubic(px, py, true, frame) * 0.35f : mottling;
}

SILENTFILM_DEFECT_HD inline float development(float px, float py, const DefectFrame& frame) noexcept
{
    if (frame.parameters.development != DefectDevelopment::Legacy) {
        const float field = developmentField(px, py, frame);
        const float strength = frame.parameters.mixResponse == DefectMixResponse::Physical ? frame.parameters.strength : 1;
        const float density = strength * (0.35f * field + 0.3f * frame.parameters.developmentFog * (field + 1) * 0.5f);
        return ::powf(10.0f, -density);
    }
    const int gx = static_cast<int>(::floorf(px / frame.cell)), gy = static_cast<int>(::floorf(py / frame.cell));
    const float fx = px / frame.cell - gx, fy = py / frame.cell - gy;
    const float a = lattice(frame.base, gx, gy), b = lattice(frame.base, gx + 1, gy);
    const float c = lattice(frame.base, gx, gy + 1), d = lattice(frame.base, gx + 1, gy + 1);
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float top = a + (b - a) * sx, bottom = c + (d - c) * sx;
    const float field = top + (bottom - top) * sy;
    const float strength = frame.parameters.mixResponse == DefectMixResponse::Physical ? frame.parameters.strength : 1.0f;
    const float density = strength * (0.35f * field + 0.3f * (field + 1) * 0.5f);
    return ::powf(10.0f, -density);
}

/** Reject finite dust/scratch support before evaluating sqrt or touching color conversion. */
SILENTFILM_DEFECT_HD inline float coverage(float px, float py, int height, const DefectFrame& frame) noexcept
{
    float result = 0;
    for (int i = 0; i < frame.count; ++i) {
        const auto& particle = frame.particles[static_cast<std::size_t>(i)];
        float mark = 0;
        if (frame.parameters.damage != DefectDamage::Legacy) {
            mark = particleCoverage(px, py, height, particle, frame.parameters);
        } else if (frame.parameters.pattern == DefectPattern::Scratches) {
            const float center = particle.x + particle.drift * (py / ::fmaxf(1, static_cast<float>(height)) - 0.5f);
            const float distance = ::fabsf(px - center);
            if (distance >= 0.5f + particle.radius) continue;
            mark = film::clamp(0.5f + particle.radius - distance, 0, 1);
        } else {
            const float dx = px - particle.x, dy = py - particle.y;
            if (frame.parameters.pattern == DefectPattern::Dust
                && (::fabsf(dx) > 1.5f * particle.radius || ::fabsf(dy) > 1.5f * particle.radius)) continue;
            const float radius = ::sqrtf(dx * dx + dy * dy) / particle.radius;
            mark = frame.parameters.pattern == DefectPattern::Stains ? ::expf(-1.5f * radius * radius)
                : film::clamp(1.5f - radius, 0, 1);
        }
        result = ::fmaxf(result, mark);
        if (result == 1) break;
    }
    return result * (frame.parameters.mixResponse == DefectMixResponse::Physical ? frame.parameters.strength : 1.0f);
}

/** View negative transmission through the fixed neutral print in sRGB for opacity controls. */
SILENTFILM_DEFECT_HD inline float referenceView(float transmission) noexcept
{
    const EmulsionParameters reference{};
    return film::viewPositive(film::printDensity(transmission, reference), Transfer::SRGB);
}

/** Invert the neutral viewing calibration back to physical negative transmission. */
SILENTFILM_DEFECT_HD inline float referenceTransmission(float value) noexcept
{
    const EmulsionParameters reference{};
    return film::transmittance(film::negativeDensity(film::decode(value, Transfer::SRGB), reference));
}

/** Interpolate visible negative/printing response without lifting tiny transmission toward clear stock. */
SILENTFILM_DEFECT_HD inline float visibleMix(float source, float effect, float opacity, bool negative) noexcept
{
    if (opacity <= 0 || source == effect) return source;
    if (opacity >= 1) return effect;
    if (!negative) return source * (1 - opacity) + effect * opacity;
    const float clean = referenceView(source), wet = referenceView(effect);
    // Одинаковый вид за границами калиброванной шкалы не должен сдвигать исходную плотность.
    if (clean == wet) return source * (1 - opacity) + effect * opacity;
    return referenceTransmission(clean * (1 - opacity) + wet * opacity);
}

SILENTFILM_DEFECT_HD inline float shade(float value, float multiplier, float coverage,
    const DefectParameters& p) noexcept
{
    const bool physical = p.material == DefectMaterial::Negative || p.material == DefectMaterial::Printing;
    if (!physical) value = film::decode(value, p.transfer);
    if (p.pattern == DefectPattern::Development) {
        value *= multiplier;
        if (p.development != DefectDevelopment::Legacy) value = film::clamp(value, 0, 1);
    }
    else if (p.damage != DefectDamage::Legacy) {
        const bool gate = p.material == DefectMaterial::Capture || p.material == DefectMaterial::Printing
            || p.material == DefectMaterial::Projector;
        if (gate) value *= 1 - coverage;
        else if (p.damage == DefectDamage::EmulsionLoss) {
            if (p.pattern == DefectPattern::Dust) value *= 1 - coverage;
            else {
                const float clear = physical ? 0.79432823f : 1.0f;
                value += (::fmaxf(clear, value) - value) * coverage;
            }
        } else value *= ::powf(10.0f, -(p.damage == DefectDamage::Dirty ? 1.5f : 0.65f) * coverage);
        value = film::clamp(value, 0, 1);
    }
    else if (p.material == DefectMaterial::Negative && p.pattern == DefectPattern::Scratches)
        value += (::fmaxf(0.79432823f, value) - value) * coverage;
    else value *= 1 - coverage;
    return physical ? value : film::encode(value, p.transfer);
}

/** Preserve sparse backgrounds and process monochrome input with a single transfer pair. */
SILENTFILM_DEFECT_HD inline Pixel apply(Pixel original, float px, float py, int height,
    const DefectFrame& frame) noexcept
{
    const auto& p = frame.parameters;
    if (neutral(p)) return convertAlpha(original, p);
    float mask = 0, multiplier = 1;
    if (p.pattern == DefectPattern::Development) multiplier = development(px, py, frame);
    else {
        mask = coverage(px, py, height, frame);
        if (mask == 0) return convertAlpha(original, p);
    }
    Pixel pixel = original;
    if (p.sourcePremultiplied) {
        if (pixel.a <= 0) pixel.r = pixel.g = pixel.b = 0;
        else { pixel.r /= pixel.a; pixel.g /= pixel.a; pixel.b /= pixel.a; }
    }
    const Pixel clean = pixel;
    if (pixel.r == pixel.g && pixel.g == pixel.b) pixel.r = pixel.g = pixel.b = shade(pixel.r, multiplier, mask, p);
    else {
        pixel.r = shade(pixel.r, multiplier, mask, p); pixel.g = shade(pixel.g, multiplier, mask, p);
        pixel.b = shade(pixel.b, multiplier, mask, p);
    }
    if (p.mixResponse == DefectMixResponse::Perceptual) {
        const float opacity = p.strength * p.blend;
        const bool negative = p.material == DefectMaterial::Negative || p.material == DefectMaterial::Printing;
        if (pixel.r == pixel.g && pixel.g == pixel.b && clean.r == clean.g && clean.g == clean.b)
            pixel.r = pixel.g = pixel.b = visibleMix(clean.r, pixel.r, opacity, negative);
        else {
            pixel.r = visibleMix(clean.r, pixel.r, opacity, negative);
            pixel.g = visibleMix(clean.g, pixel.g, opacity, negative);
            pixel.b = visibleMix(clean.b, pixel.b, opacity, negative);
        }
        if (p.outputPremultiplied) { pixel.r *= pixel.a; pixel.g *= pixel.a; pixel.b *= pixel.a; }
        return pixel;
    }
    if (p.outputPremultiplied) { pixel.r *= pixel.a; pixel.g *= pixel.a; pixel.b *= pixel.a; }
    if (p.blend < 1) {
        const auto clean = convertAlpha(original, p);
        const float dry = 1 - p.blend;
        pixel.r = clean.r * dry + pixel.r * p.blend;
        pixel.g = clean.g * dry + pixel.g * p.blend;
        pixel.b = clean.b * dry + pixel.b * p.blend;
    }
    return pixel;
}
} // namespace defect

/** Run one fused image pass; development additionally prepares only a bounded coarse GPU grid. */
CudaStatus renderCudaDefects(ImageView source, ImageView output, RenderWindow window,
    const DefectParameters& parameters, void* stream, bool hostStreamProvided) noexcept;

} // namespace silentfilm

#undef SILENTFILM_DEFECT_HD
