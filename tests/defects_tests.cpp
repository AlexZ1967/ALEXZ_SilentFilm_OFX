#include "silentfilm/defects_pixel.hpp"
#include "defects_legacy_reference.hpp"

#ifdef SILENTFILM_TEST_CUDA
#include <cuda_runtime_api.h>
#endif

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "development_fixtures.hpp"

namespace {
using namespace silentfilm;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Image {
    int width, height, x, y;
    bool reverse;
    std::vector<Pixel> pixels;
    Image(int w, int h, int xx, int yy, bool r)
        : width(w), height(h), x(xx), y(yy), reverse(r), pixels(static_cast<std::size_t>(w + 3) * h, {-7, -7, -7, -7}) {}
    ImageView view() {
        const auto stride = static_cast<std::ptrdiff_t>(width + 3) * static_cast<std::ptrdiff_t>(sizeof(Pixel));
        auto* data = reinterpret_cast<unsigned char*>(pixels.data());
        return {reverse ? data + (height - 1) * stride : data, reverse ? -stride : stride, x, y, x + width, y + height};
    }
};

void compare(const std::vector<Pixel>& actual, const std::vector<Pixel>& expected, const DefectParameters& p)
{
    const bool negativeMix = p.mixResponse == DefectMixResponse::Perceptual
        && (p.material == DefectMaterial::Negative || p.material == DefectMaterial::Printing);
    const float tolerance = negativeMix ? 4e-5f : 2e-6f;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const std::array<float, 4> a{actual[i].r, actual[i].g, actual[i].b, actual[i].a};
        const std::array<float, 4> b{expected[i].r, expected[i].g, expected[i].b, expected[i].a};
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(a[c]) || std::abs(a[c] - b[c]) > (c == 3 ? 2e-6f : tolerance))
                throw std::runtime_error("Defects differs from the selected mix reference or buffer bounds at pixel " + std::to_string(i)
                    + " channel " + std::to_string(c) + ": " + std::to_string(a[c]) + " / " + std::to_string(b[c]));
        if (negativeMix && actual[i].a >= 0) {
            EmulsionParameters print;
            print.stage = FilmStage::Print; print.transfer = Transfer::SRGB;
            print.sourcePremultiplied = p.outputPremultiplied;
            const auto visible = emulsionPixel(actual[i], print), reference = emulsionPixel(expected[i], print);
            require(std::abs(visible.r - reference.r) <= 8e-5f, "Perceptual CPU/CUDA mismatch after actual Print");
        }
    }
}

