#include "silentfilm/film_optics.hpp"
#include "silentfilm/grain.hpp"
#include "silentfilm/workflow_presets.hpp"
#include "silentfilm/color_contract.hpp"

#ifdef SILENTFILM_TEST_CUDA
#include <cuda_runtime_api.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace silentfilm;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

ImageView view(std::vector<Pixel>& pixels, int width, int height)
{
    return {reinterpret_cast<unsigned char*>(pixels.data()), static_cast<std::ptrdiff_t>(width * sizeof(Pixel)), 0, 0, width, height};
}

void compare(const float* actual, const float* expected, std::size_t count, float limit = 1e-4f)
{
    double sum = 0; float maximum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        require(std::isfinite(actual[i]), "Nonfinite film output");
        const float error = std::abs(actual[i] - expected[i]);
        maximum = std::max(maximum, error); sum += error;
    }
    std::cout << "max_error=" << maximum << " mean_error=" << sum / count << '\n';
    require(maximum <= limit && sum / count <= limit * 0.1, "Film reference mismatch");
}

/** Render a full emulsion or projector through the production CPU/CUDA implementation. */
std::vector<Pixel> render(std::vector<Pixel>& input, int width, int height, EmulsionParameters emulsion,
    const ProjectorParameters* projector = nullptr)
{
    std::vector<Pixel> output(input.size());
    const RenderWindow window{0, 0, width, height};
#ifdef SILENTFILM_TEST_CUDA
    struct Storage {
        Pixel* input = nullptr; Pixel* output = nullptr; cudaStream_t stream = nullptr;
        ~Storage() { if (stream) cudaStreamSynchronize(stream); if (input) cudaFree(input); if (output) cudaFree(output); if (stream) cudaStreamDestroy(stream); }
    } device;
    require(cudaStreamCreate(&device.stream) == cudaSuccess, "CUDA stream failed");
    const auto bytes = input.size() * sizeof(Pixel);
    require(cudaMalloc(reinterpret_cast<void**>(&device.input), bytes) == cudaSuccess
        && cudaMalloc(reinterpret_cast<void**>(&device.output), bytes) == cudaSuccess, "CUDA film allocation failed");
    require(cudaMemcpyAsync(device.input, input.data(), bytes, cudaMemcpyHostToDevice, device.stream) == cudaSuccess, "CUDA upload failed");
    auto source = view(input, width, height), target = view(output, width, height);
    source.data = reinterpret_cast<unsigned char*>(device.input); target.data = reinterpret_cast<unsigned char*>(device.output);
    ProjectorCudaCache cache;
    const auto status = projector ? cache.render(source, target, window, *projector, device.stream, true)
        : renderCudaFilm(source, target, window, emulsion, cache, device.stream, true);
    if (status.code) throw std::runtime_error(status.message ? status.message : "CUDA film failed");
    require(cudaMemcpyAsync(output.data(), device.output, bytes, cudaMemcpyDeviceToHost, device.stream) == cudaSuccess,
        "CUDA film download failed");
    require(cudaStreamSynchronize(device.stream) == cudaSuccess, "CUDA film synchronization failed");
#else
    require(projector ? renderProjectorCpu(view(input, width, height), view(output, width, height), window, *projector, nullptr, nullptr)
        : renderEmulsionCpu(view(input, width, height), view(output, width, height), window,
            emulsion, emulsion.halationStrength, emulsion.halationSize, nullptr, nullptr), "CPU film failed");
#endif
    return output;
}

/** Keep RGB defaults consistent and preserve raw stock data independently of UI encoding values. */
void colorContract()
{
    require(color::pipelineTransfer == Transfer::SRGB, "The Comfy RGB boundary contract is not sRGB");
    for (const auto requested : {Transfer::Linear, Transfer::SRGB, Transfer::Gamma24}) {
        for (const auto material : {DefectMaterial::Negative, DefectMaterial::Printing})
            require(color::defectTransfer(material, requested) == Transfer::Linear,
                "Film transmission must not inherit a display transfer");
        for (const auto material : {DefectMaterial::Capture, DefectMaterial::Positive, DefectMaterial::Projector})
            require(color::defectTransfer(material, requested) == requested, "Standalone image transfer override was discarded");
    }
    for (const auto stage : {ReferenceStage::Negative, ReferenceStage::Print, ReferenceStage::Projector, ReferenceStage::Defects}) {
        const auto profiles = referencePresets(stage);
        const auto& colorOnly = profiles.back();
        require(colorOnly.values.size() == 1 && std::string(colorOnly.values[0].id) == "encoding"
            && colorOnly.values[0].value == static_cast<int>(color::pipelineTransfer),
            "Color-only migration must not reset the film look or defect settings");
    }
    EmulsionParameters negative, print;
    negative.transfer = print.transfer = color::pipelineTransfer; print.stage = FilmStage::Print;
    for (float value : {0.02f, 0.18f, 0.5f, 1.0f}) {
        const auto stock = emulsionPixel({value, value, value, 1}, negative);
        const auto positive = emulsionPixel(stock, print);
        require(std::abs(positive.r - value) < 5e-5f, "The agreed sRGB negative/print contract is not neutral");
        auto wrong = print; wrong.transfer = Transfer::Linear;
        if (value == 0.5f)
            require(std::abs(emulsionPixel(stock, wrong).r - value) > 0.1f,
                "Regression fixture failed to reproduce the sRGB/linear mismatch");
    }
}

