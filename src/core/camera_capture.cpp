#include "silentfilm/camera_capture.hpp"

#include "silentfilm/probe.hpp"
#include "silentfilm/sensitometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace silentfilm {
namespace {

constexpr double tau = 6.283185307179586476925286766559;

std::uint64_t mixBits(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

double randomUnit(std::uint32_t seed, std::int64_t index, std::uint64_t lane)
{
    const auto value = mixBits(static_cast<std::uint64_t>(seed) ^ mixBits(static_cast<std::uint64_t>(index)) ^ lane);
    return static_cast<double>(value >> 11) * (1.0 / 9007199254740992.0);
}

double randomSigned(std::uint32_t seed, std::int64_t index, std::uint64_t lane)
{
    return randomUnit(seed, index, lane) * 2.0 - 1.0;
}

bool sameSettings(const HandCrankSettings& a, const HandCrankSettings& b)
{
    return a.minimumFps == b.minimumFps && a.maximumFps == b.maximumFps
        && a.rateVariation == b.rateVariation && a.irregularity == b.irregularity && a.seed == b.seed;
}

Pixel readPixel(ImageView image, int x, int y)
{
    x = std::clamp(x, image.x1, image.x2 - 1);
    y = std::clamp(y, image.y1, image.y2 - 1);
    Pixel pixel{};
    const auto* address = image.data + (static_cast<std::ptrdiff_t>(y) - image.y1) * image.rowBytes
        + (static_cast<std::ptrdiff_t>(x) - image.x1) * sizeof(Pixel);
    std::memcpy(&pixel, address, sizeof(pixel));
    return pixel;
}

Pixel straightPixel(Pixel value, bool premultiplied)
{
    if (premultiplied) {
        if (value.a <= 0) value.r = value.g = value.b = 0;
        else { value.r /= value.a; value.g /= value.a; value.b /= value.a; }
    }
    value.r = std::clamp(value.r, 0.0f, 1.0f);
    value.g = std::clamp(value.g, 0.0f, 1.0f);
    value.b = std::clamp(value.b, 0.0f, 1.0f);
    return value;
}

Pixel blendPixels(Pixel a, Pixel b, float t)
{
    return {a.r * (1 - t) + b.r * t, a.g * (1 - t) + b.g * t,
        a.b * (1 - t) + b.b * t, a.a * (1 - t) + b.a * t};
}

Pixel sampleBilinear(ImageView image, double x, double y, bool premultiplied)
{
    x = std::clamp(x, static_cast<double>(image.x1), static_cast<double>(image.x2 - 1));
    y = std::clamp(y, static_cast<double>(image.y1), static_cast<double>(image.y2 - 1));
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, image.x2 - 1), y1 = std::min(y0 + 1, image.y2 - 1);
    const float fx = static_cast<float>(x - x0), fy = static_cast<float>(y - y0);
    const Pixel a = straightPixel(readPixel(image, x0, y0), premultiplied);
    const Pixel b = straightPixel(readPixel(image, x1, y0), premultiplied);
    const Pixel c = straightPixel(readPixel(image, x0, y1), premultiplied);
    const Pixel d = straightPixel(readPixel(image, x1, y1), premultiplied);
    const auto blend = [fx, fy](float aa, float bb, float cc, float dd) {
        return ((aa * (1 - fx) + bb * fx) * (1 - fy)) + ((cc * (1 - fx) + dd * fx) * fy);
    };
    return {blend(a.r, b.r, c.r, d.r), blend(a.g, b.g, c.g, d.g),
        blend(a.b, b.b, c.b, d.b), blend(a.a, b.a, c.a, d.a)};
}

double ease(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
}

std::array<double, 3> cameraAim(std::uint32_t seed, std::int64_t index)
{
    constexpr std::uint64_t lane = 0x41494d504f494e54ULL;
    return {randomSigned(seed, index, lane), randomSigned(seed, index, lane + 1), randomSigned(seed, index, lane + 2)};
}

std::array<double, 3> cameraPose(std::uint32_t seed, double crankPhase)
{
    const double slowPosition = crankPhase / 3.0;
    const auto anchor = static_cast<std::int64_t>(std::floor(slowPosition));
    const double blend = ease(slowPosition - static_cast<double>(anchor));
    const auto first = cameraAim(seed, anchor), second = cameraAim(seed, anchor + 1);
    std::array<double, 3> pose{};
    for (int axis = 0; axis < 3; ++axis) pose[axis] = 0.6 * (first[axis] * (1 - blend) + second[axis] * blend);

    const auto turn = static_cast<std::int64_t>(std::floor(crankPhase));
    for (std::int64_t eventTurn = turn - 2; eventTurn <= turn + 1; ++eventTurn) {
        constexpr std::uint64_t lane = 0x4341505455524545ULL;
        if (randomUnit(seed, eventTurn, lane) <= 0.15) continue;
        const double onset = static_cast<double>(eventTurn) + 0.05 + 0.70 * randomUnit(seed, eventTurn, lane + 1);
        const int count = 1 + static_cast<int>(randomUnit(seed, eventTurn, lane + 2) < 0.35);
        double nextOnset = onset;
        for (int event = 0; event < count; ++event) {
            const auto eventIndex = eventTurn * 2 + event;
            const double attack = 0.12 + 0.12 * randomUnit(seed, eventIndex, lane + 3);
            const double recovery = 0.35 + 0.50 * randomUnit(seed, eventIndex, lane + 4);
            const double elapsed = crankPhase - nextOnset;
            if (elapsed >= 0 && elapsed < attack + recovery) {
                const double envelope = elapsed < attack ? ease(elapsed / attack)
                    : 1.0 - ease((elapsed - attack) / recovery);
                for (int axis = 0; axis < 3; ++axis) {
                    const double amplitude = axis == 0 ? -0.75 + 1.5 * randomUnit(seed, eventIndex, lane + 5)
                        : axis == 1 ? -0.8 + 1.3 * randomUnit(seed, eventIndex, lane + 6)
                        : -0.7 + 1.4 * randomUnit(seed, eventIndex, lane + 7);
                    pose[axis] += amplitude * envelope;
                }
            }
            nextOnset += 0.30 + 0.35 * randomUnit(seed, eventIndex, lane + 8);
        }
    }
    for (auto& value : pose) value = std::tanh(value);
    return pose;
}

} // namespace

