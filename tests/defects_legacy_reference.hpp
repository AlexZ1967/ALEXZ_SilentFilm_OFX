#pragma once

// Независимый регрессионный эталон CPU до оптимизации Defects 0.20.
#include "silentfilm/film_optics.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace legacy_defects {
using namespace silentfilm;
inline bool cancelled(AbortCallback abort, void* context) { return abort && abort(context); }
inline Pixel readPixel(ImageView image, int x, int y) {
    if (x < image.x1 || y < image.y1 || x >= image.x2 || y >= image.y2) return {};
    Pixel result{};
    std::memcpy(&result, image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel), sizeof(result));
    return result;
}
inline void writePixel(ImageView image, int x, int y, Pixel value) {
    std::memcpy(image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel), &value, sizeof(value));
}

inline bool render(ImageView source, ImageView output, RenderWindow window,
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

} // namespace legacy_defects
