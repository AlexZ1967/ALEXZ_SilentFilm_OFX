#include "silentfilm/camera_capture.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

/** Reject the previous source-time mapping by requiring uniform playback of captured frames. */
void projectedFrames()
{
    silentfilm::HandCrankSchedule schedule;
    silentfilm::HandCrankSettings settings;
    settings.minimumFps = 10; settings.maximumFps = 30;
    settings.rateVariation = 1; settings.irregularity = 0;
    constexpr double duration = 12;
    const auto last = schedule.intervalAt(duration - 1e-8, settings);
    const auto count = last.frame + 1;
    double shortest = 1, longest = 0;
    std::vector<silentfilm::CaptureInterval> captured;
    for (std::int64_t index = 0; index < count; ++index) {
        const double outputTime = (index + 0.5) * duration / count;
        const auto shot = schedule.projectedIntervalAt(outputTime, duration, settings);
        require(shot.frame == index, "Projection did not advance captured frames uniformly");
        const double length = shot.end - shot.start;
        shortest = std::min(shortest, length); longest = std::max(longest, length);
        require(std::abs(length * shot.fps - 1) < 1e-10, "Exposure and cadence use different capture speeds");
        if (!captured.empty()) require(std::abs(shot.start - captured.back().end) < 1e-12, "Capture intervals are not contiguous");
        captured.push_back(shot);
    }
    require(longest / shortest > 2.5, "10-30 FPS did not produce a substantial projection speed variation");
    schedule.clear(); schedule.clear();
    for (std::size_t i = captured.size(); i-- > 0;) {
        const auto rebuilt = schedule.captureAtIndex(static_cast<std::int64_t>(i), settings);
        require(rebuilt.start == captured[i].start && rebuilt.end == captured[i].end && rebuilt.fps == captured[i].fps,
            "Purging crank intervals changed the reconstructed capture");
    }
    require(schedule.projectedIntervalAt(0, duration, settings).frame == 0, "Projection lost the first capture");
    require(schedule.projectedIntervalAt(duration, duration, settings).frame == last.frame, "Projection did not reach the last capture");
    // Уже построенная таблица более длинного клипа не должна менять коэффициент проекции.
    (void)schedule.intervalAt(duration * 3, settings);
    for (std::int64_t index = count - 1; index >= 0; --index) {
        const auto shot = schedule.projectedIntervalAt((index + 0.5) * duration / count, duration, settings);
        require(shot.frame == captured[index].frame && shot.start == captured[index].start
            && shot.phase == captured[index].phase, "Projection depends on render order or cached horizon");
    }
    double maximumOffset = 0;
    for (int frame = 0; frame < 300; ++frame) {
        const double outputTime = (frame + 0.5) / 25;
        const auto shot = schedule.projectedIntervalAt(outputTime, duration, settings);
        const double sourceCenter = 0.5 * (shot.start + shot.end);
        maximumOffset = std::max(maximumOffset, std::abs(sourceCenter - outputTime));
    }
    require(maximumOffset > 0.02, "Projected source time still follows output time too closely");
}

/** Preserve total duration and expected holds for a constant-rate camera. */
void constantRate()
{
    silentfilm::HandCrankSchedule schedule;
    silentfilm::HandCrankSettings settings;
    settings.minimumFps = settings.maximumFps = 20;
    constexpr double duration = 12;
    for (int frame = 0; frame < 300; ++frame) {
        const auto shot = schedule.projectedIntervalAt((frame + 0.5) / 25, duration, settings);
        const auto expected = static_cast<std::int64_t>((2 * frame + 1) * 20 / 50);
        require(shot.frame == expected, "Constant-rate projection has unexpected frame holds or end-frame freeze");
        require(std::abs(shot.start - expected / 20.0) < 1e-10, "Constant-rate source position drifted");
    }
    // Независимый контроль точных границ и двух сторон от них.
    for (std::int64_t frame = 1; frame < 240; ++frame) {
        const double boundary = static_cast<double>(frame) / 20;
        require(schedule.projectedIntervalAt(boundary, duration, settings).frame == frame,
            "Exact projection boundary selected the preceding capture");
        require(schedule.projectedIntervalAt(boundary - 1e-10, duration, settings).frame == frame - 1,
            "Projection advanced before the capture boundary");
        require(schedule.projectedIntervalAt(boundary + 1e-10, duration, settings).frame == frame,
            "Projection did not advance after the capture boundary");
    }
    const auto tiny = schedule.projectedIntervalAt(0.005, 0.01, settings);
    require(tiny.frame == 0, "A sub-exposure clip should still project its first capture");
    bool rejected = false;
    try { (void)schedule.projectedIntervalAt(0, 0, settings); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Zero-duration projection was not rejected");
}

} // namespace

int main()
{
    try {
        projectedFrames(); constantRate();
        std::cout << "Camera timing: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
