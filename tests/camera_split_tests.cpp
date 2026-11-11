#include "silentfilm/camera_capture.hpp"
#include "silentfilm/sensitometry.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace silentfilm;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

ImageView view(std::vector<Pixel>& pixels, int width, int height)
{
    return {reinterpret_cast<unsigned char*>(pixels.data()),
        static_cast<std::ptrdiff_t>(width) * static_cast<std::ptrdiff_t>(sizeof(Pixel)), 0, 0, width, height};
}

/** Verify that capture-only RGB output bakes linked exposure without changing alpha or color channels. */
void splitPipeline()
{
    constexpr int width = 37, height = 23;
    const RenderWindow window{0, 0, width, height};
    std::vector<Pixel> source(width * height, Pixel{0.5f, 0.25f, 0.75f, 0.6f});
    std::vector<Pixel> exposed(source.size()), brighter(source.size());
    const CameraExposureSample sample{view(source, width, height), 0, {}, 0, 1};
    CameraOpticsParameters optics;
    optics.captureOnly = true;
    require(renderCameraCaptureCpu(&sample, 1, view(exposed, width, height), window,
        optics, 0.5f, 0, 1925, nullptr, nullptr), "Low exposure capture failed");
    require(renderCameraCaptureCpu(&sample, 1, view(brighter, width, height), window,
        optics, 2.0f, 0, 1925, nullptr, nullptr), "High exposure capture failed");
    const auto expected = [](float value, float gain) {
        return film::encode(std::clamp(film::decode(value, Transfer::SRGB) * gain, 0.0f, 1.0f), Transfer::SRGB);
    };
    require(std::abs(exposed[100].r - expected(0.5f, 0.5f)) < 1e-6f
        && std::abs(exposed[100].g - expected(0.25f, 0.5f)) < 1e-6f
        && std::abs(brighter[100].r - expected(0.5f, 2.0f)) < 1e-6f,
        "Cadence exposure gain was not baked into the RGB capture");
    require(exposed[100].r < brighter[100].r && exposed[100].a == 0.6f,
        "Exposure link did not change RGB brightness or preserve alpha");
}

/** Verify transparent output outside capture bounds and untouched pixels outside the render window. */
void captureBounds()
{
    const Pixel color{0.2f, 0.4f, 0.8f, 0.5f}, sentinel{-7, -7, -7, -7};
    std::vector<Pixel> source(4, color), output(36, sentinel);
    auto destination = view(output, 6, 6);
    destination.x1 = destination.y1 = -2;
    destination.x2 = destination.y2 = 4;
    const CameraExposureSample sample{view(source, 2, 2), 0, {}, 0, 1};
    CameraOpticsParameters optics;
    optics.captureOnly = true;
    optics.vignetteStrength = 0.99f;
    const RenderWindow window{-1, -1, 3, 3};
    require(renderCameraCaptureCpu(&sample, 1, destination, window, optics, 1, 0, 1925, nullptr, nullptr),
        "RGB bounds capture failed");
    for (int y = -2; y < 4; ++y) {
        for (int x = -2; x < 4; ++x) {
            const bool inWindow = x >= -1 && x < 3 && y >= -1 && y < 3;
            const bool inCapture = x >= 0 && x < 2 && y >= 0 && y < 2;
            const Pixel expected = !inWindow ? sentinel : inCapture ? color : Pixel{};
            const auto actual = output[static_cast<std::size_t>(y + 2) * 6 + (x + 2)];
            require(std::abs(actual.r - expected.r) < 1e-6f && std::abs(actual.g - expected.g) < 1e-6f
                && std::abs(actual.b - expected.b) < 1e-6f && std::abs(actual.a - expected.a) < 1e-6f,
                "RGB capture extended edge pixels or wrote outside the render window");
        }
    }
}

} // namespace

int main()
{
    try { splitPipeline(); captureBounds(); std::cout << "Cadence exposure: PASS\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
