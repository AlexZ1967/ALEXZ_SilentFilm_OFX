#include "flow_cuda.hpp"
#include <cuda_runtime_api.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace silentfilm;
using namespace silentfilm::offline;
namespace {
void check(cudaError_t code) { if (code != cudaSuccess) throw std::runtime_error(cudaGetErrorString(code)); }
struct Device {
    cudaStream_t stream = nullptr;
    Pixel *first = nullptr, *second = nullptr, *output = nullptr;
    std::unique_ptr<CudaFlowSampler> sampler;
    std::size_t count;
    Device(int w, int h) : count(static_cast<std::size_t>(w)*h) {
        check(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
        check(cudaMalloc(reinterpret_cast<void**>(&first),count*sizeof(Pixel)));
        check(cudaMalloc(reinterpret_cast<void**>(&second),count*sizeof(Pixel)));
        check(cudaMalloc(reinterpret_cast<void**>(&output),count*sizeof(Pixel)));
        sampler = std::make_unique<CudaFlowSampler>(w,h,count*7*sizeof(float),stream);
    }
    ~Device() {
        sampler.reset();
        cudaFree(first); cudaFree(second); cudaFree(output);
        cudaStreamDestroy(stream);
    }
    std::vector<Pixel> sample(FlowSampler& host, std::shared_ptr<SourceFrame> a, std::shared_ptr<SourceFrame> b, float t) {
        check(cudaMemcpyAsync(first,a->pixels.data(),count*sizeof(Pixel),cudaMemcpyHostToDevice,stream));
        check(cudaMemcpyAsync(second,b->pixels.data(),count*sizeof(Pixel),cudaMemcpyHostToDevice,stream));
        sampler->sample(host.fields(a,b),first,second,t,output);
        std::vector<Pixel> result(count);
        check(cudaMemcpyAsync(result.data(),output,count*sizeof(Pixel),cudaMemcpyDeviceToHost,stream));
        check(cudaStreamSynchronize(stream)); sampler->releaseUploadsAfterSync();
        return result;
    }
};
void compare(const std::vector<Pixel>& a,const std::vector<Pixel>& b) {
    double sum=0; float maximum=0;
    for (std::size_t i=0;i<a.size();++i) {
        const float errors[]{fabsf(a[i].r-b[i].r),fabsf(a[i].g-b[i].g),fabsf(a[i].b-b[i].b),fabsf(a[i].a-b[i].a)};
        for(float e:errors) { if(!std::isfinite(e)) throw std::runtime_error("Non-finite CUDA flow output"); sum+=e;maximum=std::max(maximum,e); }
    }
    const double mean=sum/(4*a.size());
    std::cout<<"max_error="<<maximum<<" mean_error="<<mean<<'\n';
    if(maximum>1e-3f || mean>1e-5) throw std::runtime_error("CUDA flow mismatch");
}
void fixture(const char* path) {
    std::ifstream input(path,std::ios::binary);
    const auto read=[&](void* p,std::size_t n){if(!input.read(static_cast<char*>(p),n))throw std::runtime_error("Incomplete fixture");};
    char magic[4];read(magic,4);
    if(std::string(magic,4)!="SFF1")throw std::runtime_error("Unknown fixture");
    std::uint32_t dims[3];read(dims,sizeof(dims));
    if(!dims[0]||!dims[1]||dims[0]>4096||dims[1]>4096||!dims[2]||dims[2]>32)throw std::runtime_error("Invalid dimensions");
    const std::size_t count=static_cast<std::size_t>(dims[0])*dims[1];
    auto a=std::make_shared<SourceFrame>(),b=std::make_shared<SourceFrame>();
    a->index=0;b->index=1;a->pixels.resize(count);b->pixels.resize(count);
    read(a->pixels.data(),count*sizeof(Pixel));read(b->pixels.data(),count*sizeof(Pixel));
    FlowSampler host(dims[0],dims[1],64*1024*1024,{});
    Device device(dims[0],dims[1]);
    for(std::uint32_t i=0;i<dims[2];++i) {
        float t;read(&t,sizeof(t));std::vector<Pixel> expected(count);read(expected.data(),count*sizeof(Pixel));
        compare(device.sample(host,a,b,t),expected);
    }
}
void parity(int width,int height) {
    const std::size_t count=static_cast<std::size_t>(width)*height;
    auto a=std::make_shared<SourceFrame>(),b=std::make_shared<SourceFrame>();
    a->index=0;b->index=1;a->pixels.resize(count);b->pixels.resize(count);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const auto scene=[&](int xx){return Pixel{0.4f+0.2f*std::sin(xx*0.21f)*std::cos(y*0.3f),0.3f,0.2f,0.6f};};
        a->pixels[y*width+x]=scene(x);b->pixels[y*width+x]=scene(x-3);
    }
    FlowSampler host(width,height,64*1024*1024,{}),cpu(width,height,64*1024*1024,{});
    Device device(width,height);
    for(float t:{0.2f,0.5f,0.8f})compare(device.sample(host,a,b,t),cpu.sample(a,b,t)->pixels);
    // Точное совпадение RGB с разной alpha не должно деформировать сцену.
    auto c=std::make_shared<SourceFrame>(*a);c->index=2;
    for(auto& p:c->pixels)p.a=0.2f;
    compare(device.sample(host,a,c,0.25f),cpu.sample(a,c,0.25f)->pixels);
    // Кэш на одну пару: повторная загрузка после вытеснения должна дать тот же результат.
    compare(device.sample(host,a,b,0.5f),cpu.sample(a,b,0.5f)->pixels);
    device.sampler->trimToBudget(0);
    if(device.sampler->cachedBytes()>count*7*sizeof(float))throw std::runtime_error("GPU cache exceeded one-pair minimum");
}
}
int main(int argc,char** argv) {
    int devices=0;if(cudaGetDeviceCount(&devices)!=cudaSuccess||devices==0){std::cout<<"CUDA unavailable\n";return 77;}
    try {
        if(argc==3&&std::string(argv[1])=="--compare-comfy")fixture(argv[2]);
        else if(argc==1){parity(1,1);parity(9,7);parity(96,64);}
        else throw std::runtime_error("Usage: silentfilm_cadence_flow_cuda_tests [--compare-comfy fixture]");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
