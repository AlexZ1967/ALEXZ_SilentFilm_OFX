#include "silentfilm/camera_capture.hpp"

#ifdef SILENTFILM_TEST_CUDA
#include <cuda_runtime_api.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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

/** Render the actual current-frame path; GPU comparisons do not go through CPU capture integration. */
std::vector<Pixel> render(std::vector<Pixel>& source, int width, int height, const CameraOpticsParameters& parameters)
{
    std::vector<Pixel> output(source.size());
    const RenderWindow window{0, 0, width, height};
#ifdef SILENTFILM_TEST_CUDA
    struct Buffers {
        Pixel* input = nullptr;
        Pixel* output = nullptr;
        ~Buffers() { if (input) cudaFree(input); if (output) cudaFree(output); }
    } buffers;
    const auto bytes = source.size() * sizeof(Pixel);
    require(cudaMalloc(reinterpret_cast<void**>(&buffers.input), bytes) == cudaSuccess, "CUDA input allocation failed");
    require(cudaMalloc(reinterpret_cast<void**>(&buffers.output), bytes) == cudaSuccess, "CUDA output allocation failed");
    require(cudaMemcpy(buffers.input, source.data(), bytes, cudaMemcpyHostToDevice) == cudaSuccess, "CUDA upload failed");
    auto sourceView = view(source, width, height), outputView = view(output, width, height);
    sourceView.data = reinterpret_cast<unsigned char*>(buffers.input);
    outputView.data = reinterpret_cast<unsigned char*>(buffers.output);
    const CameraExposureSample sample{sourceView, 0, {}, 0, 1};
    const CameraSampleTransform transform{};
    const auto status = renderCudaCamera(&sample, &transform, 1, outputView, window, parameters, 1, false, nullptr, false);
    if (status.code) throw std::runtime_error(status.message ? status.message : "CUDA optics failed");
    require(cudaMemcpy(output.data(), buffers.output, bytes, cudaMemcpyDeviceToHost) == cudaSuccess, "CUDA download failed");
#else
    require(renderCameraOpticsCpu(view(source, width, height), view(output, width, height), window,
        parameters, nullptr, nullptr), "CPU optics failed");
#endif
    return output;
}

void compare(const std::vector<Pixel>& actual, const std::vector<Pixel>& expected, float tolerance)
{
    double sum = 0;
    float maximum = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const std::array<float, 4> a{actual[i].r, actual[i].g, actual[i].b, actual[i].a};
        const std::array<float, 4> b{expected[i].r, expected[i].g, expected[i].b, expected[i].a};
        for (int channel = 0; channel < 4; ++channel) {
            require(std::isfinite(a[channel]), "Nonfinite optics pixel");
            const float error = std::abs(a[channel] - b[channel]);
            sum += error;
            maximum = std::max(maximum, error);
        }
    }
    const double mean = sum / (actual.size() * 4);
    std::cout << "max_error=" << maximum << " mean_error=" << mean << '\n';
    require(maximum <= tolerance && mean <= tolerance * 0.1, "Camera optics comparison failed");
}

/** Verify an analytic neutral/edge response and unchanged alpha with premultiplied RGB. */
void analyticResponse()
{
    constexpr int width = 17, height = 9;
    std::vector<Pixel> source(width * height), expected(source.size());
    for (bool premultiplied : {false, true}) {
        for (float strength : {0.0f, 0.6f}) {
            CameraOpticsParameters p;
            p.sourcePremultiplied = p.outputPremultiplied = premultiplied;
            p.vignetteStrength = strength;
            const float gray = 0.8f * 0.299f + 0.4f * 0.587f + 0.2f * 0.114f;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto i = static_cast<std::size_t>(y) * width + x;
                    const float alpha = static_cast<float>(i % 5) / 4;
                    const float multiplier = premultiplied ? alpha : 1;
                    source[i] = {0.8f * multiplier, 0.4f * multiplier, 0.2f * multiplier, alpha};
                    const float dx = (x - 8.0f) / 8.0f, dy = (y - 4.0f) / 8.0f;
                    const float t = std::clamp((std::sqrt(dx * dx + dy * dy) - 0.6f) / 0.4f, 0.0f, 1.0f);
                    const float value = gray * (1 - strength * t * t * (3 - 2 * t)) * multiplier;
                    expected[i] = {value, value, value, alpha};
                }
            }
            compare(render(source, width, height, p), expected, 3e-6f);
        }
    }
}