void HandCrankSchedule::reset(const HandCrankSettings& settings)
{
    settings_ = settings;
    initialized_ = true;
    initialPhase_ = randomUnit(settings.seed, 0, 0x494e495450484153ULL);
    starts_.assign(1, 0.0);
    rates_.clear();
}

void HandCrankSchedule::extendTo(double time, const HandCrankSettings& settings)
{
    if (!initialized_ || !sameSettings(settings_, settings)) reset(settings);
    const double midpoint = 0.5 * (settings.minimumFps + settings.maximumFps);
    const double amplitude = 0.5 * (settings.maximumFps - settings.minimumFps)
        * settings.rateVariation;
    constexpr double irregularityMix = 0.35;
    while (starts_.back() <= time) {
        const auto frame = static_cast<std::int64_t>(rates_.size());
        const double revolutions = initialPhase_ + static_cast<double>(frame) / 8.0;
        const auto turn = static_cast<std::int64_t>(std::floor(revolutions));
        const double phase = revolutions - static_cast<double>(turn);
        const double smoothPhase = 0.5 - 0.5 * std::cos(3.14159265358979323846 * phase);
        const double previousTempo = randomSigned(settings.seed, turn - 1, 0x54454d504fULL);
        const double nextTempo = randomSigned(settings.seed, turn, 0x54454d504fULL);
        const double tempo = previousTempo * (1.0 - smoothPhase) + nextTempo * smoothPhase;
        const double cycle = std::sin(tau * phase);
        const double driftMix = irregularityMix * settings.irregularity;
        const double variation = cycle * (1.0 - driftMix) + tempo * driftMix;
        const double fps = std::clamp(midpoint + amplitude * variation,
            static_cast<double>(settings.minimumFps), static_cast<double>(settings.maximumFps));
        rates_.push_back(fps);
        starts_.push_back(starts_.back() + 1.0 / fps);
        if (rates_.size() > 10000000) break;
    }
}

CaptureInterval HandCrankSchedule::intervalForIndex(std::size_t index) const
{
    if (index >= rates_.size()) index = rates_.size() - 1;
    return {static_cast<std::int64_t>(index), starts_[index], starts_[index + 1],
        initialPhase_ + static_cast<double>(index) / 8.0, rates_[index]};
}

void HandCrankSchedule::clear()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<double>{}.swap(starts_);
    std::vector<double>{}.swap(rates_);
    initialized_ = false;
}

