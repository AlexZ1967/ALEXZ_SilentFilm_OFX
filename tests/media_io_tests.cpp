#include "media_io.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace silentfilm::offline;
void compareProvider(const char* metadata, const char* firstPng, const char* secondPng) {
    VideoReader first(firstPng,32*1024*1024,silentfilm::Transfer::SRGB,[]{return false;});
    VideoReader second(secondPng,32*1024*1024,silentfilm::Transfer::SRGB,[]{return false;});
    const auto expectedA=first.get(0), expectedB=second.get(0);
    VideoReader remote(metadata,32*1024*1024,silentfilm::Transfer::SRGB,[]{return false;},true);
    const auto equal=[](const SourceFrame& a,const SourceFrame& b) {
        if(a.pixels.size()!=b.pixels.size())throw std::runtime_error("Provider image size mismatch");
        for(std::size_t i=0;i<a.pixels.size();++i) {
            const auto& p=a.pixels[i];const auto& q=b.pixels[i];
            if(p.r!=q.r||p.g!=q.g||p.b!=q.b||p.a!=q.a)
                throw std::runtime_error("Independent PNG frames were corrupted before GPU processing");
        }
    };
    const auto retained=remote.get(0);
    for(int index:{0,1,2,3,4,1,0})equal(*remote.get(index),index%2 ? *expectedB : *expectedA);
    equal(*retained,*expectedA);
    std::cout<<"Independent 16-bit PNG pixel comparison: PASS\n";
}
int main(int argc, char** argv) {
    try {
        if(argc==5 && std::string(argv[1])=="--provider") {compareProvider(argv[2],argv[3],argv[4]);return 0;}
        if (argc != 2) throw std::runtime_error("Expected CFR fixture path");
        VideoReader source(argv[1],32*1024*1024,silentfilm::Transfer::SRGB,[] {return false;});
        std::vector<std::shared_ptr<const SourceFrame>> frames;
        for (int i=0;i<25;++i) frames.push_back(source.get(i));
        VideoReader mapped(argv[1],32*1024*1024,silentfilm::Transfer::SRGB,[] {return false;});
        mapped.setTimelineRange(5,15,20,30);
        if (std::abs(mapped.fps()-30)>1e-9 || mapped.frameCountHint()!=20)
            throw std::runtime_error("Timeline clock was not applied");
        for (int i=0;i<20;++i) {
            const int expected=5+((2*i+1)*15)/40;
            const auto actual=mapped.get(i);
            if(actual->index!=expected || actual->pixels.size()!=frames[expected]->pixels.size())
                throw std::runtime_error("Wrong raw frame mapping");
            for(std::size_t j=0;j<actual->pixels.size();++j) {
                const auto& a=actual->pixels[j];const auto& b=frames[expected]->pixels[j];
                if(a.r!=b.r||a.g!=b.g||a.b!=b.b||a.a!=b.a)
                    throw std::runtime_error("Mapped source pixels changed");
            }
        }
        if(mapped.locate(20)!=19 || !mapped.eof() || mapped.decodedCount()!=20)
            throw std::runtime_error("Incorrect timeline EOF");
        VideoReader invalid(argv[1],32*1024*1024,silentfilm::Transfer::SRGB,[] {return false;});
        invalid.setTimelineRange(20,15,20,25);
        bool rejected=false;
        try {invalid.get(19);}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Invalid source trim was silently clamped");
        std::cout<<"Native timeline mapping: PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
