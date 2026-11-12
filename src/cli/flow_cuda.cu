#include "flow_cuda.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <vector>

namespace silentfilm::offline {
namespace {
void checked(cudaError_t code) {
    if (code != cudaSuccess) throw std::runtime_error(cudaGetErrorString(code));
}

__device__ float mixValue(float a, float b, float t) { return a * (1 - t) + b * t; }
__device__ Pixel mixValue(Pixel a, Pixel b, float t) {
    return {mixValue(a.r,b.r,t), mixValue(a.g,b.g,t), mixValue(a.b,b.b,t), mixValue(a.a,b.a,t)};
}
__device__ float2 mixValue(float2 a, float2 b, float t) {
    return make_float2(mixValue(a.x,b.x,t), mixValue(a.y,b.y,t));
}

template<class T> __device__ T remap(const T* data, int width, int height, float x, float y) {
    // Comfy torch и OpenCV remap используют координаты, округлённые до 1/32 пикселя.
    x = fminf(width - 1, fmaxf(0, __int2float_rn(__float2int_rn(x * 32)) / 32));
    y = fminf(height - 1, fmaxf(0, __int2float_rn(__float2int_rn(y * 32)) / 32));
    const int ix = static_cast<int>(floorf(x)), iy = static_cast<int>(floorf(y));
    const int nx = min(ix + 1, width - 1), ny = min(iy + 1, height - 1);
    return mixValue(mixValue(data[iy * width + ix], data[iy * width + nx], x - ix),
                    mixValue(data[ny * width + ix], data[ny * width + nx], x - ix), y - iy);
}

__device__ float consistency(const float2* reverse, float2 flow, int x, int y, int width, int height) {
    const float xx = x + flow.x, yy = y + flow.y;
    if (xx < 0 || yy < 0 || xx > width - 1 || yy > height - 1) return 0;
    const auto r = remap(reverse, width, height, xx, yy);
    const float dx = flow.x + r.x, dy = flow.y + r.y;
    const float magnitude = flow.x * flow.x + flow.y * flow.y + r.x * r.x + r.y * r.y;
    return expf(-(dx * dx + dy * dy) / (0.5f + 0.01f * magnitude));
}

__global__ void prepare(const float2* forward, const float2* backward, const Pixel* first, const Pixel* second,
    float* confA, float* confB, float* difference, int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const int i = y * width + x;
    confA[i] = consistency(backward, forward[i], x, y, width, height);
    confB[i] = consistency(forward, backward[i], x, y, width, height);
    const auto a = first[i], b = second[i];
    difference[i] = (fabsf(a.r-b.r) + fabsf(a.g-b.g) + fabsf(a.b-b.b)) / 3;
}

__global__ void mask(const float2* forward, const float2* backward, const Pixel* first, const Pixel* second,
    const float* confA, const float* confB, const float* difference, float t, float* result, int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const int i = y * width + x;
    const auto f = forward[i], b = backward[i];
    const float ax = x - f.x * t, ay = y - f.y * t;
    const float bx = x - b.x * (1-t), by = y - b.y * (1-t);
    const auto a = remap(first, width, height, ax, ay), bb = remap(second, width, height, bx, by);
    const float c = fminf(fminf(confA[i], confB[i]), fminf(remap(confA,width,height,ax,ay), remap(confB,width,height,bx,by)));
    const float disagreement = (fabsf(a.r-bb.r) + fabsf(a.g-bb.g) + fabsf(a.b-bb.b)) / 3;
    result[i] = c * expf(-fmaxf(0, disagreement - difference[i]) / 0.03f);
}

__device__ int border(int value, int size, bool reflect) {
    if (!reflect) return max(0, min(size - 1, value));
    while (value < 0 || value >= size) value = value < 0 ? -value : 2 * size - 2 - value;
    return value;
}

__global__ void blend(const float2* forward, const float2* backward, const Pixel* first, const Pixel* second,
    const float* confidence, float t, Pixel* output, int width, int height, bool stationary) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const int i = y * width + x;
    const auto simple = mixValue(first[i], second[i], t);
    if (stationary) { output[i] = {first[i].r, first[i].g, first[i].b, simple.a}; return; }
    const float weights[5]{1.f/16, 4.f/16, 6.f/16, 4.f/16, 1.f/16};
    float c = 0;
    const bool reflect = min(width, height) > 2;
    for (int yy = -2; yy <= 2; ++yy)
        for (int xx = -2; xx <= 2; ++xx)
            c += confidence[border(y+yy,height,reflect)*width + border(x+xx,width,reflect)] * weights[yy+2] * weights[xx+2];
    const auto f = forward[i], b = backward[i];
    const auto warped = mixValue(remap(first,width,height,x-f.x*t,y-f.y*t),
                                 remap(second,width,height,x-b.x*(1-t),y-b.y*(1-t)), t);
    output[i] = mixValue(simple, warped, c);
}
} // namespace

