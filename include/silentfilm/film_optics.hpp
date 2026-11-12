#pragma once

#include "silentfilm/cuda_backend.hpp"
#include "silentfilm/optics.hpp"
#include "silentfilm/projector.hpp"

namespace silentfilm {

/** Diffuse a scalar plane with the Comfy film-optics reflected-boundary PSF. */
bool diffuseFilmPlane(const float* source, float* output, int width, int height, float sigma,
    AbortCallback abort, void* context);

/** Apply negative or print emulsion, including optional pre-development halation. */
bool renderEmulsionCpu(ImageView source, ImageView output, RenderWindow window,
    EmulsionParameters parameters, float halationStrength, float halationSize,
    AbortCallback abort, void* context);

/** Process spatial emulsion and normalized grain entirely on the host CUDA stream. */
CudaStatus renderCudaFilm(ImageView source, ImageView output, RenderWindow window,
    EmulsionParameters parameters, ProjectorCudaCache& diffusion,
    void* stream, bool hostStreamProvided) noexcept;

/** Transport the positive, modulate linear lamp light, then apply projection glow. */
bool renderProjectorCpu(ImageView source, ImageView output, RenderWindow window,
    const ProjectorParameters& parameters,
    AbortCallback abort, void* context);

enum class DefectMaterial : int { Capture = 0, Negative = 1, Printing = 2, Positive = 3, Projector = 4 };
enum class DefectPattern : int { Dust = 0, Scratches = 1, Stains = 2, Development = 3 };
enum class DefectMixResponse : int { Perceptual = 0, Physical = 1 };
struct DefectParameters {
    DefectMaterial material = DefectMaterial::Capture;
    DefectPattern pattern = DefectPattern::Dust;
    Transfer transfer = Transfer::Linear;
    float strength = 0.2f;
    float size = 0.12f;
    float amount = 3.0f;
    float sizeSpreadPercent = 35.0f;
    float thicknessSpreadPercent = 35.0f;
    float edgeBias = 0;
    float scratchWander = 0; // Максимальное боковое отклонение в пикселях при ширине 1920.
    int scratchWanderTime = 8;
    float amountVariationPercent = 0;
    int amountDriftTime = 48;
    float blend = 1.0f;
    DefectMixResponse mixResponse = DefectMixResponse::Perceptual;
    int persistence = 48;
    std::uint32_t seed = 1925;
    double time = 0; // Секунды от начала клипа; OFX adapter переводит frame time до вызова core.
    double frameRate = 24;
    bool sourcePremultiplied = false;
    bool outputPremultiplied = false;
};

/** Apply deterministic standalone film contamination at its selected material stage. */
bool renderDefectsCpu(ImageView source, ImageView output, RenderWindow window,
    const DefectParameters& parameters, AbortCallback abort, void* context);

} // namespace silentfilm
