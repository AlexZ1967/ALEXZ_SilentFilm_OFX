#pragma once

#include "silentfilm/cuda_backend.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <vector>

namespace silentfilm {

struct ProjectorFrameState {
    float shiftX = 0, shiftY = 0, lampGain = 1;
};

struct ProjectorParameters {
    Transfer transfer = Transfer::SRGB;
    float glowStrength = 0, glowSize = 1;
    ProjectorFrameState frame;
    bool sourcePremultiplied = false, outputPremultiplied = false;
};

/** Cache clip-normalized AR(1) signals independently of render order and camera cadence. */
class ProjectorSignalCache {
    std::mutex mutex_;
    std::vector<std::array<float, 4>> signals_;
    std::uint32_t seed_ = 0;
public:
    ProjectorFrameState at(std::size_t frame, std::size_t frameCount, std::uint32_t seed,
        float weave, float flicker, float breathing);
};

/** Reuse context/stream-specific cuFFT plans without retaining any host image pointer. */
class ProjectorCudaCache {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    ProjectorCudaCache();
    ~ProjectorCudaCache();
    ProjectorCudaCache(const ProjectorCudaCache&) = delete;
    ProjectorCudaCache& operator=(const ProjectorCudaCache&) = delete;
    CudaStatus render(ImageView source, ImageView output, RenderWindow window,
        const ProjectorParameters& parameters, void* stream, bool hostStreamProvided) noexcept;
};

} // namespace silentfilm