/** Compare optimized CPU/CUDA against the frozen pre-optimization renderer, including padding/ROI. */
void run(DefectParameters p, bool suppliedStream, bool legacy = true)
{
    const bool reverse = p.sourcePremultiplied;
    Image source(37, 23, -3, 5, reverse), output(43, 29, -5, 3, !reverse), reference = output;
    auto input = source.view();
    const bool mono = p.material == DefectMaterial::Negative || p.material == DefectMaterial::Positive;
    for (int y = input.y1; y < input.y2; ++y) {
        for (int x = input.x1; x < input.x2; ++x) {
            const float level = static_cast<float>((x * 17 + y * 7 + 10000) % 53) / 52;
            const float alpha = static_cast<float>((x + y + 100) % 5) / 4;
            Pixel value{level, mono ? level : 0.2f + level * 0.7f, mono ? level : 1 - level, alpha};
            if (p.sourcePremultiplied) { value.r *= alpha; value.g *= alpha; value.b *= alpha; }
            std::memcpy(input.data + (y - input.y1) * input.rowBytes + (x - input.x1) * sizeof(Pixel), &value, sizeof(value));
        }
    }
    const RenderWindow region{-4, 4, 36, 30};
    auto referenceParameters = p;
    // Старый разброс 0.7..1.4 имеет центр 1.05; новый — симметричный относительно Size.
    // Для независимой legacy-регрессии калибруем Size, не переписывая замороженный renderer.
    if (p.pattern != DefectPattern::Development) referenceParameters.size /= 1.05f;
    const bool perceptual = p.mixResponse == DefectMixResponse::Perceptual;
    if (perceptual) referenceParameters.strength = 1;
    if (legacy)
        require(legacy_defects::render(input, reference.view(), region, referenceParameters, nullptr, nullptr), "Legacy reference failed");
    const float opacity = perceptual ? p.strength * p.blend : p.blend;
    if (legacy && opacity < 1) {
        for (int y = region.y1; y < region.y2; ++y) {
            for (int x = region.x1; x < region.x2; ++x) {
                auto clean = legacy_defects::readPixel(input, x, y);
                if (p.sourcePremultiplied != p.outputPremultiplied) {
                    if (p.outputPremultiplied) { clean.r *= clean.a; clean.g *= clean.a; clean.b *= clean.a; }
                    else if (clean.a > 0) { clean.r /= clean.a; clean.g /= clean.a; clean.b /= clean.a; }
                    else clean.r = clean.g = clean.b = 0;
                }
                const auto wet = legacy_defects::readPixel(reference.view(), x, y);
                const float dry = 1 - opacity;
                const bool negative = p.material == DefectMaterial::Negative || p.material == DefectMaterial::Printing;
                const auto mix = [&](float sourceValue, float wetValue) {
                    if (opacity == 0) return sourceValue;
                    if (!perceptual || !negative) return sourceValue * dry + wetValue * opacity;
                    const float alphaFactor = p.outputPremultiplied ? clean.a : 1;
                    if (alphaFactor == 0) return 0.0f;
                    const EmulsionParameters neutral{};
                    const float a = film::viewPositive(film::printDensity(sourceValue / alphaFactor, neutral), Transfer::SRGB);
                    const float b = film::viewPositive(film::printDensity(wetValue / alphaFactor, neutral), Transfer::SRGB);
                    if (a == b) return sourceValue * dry + wetValue * opacity;
                    return film::transmittance(film::negativeDensity(film::decode(a * dry + b * opacity, Transfer::SRGB), neutral)) * alphaFactor;
                };
                legacy_defects::writePixel(reference.view(), x, y,
                    {mix(clean.r, wet.r), mix(clean.g, wet.g), mix(clean.b, wet.b), clean.a});
            }
        }
    }
    require(renderDefectsCpu(input, output.view(), region, p, nullptr, nullptr), "CPU Defects failed");
    if (legacy) compare(output.pixels, reference.pixels, p);
    else reference.pixels = output.pixels;
#ifdef SILENTFILM_TEST_CUDA
    struct Device {
        unsigned char* input = nullptr; unsigned char* output = nullptr; cudaStream_t stream = nullptr;
        ~Device() { cudaStreamSynchronize(stream); if (input) cudaFree(input); if (output) cudaFree(output); if (stream) cudaStreamDestroy(stream); }
    } device;
    const auto check = [](cudaError_t status) { if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status)); };
    if (suppliedStream) check(cudaStreamCreate(&device.stream));
    const auto sourceBytes = source.pixels.size() * sizeof(Pixel), outputBytes = output.pixels.size() * sizeof(Pixel);
    check(cudaMalloc(reinterpret_cast<void**>(&device.input), sourceBytes));
    check(cudaMalloc(reinterpret_cast<void**>(&device.output), outputBytes));
    check(cudaMemcpyAsync(device.input, source.pixels.data(), sourceBytes, cudaMemcpyHostToDevice, device.stream));
    std::fill(output.pixels.begin(), output.pixels.end(), Pixel{-7, -7, -7, -7});
    check(cudaMemcpyAsync(device.output, output.pixels.data(), outputBytes, cudaMemcpyHostToDevice, device.stream));
    auto gpuInput = input, gpuOutput = output.view();
    gpuInput.data = device.input + (input.data - reinterpret_cast<unsigned char*>(source.pixels.data()));
    gpuOutput.data = device.output + (gpuOutput.data - reinterpret_cast<unsigned char*>(output.pixels.data()));
    const auto status = renderCudaDefects(gpuInput, gpuOutput, region, p, device.stream, suppliedStream);
    if (status.code) throw std::runtime_error(status.message ? status.message : "CUDA Defects failed");
    check(cudaMemcpyAsync(output.pixels.data(), device.output, outputBytes, cudaMemcpyDeviceToHost, device.stream));
    check(cudaStreamSynchronize(device.stream));
    compare(output.pixels, reference.pixels, p);
