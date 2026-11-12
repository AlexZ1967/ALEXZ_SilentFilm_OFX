#include "silentfilm/grain.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace silentfilm {
namespace {

void normalize(std::vector<float>& values)
{
    double sum = 0;
    for (const auto value : values) sum += value;
    const double mean = sum / values.size();
    double square = 0;
    for (const auto value : values) { const double centered = value - mean; square += centered * centered; }
    const double deviation = std::max(1e-6, std::sqrt(square / values.size()));
    for (auto& value : values) value = static_cast<float>((value - mean) / deviation);
}

bool expand(const GrainLattice& lattice, int width, int height, std::vector<float>& output,
    AbortCallback abort, void* context)
{
    if (lattice.width <= 0 || lattice.height <= 0
        || lattice.values.size() != static_cast<std::size_t>(lattice.width) * lattice.height)
        throw std::invalid_argument("Invalid emulsion grain lattice");
    output.resize(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        if (abort && abort(context)) return false;
        const float py = std::clamp((y + 0.5f) * (static_cast<float>(lattice.height) / height) - 0.5f,
            0.0f, static_cast<float>(lattice.height - 1));
        const int y0 = static_cast<int>(std::floor(py)), y1 = std::min(y0 + 1, lattice.height - 1);
        const float fy = py - y0;
        for (int x = 0; x < width; ++x) {
            const float px = std::clamp((x + 0.5f) * (static_cast<float>(lattice.width) / width) - 0.5f,
                0.0f, static_cast<float>(lattice.width - 1));
            const int x0 = static_cast<int>(std::floor(px)), x1 = std::min(x0 + 1, lattice.width - 1);
            const float fx = px - x0;
            const auto row0 = static_cast<std::size_t>(y0) * lattice.width, row1 = static_cast<std::size_t>(y1) * lattice.width;
            const float a = lattice.values[row0 + x0] * (1 - fx) + lattice.values[row0 + x1] * fx;
            const float b = lattice.values[row1 + x0] * (1 - fx) + lattice.values[row1 + x1] * fx;
            output[static_cast<std::size_t>(y) * width + x] = a * (1 - fy) + b * fy;
        }
    }
    normalize(output);
    return true;
}
} // namespace

std::array<GrainLattice, 4> emulsionGrainLattices(int width, int height, const EmulsionParameters& p)
{
    if (width <= 0 || height <= 0) throw std::invalid_argument("Empty grain image");
    std::array<GrainLattice, 4> result;
    const double effective = std::max(1.0, static_cast<double>(p.grainSize) * width / 1920.0);
    const std::array<double, 4> scales{effective, effective * 2, effective * 4, effective * 6};
    const auto seed = p.grainSeed ^ film::hash(p.grainFrame);
    for (std::size_t lane = 0; lane < result.size(); ++lane) {
        auto& lattice = result[lane];
        lattice.width = optics::blurDimension(width, 1 / scales[lane]);
        lattice.height = optics::blurDimension(height, 1 / scales[lane]);
        lattice.values.resize(static_cast<std::size_t>(lattice.width) * lattice.height);
        for (int y = 0; y < lattice.height; ++y)
            for (int x = 0; x < lattice.width; ++x)
                lattice.values[static_cast<std::size_t>(y) * lattice.width + x] = emulsionGrainNormal(x, y, seed, static_cast<std::uint32_t>(lane) * 16);
    }
    return result;
}

bool emulsionGrainField(const std::array<GrainLattice, 4>& lattices, int width, int height,
    std::vector<float>& output, AbortCallback abort, void* context)
{
    if (width <= 0 || height <= 0) throw std::invalid_argument("Empty grain image");
    std::vector<float> fine, field;
    if (!expand(lattices[0], width, height, fine, abort, context)) return false;
    if (!expand(lattices[3], width, height, field, abort, context)) return false;
    output.resize(fine.size());
    for (std::size_t i = 0; i < fine.size(); ++i)
        output[i] = fine[i] * (0.78f + 0.48f / (1 + std::exp(-1.3f * field[i])));
    for (std::size_t lane = 1; lane <= 2; ++lane) {
        if (!expand(lattices[lane], width, height, field, abort, context)) return false;
        const float weight = lane == 1 ? 0.42f : 0.24f;
        for (std::size_t i = 0; i < output.size(); ++i) output[i] += field[i] * weight;
    }
    normalize(output);
    for (auto& value : output) value = std::tanh(value * 1.25f);
    return !(abort && abort(context));
}
} // namespace silentfilm