CaptureInterval HandCrankSchedule::intervalAt(double relativeTime, const HandCrankSettings& settings)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const double time = std::max(0.0, relativeTime);
    extendTo(time, settings);
    auto upper = std::upper_bound(starts_.begin(), starts_.end(), time);
    std::size_t index = upper == starts_.begin() ? 0 : static_cast<std::size_t>(upper - starts_.begin() - 1);
    return intervalForIndex(index);
}

CaptureInterval HandCrankSchedule::captureAtIndex(std::int64_t index, const HandCrankSettings& settings)
{
    if (index < 0 || index >= 10000000) throw std::invalid_argument("Invalid physical capture index");
    const std::lock_guard<std::mutex> lock(mutex_);
    extendTo(0, settings);
    while (rates_.size() <= static_cast<std::size_t>(index)) extendTo(starts_.back(), settings);
    return intervalForIndex(static_cast<std::size_t>(index));
}

float cameraFocusSignal(std::int64_t frame, std::uint32_t seed) noexcept
{
    float previous = 0;
    for (std::int64_t i = frame - 48; i <= frame; ++i) {
        const float noise = film::gaussianLattice(static_cast<int>(i), 0, seed ^ 0x464f4355u, 64u);
        previous = previous * 0.48f + noise * 0.52f;
    }
    return std::tanh(previous / 0.61f);
}

CaptureInterval HandCrankSchedule::projectedIntervalAt(double relativeOutputTime, double sourceDuration,
    const HandCrankSettings& settings)
{
    if (!std::isfinite(sourceDuration) || sourceDuration <= 0 || !std::isfinite(relativeOutputTime))
        throw std::invalid_argument("Invalid camera projection time or duration");
    const std::lock_guard<std::mutex> lock(mutex_);
    extendTo(sourceDuration, settings);
    // Число экспозиций определяется всем клипом, а не уже запрошенными кадрами кэша.
    const double boundary = sourceDuration - std::min(1e-9, sourceDuration * 0.5);
    const auto end = std::lower_bound(starts_.begin(), starts_.end(), boundary);
    const auto count = std::max<std::size_t>(1,
        std::min(rates_.size(), static_cast<std::size_t>(end - starts_.begin())));
    const double phase = std::clamp(relativeOutputTime / sourceDuration, 0.0, 1.0);
    double position = phase * static_cast<double>(count);
    // На точной границе деление может дать 5,999999999999999 вместо 6.
    // Допуск покрывает только погрешность double, не заметную долю кадра.
    const double nearest = std::round(position);
    const double tolerance = 8 * std::numeric_limits<double>::epsilon() * std::max(1.0, position);
    if (std::abs(position - nearest) <= tolerance) position = nearest;
    const auto index = std::min(count - 1, static_cast<std::size_t>(std::floor(position)));
    return intervalForIndex(index);
}

double HandCrankSchedule::phaseAt(double relativeTime, const HandCrankSettings& settings)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const double time = std::max(0.0, relativeTime);
    extendTo(time, settings);
    auto upper = std::upper_bound(starts_.begin(), starts_.end(), time);
    std::size_t index = upper == starts_.begin() ? 0 : static_cast<std::size_t>(upper - starts_.begin() - 1);
    if (index >= rates_.size()) index = rates_.size() - 1;
    const double duration = starts_[index + 1] - starts_[index];
    const double fraction = std::clamp((time - starts_[index]) / duration, 0.0, 1.0);
    return initialPhase_ + (static_cast<double>(index) + fraction) / 8.0;
}

CameraSampleTransform cameraSampleTransform(float phase, float strength,
    std::uint32_t seed, int width, int height)
{
    if (strength <= 0) return {};
    const auto pose = cameraPose(seed, phase);
    const double angle = 0.0013962634015954637 * strength * pose[2];
    return {std::cos(angle), std::sin(angle), strength * 0.003 * width * pose[0],
        strength * 0.002 * height * pose[1]};
}

