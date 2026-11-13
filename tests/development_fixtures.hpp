#pragma once

#include "silentfilm/defects_pixel.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef SILENTFILM_TEST_CUDA
#include <cuda_runtime_api.h>
#endif

namespace development_fixture {
/** Export raw independent knots and production outputs for the actual Comfy reference. */
inline void dump(const std::filesystem::path& folder)
{
    using namespace silentfilm;
    std::filesystem::create_directories(folder);
    std::ofstream json(folder / "cases.json");
    json << std::setprecision(17) << '[';
    const int widths[] = {1, 7, 31, 129, 9, 257, 65};
    const int heights[] = {1, 5, 19, 65, 129, 3, 33};
    const int frames[] = {0, 1, 11, 23, 24, 25, 47};
    const float sizes[] = {0.01f, 0.5f, 8, 15, 50};
    const float irregularities[] = {0, 0.6f, 1};
    const float fogs[] = {0, 0.15f, 1};
    for (int number = 0; number < 27; ++number) {
        const int width = widths[number % 7], height = heights[number % 7], frameId = frames[number % 7];
        DefectParameters p;
        p.pattern = DefectPattern::Development; p.development = static_cast<DefectDevelopment>(1 + number % 3);
        p.material = number % 2 ? DefectMaterial::Positive : DefectMaterial::Negative;
        p.mixResponse = DefectMixResponse::Physical; p.transfer = Transfer::Linear;
        p.strength = 0.37f; p.blend = 1; p.amount = 0; p.frameRate = 25; p.persistence = 24;
        p.time = frameId / p.frameRate; p.seed = 1925 + number;
        p.size = sizes[number % 5]; p.irregularity = irregularities[(number / 3) % 3];
        p.developmentFog = fogs[(number / 9) % 3];
        auto prepared = prepareDefects(p, width, height);
        const auto prefix = std::to_string(number);
        const auto write = [&](const std::string& name, const std::vector<float>& values) {
            std::ofstream file(folder / (prefix + "_" + name + ".f32"), std::ios::binary);
            file.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(float)));
            if (!file) throw std::runtime_error("Cannot write development fixture");
        };
        for (int channel = 0; channel < 2; ++channel) {
            const int count = (channel ? 2 : prepared.development.rows) * prepared.development.columns;
            for (int knot = 0; knot < 3; ++knot) {
                std::vector<float> values(static_cast<std::size_t>(count));
                for (int i = 0; i < count; ++i)
                    values[static_cast<std::size_t>(i)] = defect::developmentKnot(p.seed, channel != 0,
                        knot == 2 ? 0 : prepared.development.knot + knot, i, knot == 2);
                write(std::string(channel ? "streaks" : "mottling") + "_" + (knot == 2 ? "static" : knot == 0 ? "a" : "b"), values);
            }
        }
        std::vector<float> grid(static_cast<std::size_t>(prepared.development.count));
        for (int i = 0; i < prepared.development.count; ++i) grid[static_cast<std::size_t>(i)] = defect::developmentGridValue(prepared, i);
        prepared.development.values = grid.data();
        std::vector<float> field(static_cast<std::size_t>(width) * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                field[static_cast<std::size_t>(y * width + x)] = defect::developmentField(x + 0.5f, y + 0.5f, prepared);
        write("field", field);
        std::vector<Pixel> source(static_cast<std::size_t>(width) * height, {0.002f, 0.3f, 0.9f, 0.5f}), output(source.size());
        const auto view = [&](void* data) { return ImageView{reinterpret_cast<unsigned char*>(data),
            static_cast<std::ptrdiff_t>(width) * static_cast<std::ptrdiff_t>(sizeof(Pixel)), 0, 0, width, height}; };
#ifdef SILENTFILM_TEST_CUDA
        const auto check = [](cudaError_t status) { if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status)); };
        struct Device {
            Pixel* input = nullptr; Pixel* output = nullptr; cudaStream_t stream = nullptr;
            ~Device() { cudaStreamSynchronize(stream); if (input) cudaFree(input); if (output) cudaFree(output); if (stream) cudaStreamDestroy(stream); }
        } device;
        const bool supplied = number % 2 != 0;
        if (supplied) check(cudaStreamCreate(&device.stream));
        const auto bytes = source.size() * sizeof(Pixel);
        check(cudaMalloc(reinterpret_cast<void**>(&device.input), bytes)); check(cudaMalloc(reinterpret_cast<void**>(&device.output), bytes));
        check(cudaMemcpyAsync(device.input, source.data(), bytes, cudaMemcpyHostToDevice, device.stream));
        const auto status = renderCudaDefects(view(device.input), view(device.output), {0, 0, width, height}, p, device.stream, supplied);
        if (status.code) throw std::runtime_error(status.message ? status.message : "CUDA development failed");
        check(cudaMemcpyAsync(output.data(), device.output, bytes, cudaMemcpyDeviceToHost, device.stream)); check(cudaStreamSynchronize(device.stream));
#else
        if (!renderDefectsCpu(view(source.data()), view(output.data()), {0, 0, width, height}, p, nullptr, nullptr))
            throw std::runtime_error("CPU development failed");
#endif
        std::vector<float> pixels(output.size() * 4);
        for (std::size_t i = 0; i < output.size(); ++i) {
            pixels[i * 4] = output[i].r; pixels[i * 4 + 1] = output[i].g; pixels[i * 4 + 2] = output[i].b; pixels[i * 4 + 3] = output[i].a;
        }
        write("image", pixels);
        if (number) json << ',';
        json << "{\"number\":" << number << ",\"width\":" << width << ",\"height\":" << height
            << ",\"frame\":" << frameId << ",\"seed\":" << p.seed << ",\"size\":" << p.size
            << ",\"irregularity\":" << p.irregularity << ",\"fog\":" << p.developmentFog
            << ",\"strength\":" << p.strength << ",\"persistence\":" << p.persistence
            << ",\"knot\":" << prepared.development.knot << ",\"mode\":" << static_cast<int>(p.development) << '}';
    }
    json << ']';
    if (!json) throw std::runtime_error("Cannot write development fixture metadata");
}
} // namespace development_fixture
