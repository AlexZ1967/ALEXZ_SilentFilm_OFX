#include "flow_sampler.hpp"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <opencv2/core/version.hpp>

namespace {
using namespace silentfilm;
using namespace silentfilm::offline;
constexpr int width = 128, height = 96;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

Pixel scene(float x, float y) {
    const float value = 0.5f + 0.10f * std::sin(0.61f * x + 0.22f * y)
        + 0.12f * std::cos(0.25f * x - 0.31f * y) + 0.08f * std::sin(0.11f * x + 0.57f * y);
    return {value, 0.1f + value * 0.6f, 0.2f + value * 0.5f, 0.6f};
}

std::shared_ptr<const SourceFrame> frame(std::int64_t index, float shift) {
    auto result = std::make_shared<SourceFrame>(); result->index = index;
    result->pixels.resize(width * height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        result->pixels[static_cast<std::size_t>(y) * width + x] = scene(x - shift, y);
    return result;
}

/** Check flow direction against a known translation and reject unbounded/history-dependent output. */
void translatedScene() {
    auto a = frame(0, 0), b = frame(1, 3), c = frame(2, 6);
    FlowSampler sampler(width, height, 1, {}); // Один pair в кэше, чтобы проверить пересчёт после вытеснения.
    require(sampler.sample(a, b, 0) == a && sampler.sample(a, b, 1) == b, "Flow changed exact source endpoints");
    const auto middle = sampler.sample(a, b, 0.5f);
    double flowError = 0, simpleError = 0;
    for (int y = 16; y < height - 16; ++y) for (int x = 16; x < width - 16; ++x) {
        const auto i = static_cast<std::size_t>(y) * width + x;
        const auto truth = scene(x - 1.5f, y);
        flowError += std::abs(middle->pixels[i].r - truth.r);
        simpleError += std::abs((a->pixels[i].r + b->pixels[i].r) * 0.5f - truth.r);
    }
    require(flowError < simpleError * 0.9, "Flow did not improve the known translated scene over frame mixing");
    for (const auto& pixel : middle->pixels)
        require(std::isfinite(pixel.r) && pixel.r >= 0 && pixel.r <= 1 && std::abs(pixel.a - 0.6f) < 1e-6f,
            "Flow produced invalid RGB or changed constant alpha");
    (void)sampler.sample(b, c, 0.5f);
    const auto repeated = sampler.sample(a, b, 0.5f);
    for (std::size_t i = 0; i < middle->pixels.size(); ++i)
        require(std::abs(repeated->pixels[i].r - middle->pixels[i].r) < 2e-5f, "Flow cache eviction changed output");
    const auto still = frame(3, 0);
    for (float mix : {0.2f, 0.5f, 0.8f}) {
        const auto unchanged = sampler.sample(a, still, mix);
        for (std::size_t i = 0; i < a->pixels.size(); ++i)
            require(unchanged->pixels[i].r == a->pixels[i].r
                && unchanged->pixels[i].g == a->pixels[i].g && unchanged->pixels[i].b == a->pixels[i].b
                && unchanged->pixels[i].a == a->pixels[i].a,
                "Flow displaced a stationary scene");
    }
    auto changedAlpha = std::make_shared<SourceFrame>(*a);
    changedAlpha->index = 4;
    for (auto& pixel : changedAlpha->pixels) pixel.a = 0.2f;
    const auto alphaBlend = sampler.sample(a, changedAlpha, 0.25f);
    for (std::size_t i = 0; i < a->pixels.size(); ++i)
        require(alphaBlend->pixels[i].r == a->pixels[i].r && alphaBlend->pixels[i].g == a->pixels[i].g
            && alphaBlend->pixels[i].b == a->pixels[i].b && std::abs(alphaBlend->pixels[i].a - 0.5f) < 1e-6f,
            "Stationary RGB shortcut lost alpha interpolation");
}

/** Compare native interpolation with fixtures computed directly by the Comfy reference functions. */
void compareComfy(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    const auto read = [&](void* data, std::size_t bytes) {
        if (!input.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes)))
            throw std::runtime_error("Incomplete Comfy flow fixture");
    };
    char magic[4]; read(magic, sizeof(magic));
    require(std::string(magic, sizeof(magic)) == "SFF1", "Unknown Comfy flow fixture");
    // Диагностический формат little-endian float32; базовая платформа проекта Linux x86_64.
    std::uint32_t dimensions[3]; read(dimensions, sizeof(dimensions));
    const auto w = dimensions[0], h = dimensions[1], samples = dimensions[2];
    require(w > 0 && w <= 4096 && h > 0 && h <= 4096 && samples > 0 && samples <= 32,
        "Invalid Comfy fixture dimensions");
    const auto count = static_cast<std::size_t>(w) * h;
    auto a = std::make_shared<SourceFrame>(), b = std::make_shared<SourceFrame>();
    a->index = 0; b->index = 1; a->pixels.resize(count); b->pixels.resize(count);
    static_assert(sizeof(Pixel) == 4 * sizeof(float), "Fixture requires packed RGBA32F");
    read(a->pixels.data(), count * sizeof(Pixel)); read(b->pixels.data(), count * sizeof(Pixel));
    FlowSampler sampler(static_cast<int>(w), static_cast<int>(h), 64 * 1024 * 1024, {});
    std::vector<Pixel> expected(count);
    std::cout << "Native OpenCV " << CV_VERSION << ", " << w << 'x' << h << '\n';
    bool matched = true;
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
        float mix; read(&mix, sizeof(mix)); read(expected.data(), count * sizeof(Pixel));
        const auto actual = sampler.sample(a, b, mix);
        double totalError = 0; float maxError = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& p = actual->pixels[i]; const auto& q = expected[i];
            const float errors[] = {std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b), std::abs(p.a - q.a)};
            for (const float error : errors) {
                require(std::isfinite(error), "Non-finite Comfy comparison output");
                totalError += error; maxError = std::max(maxError, error);
            }
        }
        const auto meanError = totalError / (4 * count);
        std::cout << "mix=" << mix << " max_error=" << maxError << " mean_error=" << meanError << '\n';
        matched = matched && maxError <= 1e-3f && meanError <= 1e-5;
    }
    require(matched, "Native flow differs from Comfy fixture; compare input frames and OpenCV versions");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--compare-comfy") compareComfy(argv[2]);
        else if (argc == 1) translatedScene();
        else throw std::runtime_error("Usage: silentfilm_cadence_flow_tests [--compare-comfy fixture]");
        std::cout << "Cadence optical flow: PASS\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