#else
    (void)suppliedStream;
#endif
}

void timingAndBypass()
{
    DefectParameters p;
    p.strength = 0.5f; p.amount = 4; p.persistence = 24; p.frameRate = 25;
    p.time = 0;
    const auto initial = prepareDefects(p, 1920, 1080);
    p.time = 23.0 / p.frameRate;
    require(prepareDefects(p, 1920, 1080).base == initial.base, "Persistence changed before 24 physical frames");
    p.time = 24.0 / p.frameRate;
    require(prepareDefects(p, 1920, 1080).base != initial.base, "Persistence did not change at the physical-frame boundary");
    p.persistence = 1; p.time = 17.0 / 25; p.frameRate = 25;
    const auto frame25 = prepareDefects(p, 1920, 1080);
    p.time = 17.0 / 50; p.frameRate = 50;
    require(prepareDefects(p, 1920, 1080).base == frame25.base, "The same frame depends on FPS conversion roundoff");
    p.amount = 0;
    require(defect::neutral(p), "Zero amount should bypass particle effects");
    p.pattern = DefectPattern::Development;
    require(!defect::neutral(p), "Development does not use particle amount");
    p.strength = 0;
    require(defect::neutral(p), "Zero strength should bypass every defect type");
    const Pixel original{0.1234567f, 0.42f, 0.9f, 0.37f};
    p.strength = 0.5f; p.pattern = DefectPattern::Dust; p.amount = 0;
    const auto packet = prepareDefects(p, 1920, 1080);
    const auto copied = defect::apply(original, 10, 10, 1080, packet);
    require(std::memcmp(&original, &copied, sizeof(Pixel)) == 0, "Neutral background is not copied exactly");
    p.amount = 1; p.size = 0.01f;
    const auto sparse = prepareDefects(p, 1920, 1080);
    const float x = sparse.particles[0].x < 960 ? 1919.5f : 0.5f;
    const float y = sparse.particles[0].y < 540 ? 1079.5f : 0.5f;
    const auto background = defect::apply(original, x, y, 1080, sparse);
    require(std::memcmp(&original, &background, sizeof(Pixel)) == 0, "Sparse active background went through a transfer round trip");
    p.blend = 0; p.pattern = DefectPattern::Development;
    require(defect::neutral(p), "Blend zero must bypass even active development");
    const auto dry = defect::apply(original, 10, 10, 1080, prepareDefects(p, 1920, 1080));
    require(std::memcmp(&original, &dry, sizeof(Pixel)) == 0, "Blend zero is not the exact source");
}