void grainProperties()
{
    EmulsionParameters p; p.grainSize = 2; p.grainSeed = 1925; p.grainFrame = 7;
    const auto lattices = emulsionGrainLattices(257, 129, p);
    require(lattices[0].width == 257 && lattices[0].height == 129,
        "Comfy clamps effective grain lattice spacing to at least one pixel");
    std::vector<float> field, repeat, other;
    require(emulsionGrainField(lattices, 257, 129, field), "Grain field failed");
    require(emulsionGrainField(lattices, 257, 129, repeat), "Repeated grain failed");
    require(field == repeat, "Grain depends on render order");
    double sum = 0, square = 0;
    for (float value : field) { require(std::isfinite(value) && std::abs(value) <= 1, "Grain outside tanh bounds"); sum += value; square += value * value; }
    require(std::abs(sum / field.size()) < 0.1 && square / field.size() > 0.15, "Grain contrast or centering is wrong");
    ++p.grainFrame;
    require(emulsionGrainField(emulsionGrainLattices(257, 129, p), 257, 129, other), "Next grain failed");
    require(other != field, "Grain frame has no effect");
    require(emulsionGrainField(emulsionGrainLattices(1, 1, p), 1, 1, other) && other[0] == 0,
        "Degenerate grain must normalize to zero");
}

/** Verify stock grain in density, including inherited negative grain with zero print-stock grain. */
void densityGrain()
{
    constexpr int width = 257, height = 43;
    std::vector<Pixel> source(width * height, {0.18f, 0.18f, 0.18f, 0.7f});
    for (const auto stage : {FilmStage::Negative, FilmStage::Print}) {
        EmulsionParameters p; p.stage = stage; p.transfer = Transfer::SRGB;
        p.grainStrength = 0.08f; p.grainSize = 16; p.grainSeed = stage == FilmStage::Negative ? 1925 : 2022;
        std::vector<float> field;
        require(emulsionGrainField(emulsionGrainLattices(width, height, p), width, height, field), "Density grain field failed");
        const float base = stage == FilmStage::Negative ? film::negativeDensity(film::decode(0.18f, p.transfer), p)
            : film::printDensity(0.18f, p);
        std::vector<Pixel> expected(source.size());
        for (std::size_t i = 0; i < field.size(); ++i) {
            const float density = base + field[i] * 0.08f;
            const float value = stage == FilmStage::Negative ? film::transmittance(density) : film::viewPositive(density, p.transfer);
            expected[i] = {value, value, value, 0.7f};
        }
        const auto output = render(source, width, height, p);
        compare(reinterpret_cast<const float*>(output.data()), reinterpret_cast<const float*>(expected.data()), output.size() * 4);
    }
    EmulsionParameters negative; negative.transfer = Transfer::SRGB; negative.grainStrength = 0.01f;
    auto transmission = render(source, width, height, negative);
    EmulsionParameters print; print.stage = FilmStage::Print; print.transfer = Transfer::SRGB;
    auto positive = render(transmission, width, height, print);
    require(positive[0].r != positive[width * height / 2].r, "Zero print grain incorrectly erased inherited negative grain");
}

/** Cover CPU/CUDA spatial emulsion with halation, independent stocks and alpha representations. */
void parity()
{
    constexpr int width = 61, height = 29;
    std::vector<Pixel> source(width * height), expected(source.size());
    for (const auto stage : {FilmStage::Negative, FilmStage::Print}) {
        for (const bool premultiplied : {false, true}) {
            for (const float halo : {0.0f, 0.5f}) {
                EmulsionParameters p; p.stage = stage; p.transfer = Transfer::SRGB;
                p.grainStrength = 0.06f; p.grainSize = 10; p.grainSeed = 1925; p.grainFrame = 19;
                p.halationStrength = halo; p.halationSize = 0.7f;
                p.sourcePremultiplied = p.outputPremultiplied = premultiplied;
                for (std::size_t i = 0; i < source.size(); ++i) {
                    const float alpha = static_cast<float>(i % 5) / 4, factor = premultiplied ? alpha : 1;
                    const float value = 0.04f + static_cast<float>((i * 17) % 57) / 80;
                    source[i] = {value * factor, value * 0.8f * factor, 0.2f * factor, alpha};
                }
                require(renderEmulsionCpu(view(source, width, height), view(expected, width, height), {0, 0, width, height},
                    p, halo, p.halationSize, nullptr, nullptr), "CPU spatial reference failed");
                const auto output = render(source, width, height, p);
                compare(reinterpret_cast<const float*>(output.data()), reinterpret_cast<const float*>(expected.data()), output.size() * 4, 2e-4f);
            }
        }
    }
}

