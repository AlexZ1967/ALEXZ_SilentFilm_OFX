#pragma once

#include "silentfilm/cuda_backend.hpp"
#include "silentfilm/optics.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace silentfilm {

constexpr int maximumCameraExposureSamples = 17;

struct HandCrankSettings {
    float minimumFps = 16.0f;
    float maximumFps = 20.0f;
    float rateVariation = 0.7f;
    float irregularity = 0.15f;
    std::uint32_t seed = 1925;
};

struct CaptureInterval {
    std::int64_t frame = 0;
    double start = 0.0;
    double end = 0.0;
    double phase = 0.0;
    double fps = 0.0;
};

/** Cache deterministic crank intervals so random-access renders share no history dependence. */
class HandCrankSchedule {
    mutable std::mutex mutex_;
    HandCrankSettings settings_{};
    bool initialized_ = false;
    double initialPhase_ = 0.0;
    std::vector<double> starts_;
    std::vector<double> rates_;

    void reset(const HandCrankSettings& settings);
    void extendTo(double time, const HandCrankSettings& settings);
    CaptureInterval intervalForIndex(std::size_t index) const;

public:
    /** Release cached intervals; the next request regenerates the deterministic schedule. */
    void clear();
    /** Return one physical capture by index without duration normalization or playback resampling. */
    CaptureInterval captureAtIndex(std::int64_t index, const HandCrankSettings& settings);
    CaptureInterval intervalAt(double relativeTime, const HandCrankSettings& settings);
    /** Project captured frames at a uniform rate, normalized once to preserve clip duration. */
    CaptureInterval projectedIntervalAt(double relativeOutputTime, double sourceDuration,
        const HandCrankSettings& settings);
    double phaseAt(double relativeTime, const HandCrankSettings& settings);
};

/** Evaluate the native focus signal for the physical capture shared by OFX and offline rendering. */
float cameraFocusSignal(std::int64_t captureFrame, std::uint32_t seed) noexcept;

struct CameraExposureSample {
    ImageView image;
    float phase = 0.0f;
    ImageView nextImage{};
    float temporalMix = 0.0f;
    float weight = 1.0f;
};

struct CameraSampleTransform {
    double cosine = 1, sine = 0, shiftX = 0, shiftY = 0;
};

/** Evaluate the shared deterministic affine camera pose on the CPU. */
CameraSampleTransform cameraSampleTransform(float phase, float strength,
    std::uint32_t seed, int width, int height);

/** Enqueue the complete camera pipeline on device images without CPU pixel transfers. */
CudaStatus renderCudaCamera(const CameraExposureSample* samples, const CameraSampleTransform* transforms,
    int sampleCount, ImageView output, RenderWindow window, const CameraOpticsParameters& parameters,
    float exposureGain, bool shakeEnabled, void* stream, bool hostStreamProvided) noexcept;

/** Integrate weighted, temporally interpolated exposure samples and apply camera optics. */
bool renderCameraCaptureCpu(const CameraExposureSample* samples, int sampleCount,
    ImageView output, RenderWindow window, const CameraOpticsParameters& parameters,
    float exposureGain, float shakeStrength, std::uint32_t seed,
    AbortCallback abort, void* context);

} // namespace silentfilm