/** Blurring a flat field must not darken borders; SDR clamping precedes optics and monochrome. */
void flatField()
{
    constexpr int width = 160, height = 54;
    std::vector<Pixel> source(width * height, {-0.2f, 1.4f, 0.5f, 0.7f});
    const float gray = 0.587f + 0.5f * 0.114f;
    std::vector<Pixel> expected(source.size(), {gray, gray, gray, 0.7f});
    CameraOpticsParameters p;
    p.edgeSoftness = 1; p.softness = 1; p.focusDrift = 0.5f; p.focusSignal = 1;
    compare(render(source, width, height, p), expected, 8e-5f);
}

/** Check unit population variance before tanh and render-order/cache independence. */
void focusNormalization()
{
    CameraFocusCache cache, independent;
    constexpr std::size_t count = 173;
    std::vector<float> values(count);
    double sum = 0, square = 0;
    for (std::size_t i = count; i-- > 0;) {
        values[i] = cache.at(i, count, 1925);
        require(values[i] == independent.at(i, count, 1925), "Focus depends on request history");
        const double raw = std::atanh(static_cast<double>(values[i]));
        sum += raw; square += raw * raw;
    }
    require(std::abs(sum / count) < 2e-6 && std::abs(square / count - 1) < 2e-6,
        "Focus is not clip-normalized like Comfy");
    require(cache.at(12, count, 1926) != values[12], "Focus seed has no effect");
    require(cache.at(12, count, 1925) == values[12], "Focus seed round trip changed the signal");
    cache.clear(); cache.clear();
    require(cache.at(12, count, 1925) == values[12], "Purging focus signals changed the reconstructed frame");
    require(cache.at(0, 1, 1925) == 0 && cache.at(0, 0, 1925) == 0, "Single-frame focus must be neutral");
    require(optics::blurDimension(54, 0.75) == 40 && optics::blurDimension(50, 0.75) == 38,
        "Gaussian reduction must use Python nearest-even rounding");
}

/** Exercise broad reduction, narrow frames, both focus extremes and the standalone CPU/GPU paths. */
void standaloneParity()
{
    const std::array<std::array<int, 2>, 4> sizes{{{160, 54}, {257, 43}, {73, 2}, {1, 1}}};
    for (const auto size : sizes) {
        const int width = size[0], height = size[1];
        std::vector<Pixel> source(static_cast<std::size_t>(width) * height), expected(source.size());
        for (std::size_t i = 0; i < source.size(); ++i) {
            const float value = static_cast<float>((i * 13) % 53) / 52;
            source[i] = {value, 1 - value, 0.3f + value * 0.2f, static_cast<float>(i % 5) / 4};
        }
        for (float focus : {-1.0f, 0.3f, 1.0f}) {
            CameraOpticsParameters p;
            p.edgeSoftness = 1; p.vignetteStrength = 0.2f; p.softness = 0.7f;
            p.focusDrift = 0.5f; p.focusSignal = focus;
            require(renderCameraCpu(view(source, width, height), view(expected, width, height),
                {0, 0, width, height}, p, 1, nullptr, nullptr), "RGB-plane CPU reference failed");
            compare(render(source, width, height, p), expected, 8e-5f);
        }
    }
}

