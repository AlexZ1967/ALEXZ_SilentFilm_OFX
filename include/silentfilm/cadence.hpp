#pragma once

#include "silentfilm/camera_capture.hpp"

#include <array>

namespace silentfilm {

struct CadenceSettings {
    HandCrankSettings crank;
    CameraOpticsParameters optics;
    double shutterFraction = 0.65, motionBlurStrength = 0.8, exposureVariation = 1;
    float shake = 0.25f;
    std::uint32_t focusSeed = 1925;
    int exposureSamples = 5;
};

struct CaptureExposurePlan {
    CaptureInterval interval;
    double sourceCenter = 0;
    double exposureFrames = 0;
    float gain = 1;
    int count = 1;
    std::array<double, 9> positions{};
    std::array<float, 9> weights{};
};

/** Build one physical exposure in source-frame coordinates, independent of playback FPS. */
CaptureExposurePlan planCaptureExposure(CaptureInterval interval, double sourceFps,
    const CadenceSettings& settings);

/** Count physical exposures intersecting the source duration, without output-grid normalization. */
std::int64_t physicalCaptureCount(HandCrankSchedule& schedule, double sourceDuration,
    const HandCrankSettings& settings);

} // namespace silentfilm
