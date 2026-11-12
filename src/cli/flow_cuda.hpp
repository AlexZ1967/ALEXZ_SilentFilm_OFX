#pragma once

#include "flow_sampler.hpp"
#include <cuda_runtime_api.h>

namespace silentfilm::offline {

/** Keep flow fields and confidence masks on CUDA; emit interpolated samples on the caller stream. */
class CudaFlowSampler {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    CudaFlowSampler(int width, int height, std::size_t cacheBytes, cudaStream_t stream);
    ~CudaFlowSampler();
    void sample(std::shared_ptr<const FlowFields> fields, const Pixel* first, const Pixel* second,
        float mix, Pixel* output);
    void releaseUploadsAfterSync();
    void trimToBudget(std::size_t bytes);
    std::size_t cachedBytes() const;
};

} // namespace silentfilm::offline