bool renderCameraCaptureCpu(const CameraExposureSample* samples, int sampleCount,
    ImageView output, RenderWindow window, const CameraOpticsParameters& parameters,
    float exposureGain, float shakeStrength, std::uint32_t seed,
    AbortCallback abort, void* context)
{
    if (!samples || sampleCount <= 0) return false;
    const auto bounds = samples[0].image;
    const int width = bounds.x2 - bounds.x1, height = bounds.y2 - bounds.y1;
    if (width <= 0 || height <= 0) return true;
    std::vector<Pixel> combined(static_cast<std::size_t>(width) * height);
    float totalWeight = 0;
    for (int i = 0; i < sampleCount; ++i) totalWeight += samples[i].weight;
    if (!(totalWeight > 0)) return false;
    std::vector<CameraSampleTransform> transforms(static_cast<std::size_t>(sampleCount));
    if (shakeStrength > 0) {
        for (int index = 0; index < sampleCount; ++index) {
            transforms[static_cast<std::size_t>(index)] = cameraSampleTransform(
                samples[index].phase, shakeStrength, seed, width, height);
        }
    }
    for (int y = 0; y < height; ++y) {
        if (abort && abort(context)) return false;
        for (int x = 0; x < width; ++x) {
            const int absoluteX = bounds.x1 + x, absoluteY = bounds.y1 + y;
            const double centerX = bounds.x1 + (width - 1) * 0.5;
            const double centerY = bounds.y1 + (height - 1) * 0.5;
            Pixel sum{};
            for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
                const auto& sample = samples[sampleIndex];
                Pixel value;
                if (shakeStrength > 0) {
                    const auto& transform = transforms[static_cast<std::size_t>(sampleIndex)];
                    const double dx = absoluteX - centerX, dy = absoluteY - centerY;
                    const double sourceX = centerX + transform.cosine * dx + transform.sine * dy - transform.shiftX;
                    const double sourceY = centerY - transform.sine * dx + transform.cosine * dy - transform.shiftY;
                    value = sampleBilinear(sample.image, sourceX, sourceY, parameters.sourcePremultiplied);
                    if (sample.temporalMix > 0)
                        value = blendPixels(value, sampleBilinear(sample.nextImage, sourceX, sourceY,
                            parameters.sourcePremultiplied), sample.temporalMix);
                } else {
                    value = straightPixel(readPixel(sample.image, absoluteX, absoluteY), parameters.sourcePremultiplied);
                    if (sample.temporalMix > 0)
                        value = blendPixels(value, straightPixel(readPixel(sample.nextImage, absoluteX, absoluteY),
                            parameters.sourcePremultiplied), sample.temporalMix);
                }
                const float weight = sample.weight / totalWeight;
                sum.r += value.r * weight; sum.g += value.g * weight;
                sum.b += value.b * weight; sum.a += value.a * weight;
            }
            combined[static_cast<std::size_t>(y) * width + x] = sum;
        }
    }
    const ImageView merged{reinterpret_cast<unsigned char*>(combined.data()),
        static_cast<std::ptrdiff_t>(width) * static_cast<std::ptrdiff_t>(sizeof(Pixel)),
        bounds.x1, bounds.y1, bounds.x2, bounds.y2};
    if (parameters.captureOnly) {
        for (int y = std::max(window.y1, output.y1); y < std::min(window.y2, output.y2); ++y) {
            if (abort && abort(context)) return false;
            for (int x = std::max(window.x1, output.x1); x < std::min(window.x2, output.x2); ++x) {
                // За пределами захваченного кадра нужен прозрачный ноль.
                // readPixel используется sampler-ом и продлевает край изображения.
                auto pixel = x >= bounds.x1 && x < bounds.x2 && y >= bounds.y1 && y < bounds.y2
                    ? readPixel(merged, x, y) : Pixel{};
                const auto applyExposure = [exposureGain](float value) {
                    return film::encode(std::clamp(film::decode(value, Transfer::SRGB) * exposureGain,
                        0.0f, 1.0f), Transfer::SRGB);
                };
                pixel.r = applyExposure(pixel.r);
                pixel.g = applyExposure(pixel.g);
                pixel.b = applyExposure(pixel.b);
                if (parameters.outputPremultiplied) {
                    pixel.r *= pixel.a; pixel.g *= pixel.a; pixel.b *= pixel.a;
                }
                auto* destination = output.data + (static_cast<std::ptrdiff_t>(y) - output.y1) * output.rowBytes
                    + (static_cast<std::ptrdiff_t>(x) - output.x1) * sizeof(Pixel);
                std::memcpy(destination, &pixel, sizeof(pixel));
            }
        }
        return true;
    }
    auto cameraParameters = parameters;
    cameraParameters.sourcePremultiplied = false;
    return renderCameraCpu(merged, output, window, cameraParameters, exposureGain, abort, context);
}

} // namespace silentfilm