/** Check geometric bounds, reproducible motion and temporal count independently of the pixel renderer. */
void particleControls()
{
    DefectParameters p;
    p.amount = 24; p.size = 1; p.persistence = 10000; p.frameRate = 25;
    p.sizeSpreadPercent = p.thicknessSpreadPercent = 0;
    const auto uniform = prepareDefects(p, 1920, 1080);
    require(uniform.count == 24, "Integer Amount is not exact");
    for (int i = 0; i < uniform.count; ++i)
        require(std::abs(uniform.particles[i].radius - 9.6f) < 2e-6f, "Zero spread must give equal sizes");
    p.sizeSpreadPercent = 50;
    const auto varied = prepareDefects(p, 1920, 1080);
    bool diverse = false;
    for (int i = 0; i < varied.count; ++i) {
        require(varied.particles[i].radius >= 4.8f && varied.particles[i].radius <= 14.4f, "Size spread exceeds its bounds");
        diverse |= std::abs(varied.particles[i].radius - 9.6f) > 0.1f;
    }
    require(diverse, "Size spread has no effect");
    for (const auto pattern : {DefectPattern::Dust, DefectPattern::Scratches, DefectPattern::Stains}) {
        for (const auto seed : {1925u, 17u, 42u}) {
            auto distribution = p;
            distribution.pattern = pattern; distribution.seed = seed;
            distribution.edgeBias = 0;
            const auto unbiased = prepareDefects(distribution, 1920, 1080);
            distribution.edgeBias = 1;
            const auto biased = prepareDefects(distribution, 1920, 1080);
            require(biased.count == unbiased.count, "Edge bias changed particle count");
            for (int i = 0; i < biased.count; ++i) {
                require(biased.particles[i].x <= 1920 * 0.12f || biased.particles[i].x >= 1920 * 0.88f,
                    "Full edge bias must place particles at the left or right edge");
                require(biased.particles[i].y == unbiased.particles[i].y,
                    "Edge bias must preserve the uniform vertical distribution");
            }
        }
    }
    p.pattern = DefectPattern::Scratches;
    const auto scratches = prepareDefects(p, 1920, 1080);
    for (int i = 0; i < scratches.count; ++i)
        require(std::abs(scratches.particles[i].radius - 9.6f) < 2e-6f, "Dust size spread affects scratch thickness");
    p.edgeBias = 1;
    const auto edges = prepareDefects(p, 1920, 1080);
    for (int i = 0; i < edges.count; ++i)
        require(edges.particles[i].x <= 1920 * 0.12f || edges.particles[i].x >= 1920 * 0.88f,
            "Full edge bias left scratches in the central region");
    p.scratchWander = 12; p.scratchWanderTime = 8;
    p.time = 7.0 / 25;
    const auto moved = prepareDefects(p, 1920, 1080);
    const auto scaled = prepareDefects(p, 3840, 2160);
    bool motion = false;
    for (int i = 0; i < moved.count; ++i) {
        require(std::abs(moved.particles[i].x - edges.particles[i].x) <= 12.001f, "Wander exceeds amplitude");
        require(std::abs(scaled.particles[i].x - 2 * moved.particles[i].x) < 1e-4f, "Wander does not scale with resolution");
        motion |= std::abs(moved.particles[i].x - edges.particles[i].x) > 0.01f;
    }
    require(motion, "Scratch wander has no effect");
    p.amount = 10; p.amountVariationPercent = 100; p.amountDriftTime = 48;
    p.scratchWander = 0;
    p.time = 0;
    const auto first = prepareDefects(p, 1920, 1080);
    bool changed = false;
    float previous = first.effectiveAmount;
    for (int frame = 0; frame < 480; ++frame) {
        p.time = frame / 25.0;
        const auto packet = prepareDefects(p, 1920, 1080);
        require(packet.effectiveAmount >= 0 && packet.effectiveAmount <= 20, "Amount variation exceeds its bounds");
        require(packet.count == static_cast<int>(std::floor(packet.effectiveAmount))
            || packet.count == static_cast<int>(std::ceil(packet.effectiveAmount)), "Fractional particle count is invalid");
        require(std::abs(packet.effectiveAmount - previous) < 0.64f, "Amount envelope is discontinuous");
        for (int i = 0; i < std::min(first.count, packet.count); ++i)
            require(packet.particles[i].x == first.particles[i].x, "Amount variation repositions existing particles");
        changed |= std::abs(packet.effectiveAmount - first.effectiveAmount) > 1;
        previous = packet.effectiveAmount;
    }
    require(changed, "Amount variation has no effect");
    p.time = 0;
    const auto repeated = prepareDefects(p, 1920, 1080);
    require(repeated.effectiveAmount == first.effectiveAmount && repeated.count == first.count,
        "Backward rendering changed the amount envelope");
    p.amountVariationPercent = 0; p.time = 3.0;
    require(prepareDefects(p, 1920, 1080).count == 10, "Variation zero must restore base Amount");
}