/** Preserve negative strides, offset bounds, alpha and pixels outside the requested window. */
void cpuBounds()
{
    const Pixel sentinel{-7, -7, -7, -7}, color{0.8f, 0.4f, 0.2f, 0.5f};
    constexpr int sw = 5, sh = 3, sourcePitch = 7, ow = 9, oh = 7, outputPitch = 11;
    std::vector<Pixel> source(sourcePitch * sh, color), output(outputPitch * oh, sentinel);
    const ImageView input{reinterpret_cast<unsigned char*>(source.data() + sourcePitch * (sh - 1)),
        -static_cast<std::ptrdiff_t>(sourcePitch * sizeof(Pixel)), -2, 4, -2 + sw, 4 + sh};
    const ImageView destination{reinterpret_cast<unsigned char*>(output.data() + outputPitch * (oh - 1)),
        -static_cast<std::ptrdiff_t>(outputPitch * sizeof(Pixel)), -4, 2, -4 + ow, 2 + oh};
    const RenderWindow window{-3, 3, 4, 8};
    CameraOpticsParameters p;
    p.outputPremultiplied = true;
    require(renderCameraOpticsCpu(input, destination, window, p, nullptr, nullptr), "Offset CPU optics failed");
    const float gray = (color.r * 0.299f + color.g * 0.587f + color.b * 0.114f) * color.a;
    for (int row = 0; row < oh; ++row) {
        const int y = 2 + oh - 1 - row;
        for (int column = 0; column < outputPitch; ++column) {
            const int x = -4 + column;
            const bool inside = column < ow && x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2;
            const bool captured = x >= input.x1 && x < input.x2 && y >= input.y1 && y < input.y2;
            const auto expected = !inside ? sentinel : captured ? Pixel{gray, gray, gray, color.a} : Pixel{};
            const auto actual = output[static_cast<std::size_t>(row) * outputPitch + column];
            require(std::abs(actual.r - expected.r) < 1e-6f && actual.r == actual.g && actual.g == actual.b
                && actual.a == expected.a, "Optics modified padding, alpha or pixels outside the window");
        }
    }
    require(!renderCameraOpticsCpu(input, destination, window, p, [](void*) { return true; }, nullptr),
        "Optics ignored cancellation");
}

/** Read independent Comfy fixtures, including all RGB optics before the final monochrome conversion. */
void referenceFixture(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    const auto read = [&](void* data, std::size_t bytes) {
        input.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes));
        require(static_cast<bool>(input), "Truncated optics fixture");
    };
    std::array<char, 4> magic{};
    std::array<std::uint32_t, 2> size{};
    std::array<float, 7> settings{};
    read(magic.data(), magic.size()); read(size.data(), sizeof(size)); read(settings.data(), sizeof(settings));
    require(magic == std::array<char, 4>{'S', 'F', 'O', '1'} && size[0] > 0 && size[1] > 0
        && size[0] <= 4096 && size[1] <= 4096, "Invalid optics fixture");
    CameraOpticsParameters p;
    p.vignetteStrength = settings[0]; p.edgeSoftness = settings[1];
    p.edgeStart = settings[2]; p.edgeEnd = settings[3]; p.softness = settings[4];
    p.focusDrift = settings[5]; p.focusSignal = settings[6];
    std::vector<Pixel> source(static_cast<std::size_t>(size[0]) * size[1]), expected(source.size());
    read(source.data(), source.size() * sizeof(Pixel)); read(expected.data(), expected.size() * sizeof(Pixel));
    compare(render(source, static_cast<int>(size[0]), static_cast<int>(size[1]), p), expected, 8e-5f);
}
} // namespace

int main(int argc, char** argv)
{
    try {
#ifdef SILENTFILM_TEST_CUDA
        int count = 0;
        const auto status = cudaGetDeviceCount(&count);
        if (status == cudaErrorNoDevice || (status == cudaSuccess && count == 0)) return 77;
        require(status == cudaSuccess, "Cannot query CUDA device");
#endif
        if (argc == 3 && std::string(argv[1]) == "--compare-comfy") referenceFixture(argv[2]);
        else { analyticResponse(); flatField(); focusNormalization(); standaloneParity(); cpuBounds(); }
        std::cout << "Camera Optics: PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
