#pragma once

#include "silentfilm/film_optics.hpp"

namespace silentfilm::color {

/** Keep the external RGB portions of the Comfy-compatible pipeline in sRGB/Rec.709. */
constexpr Transfer pipelineTransfer = Transfer::SRGB;

/** Negative and printing-stage values are film transmission, not display-encoded RGB. */
constexpr bool filmTransmission(DefectMaterial material) noexcept
{
    return material == DefectMaterial::Negative || material == DefectMaterial::Printing;
}

/** Apply a manual transfer only to encoded image stages, never to intermediate stock transmission. */
constexpr Transfer defectTransfer(DefectMaterial material, Transfer requested) noexcept
{
    return filmTransmission(material) ? Transfer::Linear : requested;
}

} // namespace silentfilm::color