/** Check visible opacity after the actual neutral Print, not just a lerp of negative transmission. */
void negativePrintOpacity()
{
    for (const auto material : {DefectMaterial::Negative, DefectMaterial::Printing}) {
        for (float sourceGray : {0.02f, 0.18f, 0.5f, 1.0f}) {
            for (int alphaMode = 0; alphaMode < 4; ++alphaMode) {
                EmulsionParameters negative;
                negative.transfer = Transfer::SRGB;
                Pixel source = emulsionPixel({sourceGray, sourceGray, sourceGray, 0.75f}, negative);
                DefectFrame frame;
                auto& p = frame.parameters;
                p.material = material; p.pattern = DefectPattern::Scratches;
                p.sourcePremultiplied = alphaMode % 2 != 0; p.outputPremultiplied = alphaMode / 2 != 0;
                p.strength = p.blend = 1;
                frame.count = 1; frame.particles[0] = {5.5f, 5.5f, 3, 0};
                if (p.sourcePremultiplied) { source.r *= source.a; source.g *= source.a; source.b *= source.a; }
                EmulsionParameters print;
                print.stage = FilmStage::Print; print.transfer = Transfer::SRGB;
                print.sourcePremultiplied = p.sourcePremultiplied;
                const auto clean = emulsionPixel(source, print);
                print.sourcePremultiplied = p.outputPremultiplied;
                const auto wet = emulsionPixel(defect::apply(source, 5.5f, 5.5f, 12, frame), print);
                for (float strength : {0.001f, 0.1f, 0.5f, 1.0f}) {
                    for (float blend : {0.0f, 0.001f, 0.5f, 1.0f}) {
                        p.strength = strength; p.blend = blend;
                        const auto visible = emulsionPixel(defect::apply(source, 5.5f, 5.5f, 12, frame), print);
                        const float opacity = strength * blend;
                        const float expected = clean.r * (1 - opacity) + wet.r * opacity;
                        require(std::abs(visible.r - expected) < 4e-5f, "Negative scratches are not a gradual visible mix after Print");
                        require(visible.a == source.a, "Visible negative mixing changed alpha");
                    }
                }
            }
        }
    }
    DefectFrame old;
    old.parameters.material = DefectMaterial::Negative; old.parameters.pattern = DefectPattern::Scratches;
    old.parameters.mixResponse = DefectMixResponse::Physical; old.parameters.strength = 0.001f;
    old.count = 1; old.particles[0] = {5.5f, 5.5f, 3, 0};
    EmulsionParameters negative, print;
    negative.transfer = print.transfer = Transfer::SRGB; print.stage = FilmStage::Print;
    const auto source = emulsionPixel({1, 1, 1, 1}, negative);
    const auto visible = emulsionPixel(defect::apply(source, 5.5f, 5.5f, 12, old), print);
    require(visible.r < 0.95f, "Regression fixture no longer reproduces amplification of tiny physical negative scratches");
}
/** Export fixed geometry independently replayable by the actual Python mask functions. */
void dumpComfyFixtures(const std::filesystem::path& directory)
{
    constexpr int width = 31, height = 19;
    std::filesystem::create_directories(directory);
    const auto write = [&](const std::string& name, const std::vector<float>& pixels) {
        std::ofstream stream(directory / name, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * sizeof(float)));
        require(static_cast<bool>(stream), "Cannot write Comfy defects fixture");
    };
    for (int pattern = 0; pattern < 3; ++pattern) {
        DefectFrame frame;
        frame.parameters.damage = DefectDamage::Surface;
        frame.parameters.pattern = static_cast<DefectPattern>(pattern);
        frame.parameters.mixResponse = DefectMixResponse::Physical;
        frame.parameters.strength = 1;
        frame.parameters.irregularity = 0.6f;
        frame.count = 2;
        for (int slot = 0; slot < 2; ++slot) {
            auto& particle = frame.particles[slot];
            const float diameter = slot == 0 ? (pattern == 2 ? 5.5f : pattern == 1 ? 1.2f : 3.2f) : 0.35f;
            particle.x = slot == 0 ? 7.25f : 8.25f;
            particle.y = slot == 0 ? 8.75f : 9.25f;
            if (pattern == 1) {
                particle.y = particle.x;
                const float amplitude = width * 0.008f * frame.parameters.irregularity;
                particle.x += amplitude * (-0.4f + 0.8f) * 0.5f;
                particle.drift = amplitude * (0.8f + 0.4f);
                particle.radius = std::max(0.2f, diameter * 0.5f) * (slot == 0 ? 0.9f : 1.2f);
                particle.support = amplitude + particle.radius;
                particle.opacity = std::min(1.0f, diameter / 0.6f);
                particle.gapTop = 0.15f; particle.gapBottom = 0.75f;
            } else {
                particle.radius = std::max(0.3f, diameter * 0.5f);
                const float angle = slot == 0 ? 0.3f : -0.6f;
                particle.cosine = std::cos(angle); particle.sine = std::sin(angle);
                particle.aspect = slot == 0 ? 0.7f : 1.1f;
                particle.phase = slot == 0 ? -0.4f : 0.8f;
                particle.opacity = std::min(1.0f, diameter / 0.7f) * (slot == 0 ? 0.8f : 0.65f);
                particle.support = particle.radius * (pattern == 2 ? 3.0f : 1.5f);
            }
        }
        std::vector<float> mask(width * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                mask[static_cast<std::size_t>(y * width + x)] = defect::coverage(x + 0.5f, y + 0.5f, height, frame);
        write("mask_" + std::to_string(pattern) + ".f32", mask);
        for (int material = 0; material < 5; ++material)
            for (int damage = 1; damage <= 3; ++damage) {
                auto& p = frame.parameters;
                p.material = static_cast<DefectMaterial>(material); p.damage = static_cast<DefectDamage>(damage);
                p.transfer = Transfer::Linear; p.strength = 0.37f;
                std::vector<float> image(width * height * 4);
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) {
                        const Pixel pixel = defect::apply({0.002f, 0.3f, 0.9f, 0.5f}, x + 0.5f, y + 0.5f, height, frame);
                        const auto offset = static_cast<std::size_t>(y * width + x) * 4;
                        image[offset] = pixel.r; image[offset + 1] = pixel.g; image[offset + 2] = pixel.b; image[offset + 3] = pixel.a;
                    }
                write("image_" + std::to_string(pattern) + "_" + std::to_string(material) + "_" + std::to_string(damage) + ".f32", image);
            }
    }
}

