#pragma once

#include "silentfilm/color_contract.hpp"

#include <vector>

namespace silentfilm {

enum class ReferenceStage { CameraOptics, Camera, Negative, Print, Projector, Defects };
struct ReferenceValue { const char* id; double value; bool integer = false; };
struct ReferencePreset { const char* label; int node; std::vector<ReferenceValue> values; };

/** Snapshot available OFX controls from the user's with_defects workflow, without loading Comfy at runtime. */
inline std::vector<ReferencePreset> referencePresets(ReferenceStage stage)
{
    using V = ReferenceValue;
    const ReferencePreset colorOnly{"Comfy color only (keep effect)", 0,
        {{"encoding", static_cast<double>(color::pipelineTransfer), true}}};
    if (stage == ReferenceStage::CameraOptics || stage == ReferenceStage::Camera) {
        std::vector<V> workflow{{"vignette_strength", 0.6}, {"edge_softness", 0.05}, {"edge_start", 0.6}, {"edge_end", 1.3},
            {"camera_softness", 0.15}, {"focus_drift", 0.04}, {"focus_seed", 1925, true}};
        if (stage == ReferenceStage::Camera) {
            const std::vector<V> capture{{"playback_mode", 0, true}, {"capture_fps_min", 20}, {"capture_fps_max", 24},
                {"crank_variation", 1}, {"crank_irregularity", 0.5}, {"shutter_fraction", 0.5}, {"motion_blur_strength", 0.5},
                {"exposure_variation", 1}, {"exposure_samples", 9, true}, {"camera_shake", 0.5}, {"camera_seed", 1925, true}};
            workflow.insert(workflow.end(), capture.begin(), capture.end());
        }
        auto neutral = workflow;
        // Радиусы и seed при отключении оптики сохраняются.
        neutral[0].value = neutral[1].value = neutral[4].value = neutral[5].value = 0;
        return {{"Comfy workflow: Camera", 2, workflow}, {"Neutral optics", 2, neutral}};
    }
    if (stage == ReferenceStage::Negative || stage == ReferenceStage::Print) {
        const bool negative = stage == ReferenceStage::Negative;
        std::vector<V> workflow{{"exposure_ev", 0}, {"contrast", 1}, {"shadow_softness", 0}, {"highlight_softness", 0},
            {"fog_strength", 0}, {"halation_strength", 0}, {"halation_size", 0.15}, {"grain_strength", negative ? 0.01 : 0},
            {"grain_size", 2}, {"grain_seed", 1925, true}, {"encoding", 1, true}};
        if (negative) workflow.push_back({"preview_positive", 0, true});
        auto neutral = workflow, grain = workflow;
        neutral[7].value = 0; grain[7].value = 0.01;
        return {{negative ? "Comfy workflow: Negative" : "Comfy workflow: Print", negative ? 3 : 4, workflow},
            {"Neutral emulsion", 0, neutral}, {"Grain only: 0.01 / size 2", 0, grain}, colorOnly};
    }
    if (stage == ReferenceStage::Projector) {
        std::vector<V> workflow{{"gate_weave_px", 2}, {"lamp_flicker", 0.035}, {"lamp_breathing", 0.015},
            {"projector_seed", 1925, true}, {"glow_strength", 0}, {"glow_size", 1}, {"encoding", 1, true}};
        auto neutral = workflow;
        neutral[0].value = neutral[1].value = neutral[2].value = 0;
        return {{"Comfy workflow: Projector", 5, workflow}, {"Neutral projection", 0, neutral}, colorOnly};
    }
    const auto defect = [](const char* label, int node, int material, int pattern, double strength, double size,
                           double amount, int persistence, int seed) {
        return ReferencePreset{label, node, {{"material", static_cast<double>(material), true}, {"pattern", static_cast<double>(pattern), true},
            {"strength", strength}, {"blend", 1}, {"mix_response", 1, true},
            {"development_model", node == 15 ? 1.0 : node == 18 ? 2.0 : 0.0, true}, {"development_fog", 0.15},
            {"damage_type", pattern == 3 ? 0.0 : node == 16 || node == 28 || node == 39 ? 3.0
                : node == 19 || node == 32 || node == 40 ? 2.0 : 1.0, true},
            {"irregularity", node == 14 ? 1.0 : node == 28 ? 0.5 : 0.6},
            {"size", size}, {"amount", amount}, {"persistence", static_cast<double>(persistence), true},
            {"seed", static_cast<double>(seed), true}, {"encoding", static_cast<double>(color::pipelineTransfer), true},
            {"size_spread", 35}, {"thickness_spread", 35},
            {"edge_bias", node == 14 || node == 19 ? 0.7 : node == 17 ? 0.35 : node == 20 ? 0.5 : node == 28 ? 1.0 : 0.0},
            {"scratch_wander", 0}, {"scratch_wander_time", pattern == 1 ? 1.0 : 8.0},
            {"amount_variation", 0}, {"amount_drift_time", 48}}};
    };
    return {defect("n14 Capture dirt (mapped)", 14, 0, 0, 0, 0.2, 10, 48, 1925),
        defect("n15 Negative development (mapped)", 15, 1, 3, 0.1, 15, 3, 48, 1924),
        defect("n16 Negative dirty dust (mapped)", 16, 1, 0, 0.3, 0.3, 3, 24, 1923),
        defect("n32 Negative emulsion loss (mapped)", 32, 1, 0, 0.5, 0.15, 7, 24, 1922),
        defect("n33 Negative surface dust (mapped)", 33, 1, 0, 1, 0.1, 10, 24, 1921),
        defect("n17 Printing dirt (mapped)", 17, 2, 0, 0, 0.12, 10, 48, 1925),
        defect("n18 Positive development (mapped)", 18, 3, 3, 0, 8, 3, 48, 1925),
        defect("n19 Positive emulsion loss (mapped)", 19, 3, 1, 0.03, 0.07, 2, 10, 1930),
        defect("n28 Positive dirty scratches (mapped)", 28, 3, 1, 0.03, 0.12, 1.5, 20, 2022),
        defect("n39 Late negative dirty (mapped)", 39, 1, 0, 0.1, 0.3, 3, 24, 1919),
        defect("n40 Late negative emulsion loss (mapped)", 40, 1, 0, 0.3, 0.15, 7, 24, 1918),
        defect("n41 Late negative surface (mapped)", 41, 1, 0, 0.5, 0.1, 10, 24, 1917),
        defect("n20 Projector dirt (mapped)", 20, 4, 0, 0, 0.3, 5, 48, 1925), colorOnly};
}
} // namespace silentfilm
