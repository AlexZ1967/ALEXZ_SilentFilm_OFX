#pragma once

#include "media_io.hpp"

namespace silentfilm::offline {

/** Interpolate adjacent RGB(A) frames with Comfy-compatible bidirectional flow and confidence guards. */
class FlowSampler {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    FlowSampler(int width, int height, std::size_t cacheBytes, std::function<bool()> cancelled);
    ~FlowSampler();
    std::shared_ptr<const SourceFrame> sample(std::shared_ptr<const SourceFrame> first,
        std::shared_ptr<const SourceFrame> second, float mix);
};

} // namespace silentfilm::offline
