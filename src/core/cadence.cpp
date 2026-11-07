#include "silentfilm/cadence.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace silentfilm {

CaptureExposurePlan planCaptureExposure(CaptureInterval interval, double sourceFps, const CadenceSettings& s)
{
    if (!std::isfinite(sourceFps) || sourceFps <= 0 || interval.end <= interval.start || interval.fps <= 0
        || s.exposureSamples < 1 || s.exposureSamples > 9)
        throw std::invalid_argument("Invalid physical capture exposure");
    CaptureExposurePlan plan;
    plan.interval = interval;
    const double duration = interval.end - interval.start;
    plan.sourceCenter = (interval.start + duration * 0.5) * sourceFps - 0.5;
    plan.exposureFrames = duration * sourceFps * s.shutterFraction * s.motionBlurStrength;
    if (s.motionBlurStrength > 0)
        plan.exposureFrames = std::max(plan.exposureFrames, 0.28 * std::max(1.0, duration * sourceFps) * s.motionBlurStrength);
    plan.gain = static_cast<float>(std::pow((0.5 * (s.crank.minimumFps + s.crank.maximumFps)) / interval.fps, s.exposureVariation));
    plan.count = plan.exposureFrames > 1e-4 ? s.exposureSamples : 1;
    for (int i = 0; i < plan.count; ++i) {
        const double offset = plan.count > 1 ? static_cast<double>(i) / (plan.count - 1) - 0.5 : 0;
        plan.positions[i] = plan.sourceCenter + offset * plan.exposureFrames;
        plan.weights[i] = plan.count == 1 ? 1 : std::max(1e-4f, 1 - std::abs(static_cast<float>(offset)) * 2);
    }
    return plan;
}

std::int64_t physicalCaptureCount(HandCrankSchedule& schedule, double duration, const HandCrankSettings& settings)
{
    if (!std::isfinite(duration) || duration <= 0) throw std::invalid_argument("Invalid source duration");
    return schedule.intervalAt(duration - std::min(1e-9, duration * 0.5), settings).frame + 1;
}

} // namespace silentfilm