struct CudaFlowSampler::Impl {
    struct Pair { float* storage = nullptr; bool stationary = false; std::uint64_t used = 0; };
    int width, height;
    std::size_t count, capacity;
    cudaStream_t stream;
    float* scratch = nullptr;
    std::map<std::pair<std::int64_t,std::int64_t>, Pair> pairs;
    std::vector<std::shared_ptr<const FlowFields>> uploads;
    std::uint64_t clock = 0;
};

CudaFlowSampler::CudaFlowSampler(int width, int height, std::size_t bytes, cudaStream_t stream)
    : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    if (width <= 0 || height <= 0) throw std::runtime_error("Invalid CUDA flow dimensions");
    s.width = width; s.height = height; s.count = static_cast<std::size_t>(width)*height; s.stream = stream;
    s.capacity = std::clamp<std::size_t>(bytes / (s.count*7*sizeof(float)), 1, 4);
    checked(cudaMallocAsync(reinterpret_cast<void**>(&s.scratch), s.count*sizeof(float), stream));
}
CudaFlowSampler::~CudaFlowSampler() {
    auto& s = *impl_;
    for (auto& entry : s.pairs) if (entry.second.storage) cudaFreeAsync(entry.second.storage, s.stream);
    if (s.scratch) cudaFreeAsync(s.scratch,s.stream);
    cudaStreamSynchronize(s.stream);
}

void CudaFlowSampler::sample(std::shared_ptr<const FlowFields> fields, const Pixel* first, const Pixel* second,
    float t, Pixel* output) {
    auto& s = *impl_;
    if (!fields || !fields->first || !fields->second || !first || !second || !output || !(t >= 0 && t <= 1))
        throw std::runtime_error("Invalid CUDA flow sample");
    const auto key = std::make_pair(fields->first->index, fields->second->index);
    auto found = s.pairs.find(key);
    if (found == s.pairs.end()) {
        while (s.pairs.size() >= s.capacity) {
            auto oldest = std::min_element(s.pairs.begin(),s.pairs.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
            if (oldest->second.storage) checked(cudaFreeAsync(oldest->second.storage,s.stream));
            s.pairs.erase(oldest);
        }
        Impl::Pair pair;
        pair.stationary = fields->stationaryRgb;
        if (!pair.stationary) {
            if (fields->forward.size() != 2*s.count || fields->backward.size() != 2*s.count)
                throw std::runtime_error("Invalid CUDA flow fields");
            checked(cudaMallocAsync(reinterpret_cast<void**>(&pair.storage),s.count*7*sizeof(float),s.stream));
        }
        found = s.pairs.emplace(key,pair).first;
        if (!pair.stationary) {
            // Retain host arrays until asynchronous uploads finish.
            s.uploads.push_back(fields);
            checked(cudaMemcpyAsync(pair.storage,fields->forward.data(),s.count*2*sizeof(float),cudaMemcpyHostToDevice,s.stream));
            checked(cudaMemcpyAsync(pair.storage+2*s.count,fields->backward.data(),s.count*2*sizeof(float),cudaMemcpyHostToDevice,s.stream));
            const dim3 block(16,16), grid((s.width+15)/16,(s.height+15)/16);
            prepare<<<grid,block,0,s.stream>>>(reinterpret_cast<float2*>(pair.storage),reinterpret_cast<float2*>(pair.storage+2*s.count),
                first,second,pair.storage+4*s.count,pair.storage+5*s.count,pair.storage+6*s.count,s.width,s.height);
            checked(cudaGetLastError());
        }
    }
    auto& pair = found->second; pair.used = ++s.clock;
    auto* p = pair.storage;
    const dim3 block(16,16), grid((s.width+15)/16,(s.height+15)/16);
    if (!pair.stationary) {
        mask<<<grid,block,0,s.stream>>>(reinterpret_cast<float2*>(p),reinterpret_cast<float2*>(p+2*s.count),first,second,
            p+4*s.count,p+5*s.count,p+6*s.count,t,s.scratch,s.width,s.height);
        checked(cudaGetLastError());
    }
    blend<<<grid,block,0,s.stream>>>(reinterpret_cast<float2*>(p),pair.stationary?nullptr:reinterpret_cast<float2*>(p+2*s.count),
        first,second,s.scratch,t,output,s.width,s.height,pair.stationary);
    checked(cudaGetLastError());
}
void CudaFlowSampler::releaseUploadsAfterSync() { impl_->uploads.clear(); }
std::size_t CudaFlowSampler::cachedBytes() const {
    std::size_t bytes = 0;
    for (const auto& entry : impl_->pairs) if (entry.second.storage) bytes += impl_->count*7*sizeof(float);
    return bytes;
}
void CudaFlowSampler::trimToBudget(std::size_t bytes) {
    auto& s = *impl_;
    s.capacity = std::clamp<std::size_t>(bytes/(s.count*7*sizeof(float)),1,4);
    while (s.pairs.size() > s.capacity) {
        auto oldest=std::min_element(s.pairs.begin(),s.pairs.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
        if(oldest->second.storage)checked(cudaFreeAsync(oldest->second.storage,s.stream));
        s.pairs.erase(oldest);
    }
}
} // namespace silentfilm::offline
