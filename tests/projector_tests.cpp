#include "silentfilm/film_optics.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void near(float a, float b) { require(std::isfinite(a) && std::abs(a - b) < 2e-5f, "Unexpected Projector pixel"); }

void signals()
{
    silentfilm::ProjectorSignalCache cache, other;
    constexpr std::size_t count = 512;
    std::vector<silentfilm::ProjectorFrameState> expected;
    double sumX = 0, squareX = 0, sumLamp = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto p = cache.at(i, count, 1925, 1, 0.025f, 0.01f);
        expected.push_back(p); sumX += p.shiftX; squareX += p.shiftX * p.shiftX; sumLamp += p.lampGain;
    }
    require(std::abs(sumX / count) < 1e-6 && std::abs(squareX / count - 1) < 1e-6,
        "Gate weave is not centered and normalized over the clip");
    require(std::abs(sumLamp / count - 1) < 1e-6, "Lamp changes the average projection exposure");
    for (std::size_t i = count; i-- > 0;) {
        const auto p = cache.at(i, count, 1925, 1, 0.025f, 0.01f);
        const auto q = other.at(i, count, 1925, 1, 0.025f, 0.01f);
        require(p.shiftX == expected[i].shiftX && p.shiftY == q.shiftY && p.lampGain == q.lampGain,
            "Projector depends on request order or instance history");
        const auto lampOnly = cache.at(i, count, 1925, 0, 0.025f, 0.01f);
        require(lampOnly.lampGain == p.lampGain && lampOnly.shiftX == 0 && lampOnly.shiftY == 0,
            "Weave controls affect independent lamp signals");
    }
    const auto disabled = cache.at(100, count, 1925, 0, 0, 0);
    require(disabled.lampGain == 1 && disabled.shiftX == 0, "Disabled projector signals are not neutral");
    const auto single = cache.at(0, 1, 1925, 1, 0.025f, 0.01f);
    require(single.lampGain == 1 && single.shiftX == 0, "Single-frame clip is not neutral after normalization");
    const auto a = cache.at(7, count, 1925, 1, 0.025f, 0.01f);
    const auto b = cache.at(7, count, 1926, 1, 0.025f, 0.01f);
    require(a.shiftX != b.shiftX && a.lampGain != b.lampGain, "Projector seed does not change the independent signals");
}

void pixels()
{
    constexpr int width = 7, height = 5;
    std::vector<silentfilm::Pixel> source(width * height, {0.2f, 0.3f, 0.4f, 0.5f}), output(source.size());
    const auto stride = static_cast<std::ptrdiff_t>(width) * static_cast<std::ptrdiff_t>(sizeof(silentfilm::Pixel));
    const silentfilm::ImageView input{reinterpret_cast<unsigned char*>(source.data()), stride, -2, 3, -2 + width, 3 + height};
    const silentfilm::ImageView destination{reinterpret_cast<unsigned char*>(output.data()), stride, -2, 3, -2 + width, 3 + height};
    const silentfilm::RenderWindow window{input.x1, input.y1, input.x2, input.y2};
    silentfilm::ProjectorParameters p;
    p.transfer = silentfilm::Transfer::Linear; p.frame.lampGain = 1.5f;
    for (float glow : {0.0f, 0.4f, 1.0f}) {
        p.glowStrength = glow;
        require(silentfilm::renderProjectorCpu(input, destination, window, p, nullptr, nullptr), "Projector CPU failed");
        for (auto value : output) { near(value.r, 0.3f); near(value.g, 0.45f); near(value.b, 0.6f); near(value.a, 0.5f); }
    }
    p.glowStrength = 0; p.frame = {0.5f, 0, 1};
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        source[y * width + x] = {x / 10.0f, y / 10.0f, 0.2f, x / 10.0f};
    require(silentfilm::renderProjectorCpu(input, destination, window, p, nullptr, nullptr), "Projector weave failed");
    near(output[0].r, 0.05f); near(output[0].a, 0.05f);
    near(output[width - 1].r, 0.6f); near(output[width - 1].a, 0.6f);
    p.frame = {0, 0, 1};
    require(!silentfilm::renderProjectorCpu(input, destination, window, p, [](void*) { return true; }, nullptr),
        "Projector ignored render cancellation");
}

} // namespace

int main()
{
    try { signals(); pixels(); std::cout << "Projector core: PASS\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
