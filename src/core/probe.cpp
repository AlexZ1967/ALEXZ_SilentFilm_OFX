#include "silentfilm/probe.hpp"

#include <cmath>

namespace silentfilm {

float gainFromEV(double value) noexcept
{
    return static_cast<float>(std::exp2(value));
}

} // namespace silentfilm
