#include "silentfilm/sensitometry.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

using silentfilm::EmulsionParameters;
using silentfilm::FilmStage;
using silentfilm::Pixel;
using silentfilm::Transfer;

/** Report numerical failures without relying on release-disabled assertions. */
void expect(double actual, double expected, double tolerance, const char* name)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::ostringstream message;
        message << std::setprecision(10) << name << ": got " << actual << ", expected " << expected << ", tolerance " << tolerance;
        throw std::runtime_error(message.str());
    }
}

/** Provide an independent double-precision transcription of the Python density limits. */
double referenceLimits(double density, double floor, double ceiling, double lowKnee, double highKnee, double low, double high)
{
    if (low > 0 && density < lowKnee) {
        const double span = lowKnee - floor;
        density = (1 - low) * density + low * (lowKnee + span * std::expm1((density - lowKnee) / span));
    }
    if (high > 0 && density > highKnee) {
        const double span = ceiling - highKnee;
        density = (1 - high) * density + high * (highKnee - span * std::expm1(-(density - highKnee) / span));
    }
    return density;
}

/** Evaluate the original calibrated negative model using double precision. */
double referenceNegative(double linear, const EmulsionParameters& parameters)
{
    const double gray = std::log10(0.0001 + 0.9999 * 0.18);
    const double density = 2.7 + 0.65 * gray + 0.65 * parameters.contrast
        * (std::log10(0.0001 + 0.9999 * linear) + parameters.exposureEV * std::log10(2.0) - gray);
    return referenceLimits(density, 0.1, 2.7,
        2.7 + 0.65 * std::log10(0.0001 + 0.9999 * 0.03),
        2.7 + 0.65 * std::log10(0.0001 + 0.9999 * 0.5),
        parameters.shadowSoftness, parameters.highlightSoftness) + 0.25 * parameters.fog;
}

/** Evaluate positive exposure/development independently from the float implementation. */
double referencePrint(double transmission, const EmulsionParameters& parameters)
{
    const double gray = 0.02 - std::log10(0.0001 + 0.9999 * 0.18);
    const double nominal = 0.02 + (std::log10(std::max(transmission, 1e-12)) + parameters.exposureEV * std::log10(2.0) + 2.7) / 0.65;
    return referenceLimits(gray + parameters.contrast * (nominal - gray), 0.02, 4.02,
        0.02 - std::log10(0.0001 + 0.9999 * 0.5),
        0.02 - std::log10(0.0001 + 0.9999 * 0.03),
        parameters.highlightSoftness, parameters.shadowSoftness) + 0.25 * parameters.fog;
}

/** Check the complete neutral pair over a grayscale ramp in every supported transfer. */
void neutralPair()
{
    for (const auto transfer : {Transfer::Linear, Transfer::SRGB, Transfer::Gamma24}) {
        EmulsionParameters negative, print;
        negative.transfer = print.transfer = transfer;
        print.stage = FilmStage::Print;
        for (int step = 0; step <= 4096; ++step) {
            const float value = static_cast<float>(step) / 4096;
            const auto transmission = silentfilm::emulsionPixel({value, value, value, 1}, negative);
            const auto positive = silentfilm::emulsionPixel(transmission, print);
            expect(silentfilm::film::decode(positive.r, transfer), silentfilm::film::decode(value, transfer), 3e-6, "neutral pair, linear light");
            // Float32-пропускание вблизи black floor ограничивает точность закодированного чёрного.
            expect(positive.r, value, 5e-4, "neutral pair, encoded value");
            expect(positive.a, 1, 0, "opaque alpha");
            expect(positive.g, positive.r, 0, "monochrome green");
            expect(positive.b, positive.r, 0, "monochrome blue");
        }
    }
}