void fixture(const char* path, bool grain)
{
    std::ifstream input(path, std::ios::binary);
    const auto read = [&](void* data, std::size_t bytes) { input.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes)); require(static_cast<bool>(input), "Truncated reference fixture"); };
    std::array<char, 4> magic{}; std::array<std::uint32_t, 2> size{};
    read(magic.data(), 4); read(size.data(), sizeof(size));
    require(size[0] > 0 && size[1] > 0 && size[0] <= 4096 && size[1] <= 4096, "Invalid fixture dimensions");
    const auto count = static_cast<std::size_t>(size[0]) * size[1];
    if (grain) {
        require(magic == std::array<char, 4>{'S', 'F', 'G', '1'}, "Invalid grain fixture");
        std::array<GrainLattice, 4> lattices;
        for (auto& lattice : lattices) {
            std::array<std::uint32_t, 2> dimensions{}; read(dimensions.data(), sizeof(dimensions));
            require(dimensions[0] > 0 && dimensions[1] > 0 && dimensions[0] <= size[0] && dimensions[1] <= size[1], "Invalid grain lattice");
            lattice.width = static_cast<int>(dimensions[0]); lattice.height = static_cast<int>(dimensions[1]);
            lattice.values.resize(static_cast<std::size_t>(lattice.width) * lattice.height);
            read(lattice.values.data(), lattice.values.size() * sizeof(float));
        }
        std::vector<float> expected(count), actual;
        read(expected.data(), count * sizeof(float));
        require(emulsionGrainField(lattices, static_cast<int>(size[0]), static_cast<int>(size[1]), actual), "Reference grain failed");
        compare(actual.data(), expected.data(), count);
    } else {
        require(magic == std::array<char, 4>{'S', 'F', 'P', '1'}, "Invalid projector fixture");
        std::array<float, 4> values{}; read(values.data(), sizeof(values));
        ProjectorParameters p; p.frame = {values[0], values[1], values[2]}; p.glowStrength = values[3];
        std::vector<Pixel> source(count), expected(count);
        read(source.data(), count * sizeof(Pixel)); read(expected.data(), count * sizeof(Pixel));
        const auto actual = render(source, static_cast<int>(size[0]), static_cast<int>(size[1]), {}, &p);
        compare(reinterpret_cast<const float*>(actual.data()), reinterpret_cast<const float*>(expected.data()), count * 4, 2e-4f);
    }
}

void presets()
{
    std::cout << '{'; bool firstStage = true;
    const std::array<std::pair<const char*, ReferenceStage>, 5> stages{{{"camera", ReferenceStage::CameraOptics},
        {"negative", ReferenceStage::Negative}, {"print", ReferenceStage::Print}, {"projector", ReferenceStage::Projector}, {"defects", ReferenceStage::Defects}}};
    for (const auto& pair : stages) {
        if (!firstStage) std::cout << ',';
        firstStage = false; std::cout << '"' << pair.first << "\":[";
        bool firstPreset = true;
        for (const auto& preset : referencePresets(pair.second)) {
            if (!firstPreset) std::cout << ',';
            firstPreset = false; std::cout << "{\"node\":" << preset.node << ",\"values\":{";
            bool first = true;
            for (const auto& value : preset.values) {
                if (!first) std::cout << ',';
                first = false; std::cout << '"' << value.id << "\":" << value.value;
            }
            std::cout << "}}";
        }
        std::cout << ']';
    }
    std::cout << "}\n";
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string(argv[1]) == "--dump-presets") { presets(); return 0; }
#ifdef SILENTFILM_TEST_CUDA
        int count = 0; const auto status = cudaGetDeviceCount(&count);
        if (status == cudaErrorNoDevice || (status == cudaSuccess && count == 0)) return 77;
        require(status == cudaSuccess, "Cannot query CUDA device");
#endif
        if (argc == 3) fixture(argv[2], std::string(argv[1]) == "--compare-grain");
        else { colorContract(); grainProperties(); densityGrain(); parity(); }
        std::cout << "Film reference: PASS\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
