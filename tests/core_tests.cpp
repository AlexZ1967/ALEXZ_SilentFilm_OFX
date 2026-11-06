#include "silentfilm/probe.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

/** Reject a wrong diagnostic result independently of NDEBUG or compiler assert flags. */
void expect(float actual, float expected)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-6f) {
        throw std::runtime_error("Unexpected pixel value");
    }
}

} // namespace

int main()
{
    try {
        expect(silentfilm::gainFromEV(1), 2);
        expect(silentfilm::gainFromEV(-1), 0.5f);
        const silentfilm::Pixel current{0.25f, 0.5f, 2.0f, 0.4f};
        const auto identity = silentfilm::probePixel(current, {}, {}, 1.0f, 0.0f);
        expect(identity.r, current.r); expect(identity.b, current.b); expect(identity.a, current.a);
        const auto exposure = silentfilm::probePixel(current, {}, {}, 2.0f, 0.0f);
        expect(exposure.r, 0.5f); expect(exposure.g, 1.0f); expect(exposure.b, 4.0f); expect(exposure.a, 0.4f);
        const auto temporal = silentfilm::probePixel({0.4f, 0.4f, 0.4f, 0.4f}, {0, 0, 0, 0}, {1, 1, 1, 1}, 1, 1);
        expect(temporal.r, 0.45f); expect(temporal.a, 0.45f);
        // Перестановка запросов не должна менять состояние ядра.
        (void)silentfilm::probePixel({}, {}, {}, 0.5f, 0.7f);
        const auto repeated = silentfilm::probePixel(current, {}, {}, 1.0f, 0.0f);
        expect(repeated.b, identity.b);
        std::cout << "SilentFilm core: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