/** Check material physics independently of the geometric mask and legacy reference. */
void comfyMaterials()
{
    for (int material = 0; material < 5; ++material)
        for (int pattern = 0; pattern < 3; ++pattern)
            for (int damage = 1; damage <= 3; ++damage)
                for (float strength : {0.0f, 0.001f, 0.37f, 1.0f}) {
                    DefectParameters p;
                    p.material = static_cast<DefectMaterial>(material); p.pattern = static_cast<DefectPattern>(pattern);
                    p.damage = static_cast<DefectDamage>(damage); p.transfer = Transfer::Linear;
                    p.strength = strength; p.mixResponse = DefectMixResponse::Physical;
                    const float mask = 0.7f * strength, source = 0.12f;
                    const bool gate = material == 0 || material == 2 || material == 4;
                    double expected = source;
                    if (gate || (damage == 2 && pattern == 0)) expected *= 1 - mask;
                    else if (damage == 2) expected += (std::pow(10.0, material == 1 ? -0.1 : 0.0) - source) * mask;
                    else expected *= std::pow(10.0, -(damage == 3 ? 1.5 : 0.65) * mask);
                    require(std::abs(defect::shade(source, 1, mask, p) - expected) < 2e-6,
                        "Comfy damage material equation differs from independent density/transmission oracle");
                }
    for (int material = 0; material < 5; ++material)
        for (int pattern = 0; pattern < 3; ++pattern)
            for (int damage = 1; damage <= 3; ++damage)
                for (int alpha = 0; alpha < 4; ++alpha) {
                    DefectParameters p;
                    p.material = static_cast<DefectMaterial>(material); p.pattern = static_cast<DefectPattern>(pattern);
                    p.damage = static_cast<DefectDamage>(damage); p.size = pattern == 2 ? 10 : 4;
                    p.amount = 12.5f; p.time = 2.375; p.irregularity = 0.6f;
                    p.sourcePremultiplied = alpha % 2; p.outputPremultiplied = alpha / 2;
                    for (const auto response : {DefectMixResponse::Physical, DefectMixResponse::Perceptual}) {
                        p.mixResponse = response;
                        run(p, true, false);
#ifdef SILENTFILM_TEST_CUDA
                        if (alpha == 0 && damage == 1) run(p, false, false);
#endif
                    }
                }
    DefectParameters p;
    p.damage = DefectDamage::Surface; p.material = DefectMaterial::Negative; p.amount = 6;
    p.time = 0; const auto first = prepareDefects(p, 1920, 1080);
    p.time = 1 / p.frameRate; const auto next = prepareDefects(p, 1920, 1080);
    p.time = 0; const auto repeat = prepareDefects(p, 1920, 1080);
    require(first.count == repeat.count && first.particles[0].x == repeat.particles[0].x,
        "Comfy-style geometry depends on render order");
    require(first.particles[0].x != next.particles[0].x, "Film dust must change each physical frame");
}
/** Exercise development independently of legacy material masks and confirm deterministic time semantics. */
void developmentProperties()
{
    DefectParameters p;
    p.pattern = DefectPattern::Development; p.development = DefectDevelopment::Mixed;
    p.mixResponse = DefectMixResponse::Physical; p.amount = 0; p.size = 0.01f; p.strength = 0.37f;
    auto capped = prepareDefects(p, 8192, 4320);
    require(capped.development.rows == 128 && capped.development.columns == 128
        && capped.development.count == 128 * 128 + 2 * 128, "Development coarse grid is not bounded");
    p.size = 8; p.irregularity = 0; p.time = 0;
    const auto stationary = prepareDefects(p, 31, 19);
    p.time = 100 / p.frameRate;
    const auto later = prepareDefects(p, 31, 19);
    for (int i = 0; i < stationary.development.count; ++i)
        require(defect::developmentGridValue(stationary, i) == defect::developmentGridValue(later, i),
            "Irregularity zero must freeze development across events");
    p.irregularity = 0.6f; p.time = 23 / p.frameRate; p.persistence = 24;
    const auto a = prepareDefects(p, 31, 19);
    p.time = 24 / p.frameRate;
    const auto b = prepareDefects(p, 31, 19);
    p.time = 23 / p.frameRate;
    const auto repeated = prepareDefects(p, 31, 19);
    require(a.development.knot == 0 && b.development.knot == 1, "Development knot timing is wrong");
    float difference = 0;
    for (int i = 0; i < a.development.count; ++i) {
        const float value = defect::developmentGridValue(a, i);
        difference = std::max(difference, std::abs(value - defect::developmentGridValue(b, i)));
        require(value == defect::developmentGridValue(repeated, i), "Development depends on request order");
    }
    require(difference > 0 && difference < 0.02f, "Development jumps at the persistence boundary");
    const float x = 12.5f, y = 8.5f;
    p.development = DefectDevelopment::Mottling; const auto mottling = prepareDefects(p, 31, 19);
    p.development = DefectDevelopment::Streaks; const auto streaks = prepareDefects(p, 31, 19);
    p.development = DefectDevelopment::Mixed; const auto mixed = prepareDefects(p, 31, 19);
    const float expected = defect::developmentField(x, y, mottling) * 0.65f + defect::developmentField(x, y, streaks) * 0.35f;
    require(std::abs(defect::developmentField(x, y, mixed) - expected) < 2e-6f, "Mixed development uses wrong layer weights");
    auto cached = mixed;
    std::vector<float> grid(static_cast<std::size_t>(cached.development.count));
    for (int i = 0; i < cached.development.count; ++i) grid[static_cast<std::size_t>(i)] = defect::developmentGridValue(cached, i);
    cached.development.values = grid.data();
    require(std::abs(defect::developmentField(x, y, cached) - expected) < 2e-6f,
        "Prepared development lattice differs from procedural evaluation");
    p.developmentFog = 0;
    const float withoutFog = defect::development(x, y, prepareDefects(p, 31, 19));
    p.developmentFog = 1;
    require(defect::development(x, y, prepareDefects(p, 31, 19)) <= withoutFog, "Development fog must add positive density");
    p.blend = 0;
    require(prepareDefects(p, 31, 19).development.count == 0, "Bypass should not prepare development grids");
    for (int mode = 1; mode <= 3; ++mode)
        for (int material : {1, 3})
            for (int alpha = 0; alpha < 4; ++alpha)
                for (float irregularity : {0.0f, 0.6f, 1.0f}) {
                    DefectParameters q;
                    q.pattern = DefectPattern::Development; q.development = static_cast<DefectDevelopment>(mode);
                    q.material = static_cast<DefectMaterial>(material); q.amount = 0; q.size = 8;
                    q.irregularity = irregularity; q.time = 1.375; q.developmentFog = 0.15f;
                    q.sourcePremultiplied = alpha % 2; q.outputPremultiplied = alpha / 2;
                    for (auto response : {DefectMixResponse::Physical, DefectMixResponse::Perceptual}) {
                        q.mixResponse = response; run(q, true, false);
#ifdef SILENTFILM_TEST_CUDA
                        if (alpha == 0) run(q, false, false);
#endif
                    }
                }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string(argv[1]) == "--dump-comfy") { dumpComfyFixtures(argv[2]); return 0; }
