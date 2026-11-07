#include "silentfilm/cadence.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void physicalCounts()
{
    for (float rate : {16.0f, 20.0f, 22.0f, 30.0f}) {
        silentfilm::HandCrankSchedule schedule;
        silentfilm::CadenceSettings settings;
        settings.crank.minimumFps = settings.crank.maximumFps = rate;
        settings.motionBlurStrength = 0;
        const auto count = silentfilm::physicalCaptureCount(schedule, 10, settings.crank);
        require(count == static_cast<std::int64_t>(rate * 10), "Offline cadence incorrectly preserves source frame count");
        // Например, 160 снятых кадров при 25 fps должны дать 6,4 с вместо 10 с.
        require(std::abs(static_cast<double>(count) / 25 - rate * 0.4) < 1e-9, "Wrong physical projection duration");
        for (std::int64_t i = 0; i < count; ++i) {
            const auto capture = schedule.captureAtIndex(i, settings.crank);
            require(capture.frame == i && std::abs(capture.start - i / static_cast<double>(rate)) < 1e-9,
                "Offline output does not advance exactly one physical exposure");
            const auto plan = silentfilm::planCaptureExposure(capture, 25, settings);
            require(plan.count == 1 && plan.weights[0] == 1, "Disabled shutter still integrates multiple frames");
            const double expected = (i + 0.5) * 25 / rate - 0.5;
            require(std::abs(plan.positions[0] - expected) < 1e-8, "Physical exposure was normalized to output duration");
        }
        require(silentfilm::physicalCaptureCount(schedule, 0.001, settings.crank) == 1, "Short source lost its physical exposure");
    }
}

void variableExposure()
{
    silentfilm::HandCrankSchedule schedule;
    silentfilm::CadenceSettings settings;
    settings.crank.minimumFps = 10; settings.crank.maximumFps = 30; settings.crank.rateVariation = 1;
    settings.exposureSamples = 5;
    const auto count = silentfilm::physicalCaptureCount(schedule, 12, settings.crank);
    require(count < 300 && count > 120, "Variable cadence has an implausible physical output count");
    std::vector<silentfilm::CaptureInterval> captures;
    double shortest = 1, longest = 0;
    for (std::int64_t i = 0; i < count; ++i) {
        const auto shot = schedule.captureAtIndex(i, settings.crank);
        captures.push_back(shot);
        shortest = std::min(shortest, shot.end - shot.start); longest = std::max(longest, shot.end - shot.start);
        const auto plan = silentfilm::planCaptureExposure(shot, 25, settings);
        require(std::abs(plan.gain - 20 / shot.fps) < 1e-6, "Exposure does not follow physical crank speed");
        require(plan.count == 5 && plan.weights[2] > plan.weights[1] && plan.weights[1] > plan.weights[0],
            "Offline shutter is not triangular");
        require(std::abs(plan.positions[0] + plan.positions[4] - 2 * plan.sourceCenter) < 1e-9,
            "Shutter integration is not centered on its physical exposure");
    }
    require(longest / shortest > 2.4, "Physical projection lost crank speed variation");
    (void)schedule.phaseAt(60, settings.crank);
    for (std::int64_t i = count; i-- > 0;) {
        const auto again = schedule.captureAtIndex(i, settings.crank);
        require(again.start == captures[i].start && again.frame == i, "Physical export depends on render order");
        const auto focus = silentfilm::cameraFocusSignal(i, 1925);
        require(std::isfinite(focus) && focus == silentfilm::cameraFocusSignal(i, 1925), "Focus differs between offline requests");
    }
}
} // namespace

int main()
{
    try { physicalCounts(); variableExposure(); std::cout << "Physical cadence: PASS\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