/** Compare neutral and artistic controls against the independent density reference. */
void densityReference()
{
    for (int variant = 0; variant < 4; ++variant) {
        EmulsionParameters parameters;
        if (variant == 1) { parameters.exposureEV = 0.3f; parameters.contrast = 1.2f; parameters.shadowSoftness = 0.4f; parameters.highlightSoftness = 0.7f; parameters.fog = 0.12f; }
        if (variant == 2) { parameters.exposureEV = -4; parameters.contrast = 3; parameters.shadowSoftness = 1; parameters.highlightSoftness = 1; parameters.fog = 1; }
        if (variant == 3) { parameters.exposureEV = 4; parameters.contrast = 0.2f; }
        for (const float value : {0.0f, 0.0001f, 0.003f, 0.03f, 0.18f, 0.5f, 1.0f}) {
            expect(silentfilm::film::negativeDensity(value, parameters), referenceNegative(value, parameters), 5e-6, "negative density reference");
        }
        for (const float value : {0.0f, 1e-12f, 0.002f, 0.01f, 0.1f, 0.79f, 1.0f}) {
            expect(silentfilm::film::printDensity(value, parameters), referencePrint(value, parameters), 5e-5, "print density reference");
        }
    }
}

/** Preserve alpha, reference viewing, explicit encoding and the physical EV directions. */
void behavior()
{
    EmulsionParameters negative, print;
    print.stage = FilmStage::Print;
    const Pixel source{0.18f, 0.18f, 0.18f, 1};
    const auto film = silentfilm::emulsionPixel(source, negative);
    const auto neutral = silentfilm::emulsionPixel(film, print);
    negative.exposureEV = 1;
    const auto brighter = silentfilm::emulsionPixel(silentfilm::emulsionPixel(source, negative), print);
    if (brighter.r <= neutral.r) throw std::runtime_error("Negative exposure must brighten the neutral print");
    print.exposureEV = 1;
    if (silentfilm::emulsionPixel(film, print).r >= neutral.r) throw std::runtime_error("Print exposure must darken the print");
    negative = {}; print = {}; print.stage = FilmStage::Print;
    for (const auto transfer : {Transfer::Linear, Transfer::SRGB, Transfer::Gamma24}) {
        negative.transfer = print.transfer = transfer;
        negative.previewPositive = true;
        const auto preview = silentfilm::emulsionPixel(source, negative);
        negative.previewPositive = false;
        const auto actual = silentfilm::emulsionPixel(silentfilm::emulsionPixel(source, negative), print);
        expect(preview.r, actual.r, 5e-5, "reference positive preview");
        negative.sourcePremultiplied = negative.outputPremultiplied = true;
        print.sourcePremultiplied = print.outputPremultiplied = true;
        const Pixel translucent{0.18f * 0.25f, 0.18f * 0.25f, 0.18f * 0.25f, 0.25f};
        const auto positive = silentfilm::emulsionPixel(silentfilm::emulsionPixel(translucent, negative), print);
        expect(positive.a, translucent.a, 0, "premultiplied alpha");
        expect(positive.r, translucent.r, 1e-5, "premultiplied neutral pair");
        const auto transparent = silentfilm::emulsionPixel({}, negative);
        expect(transparent.r, 0, 0, "transparent RGB"); expect(transparent.a, 0, 0, "transparent alpha");
        negative.sourcePremultiplied = negative.outputPremultiplied = false;
        print.sourcePremultiplied = print.outputPremultiplied = false;
    }
    negative = {}; print = {}; print.stage = FilmStage::Print;
    const auto colored = silentfilm::emulsionPixel({0.2f, 0.4f, 0.8f, 1}, negative);
    expect(silentfilm::emulsionPixel(colored, print).r, 0.2 * 0.2126 + 0.4 * 0.7152 + 0.8 * 0.0722, 3e-6, "linear monochrome response");
    print.transfer = Transfer::Gamma24;
    const auto gamma = silentfilm::emulsionPixel(colored, print);
    print.transfer = Transfer::Linear;
    const auto linear = silentfilm::emulsionPixel(colored, print);
    expect(silentfilm::film::decode(gamma.r, Transfer::Gamma24), linear.r, 2e-6, "Print input is raw transmission regardless of output encoding");
    negative.sourcePremultiplied = true;
    print.sourcePremultiplied = false;
    print.outputPremultiplied = true;
    const auto changedRepresentation = silentfilm::emulsionPixel(silentfilm::emulsionPixel({0.045f, 0.045f, 0.045f, 0.25f}, negative), print);
    expect(changedRepresentation.r, 0.045, 3e-6, "host alpha representation conversion");
}

} // namespace

int main()
{
    try {
        neutralPair(); densityReference(); behavior();
        std::cout << "SilentFilm sensitometry: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