#ifdef SILENTFILM_TEST_CUDA
        int count = 0; const auto status = cudaGetDeviceCount(&count);
        if (status == cudaErrorNoDevice || (status == cudaSuccess && count == 0)) return 77;
        require(status == cudaSuccess, "CUDA device query failed");
#endif
        if (argc == 3 && std::string(argv[1]) == "--dump-development") { development_fixture::dump(argv[2]); return 0; }
        timingAndBypass(); particleControls(); negativePrintOpacity(); comfyMaterials(); developmentProperties();
        for (int material = 0; material < 5; ++material) {
            for (int pattern = 0; pattern < 4; ++pattern) {
                for (int mode = 0; mode < 4; ++mode) {
                    DefectParameters p;
                    p.sizeSpreadPercent = p.thicknessSpreadPercent = 100.0f / 3;
                    p.material = static_cast<DefectMaterial>(material); p.pattern = static_cast<DefectPattern>(pattern);
                    p.transfer = static_cast<Transfer>(mode % 3); p.strength = mode == 3 ? 0 : 0.6f;
                    p.amount = mode == 2 ? 0 : 3.5f; p.size = pattern == 3 ? 15 : 7;
                    p.seed = 1925 + material; p.time = 1.25; p.frameRate = 24; p.persistence = 12;
                    p.sourcePremultiplied = mode % 2 != 0; p.outputPremultiplied = mode / 2 != 0;
                    for (float blend : {0.0f, 0.37f, 1.0f}) {
                        p.blend = blend;
                        for (const auto response : {DefectMixResponse::Perceptual, DefectMixResponse::Physical}) {
                            p.mixResponse = response;
                            run(p, true);
#ifdef SILENTFILM_TEST_CUDA
                            run(p, false);
                            auto animated = p;
                            animated.sizeSpreadPercent = 75; animated.thicknessSpreadPercent = 80;
                            animated.edgeBias = 0.75f; animated.scratchWander = 12;
                            animated.scratchWanderTime = 8;
                            animated.amountVariationPercent = 100; animated.amountDriftTime = 48;
                            run(animated, true, false);
#endif
                        }
                    }
                }
            }
        }
        std::cout << "Defects legacy and Comfy materials CPU/CUDA: PASS\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
