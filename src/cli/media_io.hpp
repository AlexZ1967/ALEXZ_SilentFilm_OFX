#pragma once

#include "silentfilm/sensitometry.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace silentfilm::offline {

struct SourceFrame {
    std::int64_t index = 0;
    std::vector<Pixel> pixels;
};

/** Decode CFR SDR Rec.709 source frames with a bounded cache, preserving native frame indices. */
class VideoReader {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    VideoReader(const std::string& path, std::size_t cacheBytes, Transfer inputTransfer,
        std::function<bool()> cancelled);
    ~VideoReader();
    int width() const;
    int height() const;
    double fps() const;
    std::int64_t frameCountHint() const;
    std::int64_t decodedCount() const;
    bool eof() const;
    std::int64_t locate(std::int64_t index);
    std::shared_ptr<const SourceFrame> get(std::int64_t index);
};

/** Write a finite RGB/alpha sequence as ProRes 4444 MOV or unsigned 16-bit TIFF frames. */
class FrameWriter {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    FrameWriter(const std::filesystem::path& directory, const std::string& format,
        int width, int height, double fps);
    ~FrameWriter();
    void write(const std::vector<Pixel>& pixels, std::int64_t index);
    void finish();
    std::filesystem::path mediaPath() const;
    std::string format() const;
};

} // namespace silentfilm::offline
