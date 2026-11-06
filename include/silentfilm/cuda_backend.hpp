#pragma once

#include <cstddef>
#include "silentfilm/sensitometry.hpp"

namespace silentfilm {

struct ImageView {
    unsigned char* data;
    std::ptrdiff_t rowBytes;
    int x1, y1, x2, y2;
};

struct RenderWindow {
    int x1, y1, x2, y2;
};

struct CudaStatus {
    int code;
    const char* message;
};

/** Enqueue the probe on the host stream, synchronizing only when no stream was supplied. */
CudaStatus renderCudaProbe(ImageView current, ImageView previous, ImageView next, ImageView output,
    RenderWindow window, float gain, float temporalMix, void* stream, bool hostStreamProvided) noexcept;

/** Enqueue one negative or print pass using the same calibrated math as the CPU core. */
CudaStatus renderCudaEmulsion(ImageView source, ImageView output, RenderWindow window,
    EmulsionParameters parameters, void* stream, bool hostStreamProvided) noexcept;

} // namespace silentfilm
