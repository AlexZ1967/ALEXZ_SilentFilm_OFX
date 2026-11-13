#include "silentfilm/defects_pixel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace silentfilm {
namespace {

/** Interpolate independent hash knots without sinusoidal cycles or mutable random state. */
float smoothSignal(std::uint32_t seed, double time, std::uint32_t lane)
{
    const auto knot = static_cast<std::int64_t>(std::floor(time));
    const float t = static_cast<float>(time - static_cast<double>(knot));
    const float weight = t * t * (3 - 2 * t);
    const float a = 2 * defect::unit(seed ^ film::hash(static_cast<std::uint32_t>(knot)) ^ lane) - 1;
    const float b = 2 * defect::unit(seed ^ film::hash(static_cast<std::uint32_t>(knot + 1)) ^ lane) - 1;
    return a + (b - a) * weight;
}

} // namespace

DefectFrame prepareDefects(const DefectParameters& p, int width, int height)
{
    DefectFrame result;
    result.parameters = p;
    result.cell = std::max(2.0f, width * p.size * 0.01f);
    if (defect::neutral(p)) return result;
    // API core принимает секунды; OFX adapter переводит свой frame time один раз.
    const auto frame = static_cast<std::int64_t>(std::floor(p.time * p.frameRate + 1e-9));
    const auto event = static_cast<std::int64_t>(std::floor(static_cast<double>(frame) / std::max(1, p.persistence)));
    result.base = p.seed ^ film::hash(static_cast<std::uint32_t>(event)) ^ 0x27d4eb2du;
    if (p.pattern == DefectPattern::Development) {
        if (p.development != DefectDevelopment::Legacy) {
            auto& grid = result.development;
            grid.width = std::max(1, width); grid.height = std::max(1, height);
            const double spacing = std::max(1.0, width * static_cast<double>(p.size) / 100);
            grid.rows = std::clamp(static_cast<int>(std::ceil(height / spacing)) + 1, 2, 128);
            grid.columns = std::clamp(static_cast<int>(std::ceil(width / spacing)) + 1, 2, 128);
            const double time = static_cast<double>(frame) / std::max(1, p.persistence);
            const double knot = std::floor(time);
            const float t = static_cast<float>(time - knot);
            grid.knot = static_cast<std::uint32_t>(static_cast<std::int64_t>(knot));
            grid.weight = t * t * (3 - 2 * t);
            grid.mottlingCount = p.development == DefectDevelopment::Streaks ? 0 : grid.rows * grid.columns;
            grid.count = grid.mottlingCount + (p.development == DefectDevelopment::Mottling ? 0 : 2 * grid.columns);
        }
        return result;
    }
    float amount = p.amount;
    if (p.amountVariationPercent > 0)
        amount *= std::max(0.0f, 1 + p.amountVariationPercent * 0.01f
            * smoothSignal(p.seed, static_cast<double>(frame) / std::max(1, p.amountDriftTime), 0x414d4f55u));
    result.effectiveAmount = amount = std::clamp(amount, 0.0f, 64.0f);
    const int whole = static_cast<int>(std::floor(amount));
    const float fraction = amount - whole;
    const int count = std::min(64, whole + (fraction > 0 && whole < 64 ? 1 : 0));
    if (p.damage != DefectDamage::Legacy) {
        const bool gate = p.material == DefectMaterial::Capture || p.material == DefectMaterial::Printing
            || p.material == DefectMaterial::Projector;
        for (int i = 0; i < count; ++i) {
            const auto identity = p.seed ^ (static_cast<std::uint32_t>(i) * 0xc2b2ae35u);
            const int phase = static_cast<int>(defect::unit(identity ^ 0x50484153u) * std::max(1, p.persistence));
            const auto slotEvent = static_cast<std::int64_t>(std::floor(static_cast<double>(frame + phase) / std::max(1, p.persistence)));
            const auto index = !gate && p.pattern == DefectPattern::Dust ? frame : slotEvent;
            const auto key = identity ^ film::hash(static_cast<std::uint32_t>(index))
                ^ (static_cast<std::uint32_t>(p.pattern) * 0x27d4eb2du);
            if (i >= whole && defect::unit(key ^ 0xa511e9b3u) >= fraction) continue;
            auto& particle = result.particles[result.count++];
            particle.x = defect::unit(key) * width;
            particle.y = defect::unit(key ^ 0x68bc21ebu) * height;
            if (p.edgeBias > 0 && defect::unit(key ^ 0x45444745u) < p.edgeBias) {
                const bool left = defect::unit(key ^ 0x53494445u) < 0.5f;
                const float distance = -0.01f + 0.13f * defect::unit(key ^ 0x42414e44u);
                particle.x = width * (left ? distance : 1 - distance);
            }
            const float spread = (p.pattern == DefectPattern::Scratches ? p.thicknessSpreadPercent : p.sizeSpreadPercent) * 0.01f;
            const float diameter = width * p.size * 0.01f
                * (1.05f + spread * (2 * defect::unit(key ^ 0x02e5be93u) - 1));
            if (p.pattern == DefectPattern::Scratches) {
                particle.radius = std::max(0.2f, diameter * 0.5f)
                    * std::max(0.0f, 0.975f + (0.325f / 0.35f) * spread * (2 * defect::unit(key ^ 0x52414449u) - 1));
                const float amplitude = width * 0.008f * p.irregularity;
                const float left = 2 * defect::unit(key ^ film::hash(static_cast<std::uint32_t>(frame)) ^ 0x50415448u) - 1;
                const float right = 2 * defect::unit(key ^ film::hash(static_cast<std::uint32_t>(frame + 1)) ^ 0x50415448u) - 1;
                particle.y = particle.x; // Для царапины y хранит центр ограниченной полосы rasterizer.
                particle.x += amplitude * (left + right) * 0.5f;
                particle.drift = amplitude * (right - left);
                particle.support = amplitude + particle.radius;
                if (p.scratchWander > 0) {
                    const double period = std::max(1, p.scratchWanderTime);
                    const float a = smoothSignal(identity, static_cast<double>(frame) / period, 0x57414e44u);
                    const float b = smoothSignal(identity, (static_cast<double>(frame) + 1) / period, 0x57414e44u);
                    const float extra = p.scratchWander * width / 1920.0f;
                    particle.x += extra * (a + b) * 0.5f;
                    particle.drift += extra * (b - a);
                    particle.support += extra;
                }
                particle.gapTop = defect::unit(key ^ film::hash(static_cast<std::uint32_t>(frame)) ^ 0x47415053u);
                particle.gapBottom = defect::unit(key ^ film::hash(static_cast<std::uint32_t>(frame + 1)) ^ 0x47415053u);
                particle.opacity = std::min(1.0f, diameter / 0.6f);
            } else {
                if (gate) {
                    particle.x += diameter * p.irregularity * smoothSignal(key, frame / 8.0, 0x584d4f56u);
                    particle.y += diameter * p.irregularity * smoothSignal(key, frame / 8.0, 0x594d4f56u);
                }
                particle.radius = std::max(0.3f, diameter * 0.5f);
                const float angle = -3.14159265358979323846f + 6.28318530717958647692f * defect::unit(key ^ 0x414e474cu);
                particle.cosine = std::cos(angle); particle.sine = std::sin(angle);
                particle.aspect = 0.55f + 0.65f * defect::unit(key ^ 0x41535045u);
                particle.phase = -3.14159265358979323846f + 6.28318530717958647692f * defect::unit(key ^ 0x50484153u);
                particle.opacity = std::min(1.0f, diameter / 0.7f) * (0.55f + 0.45f * defect::unit(key ^ 0x4f504143u));
                particle.support = particle.radius * (p.pattern == DefectPattern::Stains ? 3.0f : 1.5f);
            }
        }
        return result;
    }
    const float radius = std::max(0.35f, width * p.size * 0.01f * 0.5f);
    for (int i = 0; i < count; ++i) {
        const auto key = result.base ^ (static_cast<std::uint32_t>(i) * 0xc2b2ae35u);
        if (i >= whole && defect::unit(key ^ 0xa511e9b3u) >= fraction) continue;
        auto& particle = result.particles[result.count++];
        particle.x = defect::unit(key) * width;
        particle.y = defect::unit(key ^ 0x68bc21ebu) * height;
        if (p.edgeBias > 0 && defect::unit(key ^ 0x45444745u) < p.edgeBias) {
            const int side = static_cast<int>(defect::unit(key ^ 0x53494445u) * 2);
            const float distance = -0.01f + 0.13f * defect::unit(key ^ 0x42414e44u);
            particle.x = width * (side == 0 ? distance : 1 - distance);
        }
        const float spread = (p.pattern == DefectPattern::Scratches ? p.thicknessSpreadPercent : p.sizeSpreadPercent) * 0.01f;
        particle.radius = radius * std::max(0.0f, 1 + spread * (2 * defect::unit(key ^ 0x02e5be93u) - 1));
        if (particle.radius <= 0) { --result.count; continue; }
        particle.drift = (defect::unit(key ^ 0x51ed270bu) - 0.5f) * width * 0.016f;
        if (p.pattern == DefectPattern::Scratches && p.scratchWander > 0) {
            const auto identity = p.seed ^ (static_cast<std::uint32_t>(i) * 0xc2b2ae35u);
            const double period = std::max(1, p.scratchWanderTime);
            const float left = smoothSignal(identity, static_cast<double>(frame) / period, 0x50415448u);
            const float right = smoothSignal(identity, (static_cast<double>(frame) + 1) / period, 0x50415448u);
            const float amplitude = p.scratchWander * static_cast<float>(width) / 1920.0f;
            particle.x += amplitude * (left + right) * 0.5f;
            particle.drift += amplitude * (right - left);
        }
    }
    return result;
}

