#pragma once

#include "media_io.hpp"

namespace silentfilm::offline {

struct FlowFields {
    bool stationaryRgb = false;
    std::shared_ptr<const SourceFrame> first, second;
    std::vector<float> forward, backward;
};

/** Interpolate adjacent RGB(A) frames with Comfy-compatible bidirectional flow and confidence guards. */
class FlowSampler {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    FlowSampler(int width, int height, std::size_t cacheBytes, std::function<bool()> cancelled);
    ~FlowSampler();
    double elapsedSeconds() const;
    std::shared_ptr<const FlowFields> fields(std::shared_ptr<const SourceFrame> first,
        std::shared_ptr<const SourceFrame> second);
    std::shared_ptr<const SourceFrame> sample(std::shared_ptr<const SourceFrame> first,
        std::shared_ptr<const SourceFrame> second, float mix);
};

} // namespace silentfilm::offline
