#include "silentfilm/projector.hpp"

#include <algorithm>
#include <cmath>

namespace silentfilm {
namespace {

std::uint64_t hash(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

double normal(std::uint32_t seed, std::size_t frame, int lane)
{
    const auto key = hash(seed ^ (static_cast<std::uint64_t>(lane) << 32)) ^ hash(frame);
    const double u = (static_cast<double>(hash(key) >> 11) + 0.5) / 9007199254740992.0;
    const double v = (static_cast<double>(hash(key ^ 0x4c414d50ULL) >> 11) + 0.5) / 9007199254740992.0;
    return std::sqrt(-2 * std::log(u)) * std::cos(6.2831853071795864769 * v);
}

} // namespace

void ProjectorSignalCache::clear()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::array<float, 4>>{}.swap(signals_);
}

ProjectorFrameState ProjectorSignalCache::at(std::size_t frame, std::size_t count, std::uint32_t seed,
    float weave, float flicker, float breathing)
{
    if (count == 0 || (weave == 0 && flicker == 0 && breathing == 0)) return {};
    const std::lock_guard<std::mutex> lock(mutex_);
    if (signals_.size() != count || seed_ != seed) {
        signals_.resize(count);
        seed_ = seed;
        constexpr std::array<float, 4> smoothing{0.55f, 0.60f, 0.18f, 0.92f};
        for (int lane = 0; lane < 4; ++lane) {
            float previous = 0;
            double sum = 0;
            for (std::size_t i = 0; i < count; ++i) {
                previous = previous * smoothing[lane]
                    + static_cast<float>(normal(seed, i, lane)) * (1 - smoothing[lane]);
                signals_[i][lane] = previous;
                sum += previous;
            }
            const double mean = sum / count;
            double square = 0;
            for (const auto& sample : signals_) { const double v = sample[lane] - mean; square += v * v; }
            const double stddev = std::sqrt(square / count);
            for (auto& sample : signals_) sample[lane] = static_cast<float>(
                (sample[lane] - mean) / (stddev > 1e-8 ? stddev : 1));
        }
    }
    const auto& sample = signals_[std::min(frame, count - 1)];
    return {sample[0] * weave, sample[1] * (weave * 0.8f),
        std::max(0.0f, 1 + sample[2] * flicker + sample[3] * breathing)};
}

} // namespace silentfilm