bool renderDefectsCpu(ImageView source, ImageView output, RenderWindow window,
    const DefectParameters& p, AbortCallback abort, void* context)
{
    const int width = source.x2 - source.x1, height = source.y2 - source.y1;
    if (width <= 0 || height <= 0) return true;
    auto frame = prepareDefects(p, width, height);
    std::vector<float> coarse(static_cast<std::size_t>(frame.development.count));
    for (int i = 0; i < frame.development.count; ++i)
        coarse[static_cast<std::size_t>(i)] = defect::developmentGridValue(frame, i);
    if (!coarse.empty()) frame.development.values = coarse.data();
    const int x1 = std::max(window.x1, output.x1), x2 = std::min(window.x2, output.x2);
    const int y1 = std::max(window.y1, output.y1), y2 = std::min(window.y2, output.y2);
    for (int y = y1; y < y2; ++y) {
        if (abort && abort(context)) return false;
        for (int x = x1; x < x2; ++x) {
            Pixel result{};
            if (x >= source.x1 && y >= source.y1 && x < source.x2 && y < source.y2) {
                const auto* address = source.data + (static_cast<std::ptrdiff_t>(y) - source.y1) * source.rowBytes
                    + (static_cast<std::ptrdiff_t>(x) - source.x1) * sizeof(Pixel);
                Pixel original;
                std::memcpy(&original, address, sizeof(original));
                result = defect::apply(original, x - static_cast<float>(source.x1) + 0.5f,
                    y - static_cast<float>(source.y1) + 0.5f, height, frame);
            }
            auto* address = output.data + (static_cast<std::ptrdiff_t>(y) - output.y1) * output.rowBytes
                + (static_cast<std::ptrdiff_t>(x) - output.x1) * sizeof(Pixel);
            std::memcpy(address, &result, sizeof(result));
        }
    }
    return true;
}
} // namespace silentfilm
