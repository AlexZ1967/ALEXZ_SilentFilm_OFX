#pragma once

#include "silentfilm/optics.hpp"
#include "silentfilm/sensitometry.hpp"

#include <array>
#include <vector>

namespace silentfilm {

/** Generate a standard Gaussian from stable coordinates, shared by CPU and CUDA. */
#if defined(__CUDACC__)
__host__ __device__
#endif
inline float emulsionGrainNormal(int x, int y, std::uint32_t seed, std::uint32_t lane) noexcept
{
    const double u = static_cast<double>(film::randomLattice(x, y, seed, lane)) + 0.5 / 16777216.0;
    const double v = static_cast<double>(film::randomLattice(x, y, seed, lane + 1));
    return static_cast<float>(::sqrt(-2 * ::log(u)) * ::cos(6.2831853071795864769 * v));
}

struct GrainLattice {
    int width = 0, height = 0;
    std::vector<float> values;
};

/** Build independent Gaussian lattices; Torch RNG identity is deliberately not assumed. */
std::array<GrainLattice, 4> emulsionGrainLattices(int width, int height, const EmulsionParameters& parameters);

/** Match Comfy bilinear expansion, population normalization, clustering and final tanh. */
bool emulsionGrainField(const std::array<GrainLattice, 4>& lattices, int width, int height,
    std::vector<float>& output, AbortCallback abort = nullptr, void* context = nullptr);

} // namespace silentfilm
