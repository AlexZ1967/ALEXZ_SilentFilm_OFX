#include "silentfilm/probe.hpp"
#include "silentfilm/cuda_backend.hpp"
#include "silentfilm/optics.hpp"
#include "silentfilm/film_optics.hpp"
#include "silentfilm/camera_capture.hpp"
#include "silentfilm/workflow_presets.hpp"
#include "silentfilm/defects_pixel.hpp"
#include "silentfilm/color_contract.hpp"
#include "silentfilm/crank_exposure.hpp"

#include <ofxImageEffect.h>
#include <ofxGPURender.h>
#include <ofxProperty.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

namespace {

OfxHost* host = nullptr;
const OfxPropertySuiteV1* properties = nullptr;
const OfxImageEffectSuiteV1* effects = nullptr;
const OfxParameterSuiteV1* parameters = nullptr;
const OfxMessageSuiteV1* messages = nullptr;
bool temporalAccess = false;

constexpr const char* gainName = "diagnostic_gain_ev";
constexpr const char* mixName = "temporal_mix";
constexpr const char* offsetName = "temporal_offset";

enum class EffectKind { Probe, Camera, CameraOptics, Negative, Print, Projector, Defects, CrankExposure };

/** Keep independent plugin names and identifiers within one shared adapter. */
const char* effectName(EffectKind kind)
{
    if (kind == EffectKind::CrankExposure) return "SilentFilm Crank Exposure";
    if (kind == EffectKind::Negative) return "SilentFilm Negative";
    if (kind == EffectKind::Print) return "SilentFilm Print";
    if (kind == EffectKind::Projector) return "SilentFilm Projector";
    if (kind == EffectKind::Defects) return "SilentFilm Defects";
    if (kind == EffectKind::Camera) return "SilentFilm Camera";
    if (kind == EffectKind::CameraOptics) return "SilentFilm Camera Optics";
    return "SilentFilm Host Probe";
}

/** Trace action boundaries and report slow host calls without synchronizing CUDA work. */
class ActionTimer {
    const char* plugin_;
    const char* action_;
    bool trace_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
public:
    ActionTimer(const char* plugin, const char* action) : plugin_(plugin), action_(action) {
        const auto* setting = std::getenv("ALEXZ_SILENTFILM_OFX_TRACE");
        trace_ = setting && std::strcmp(setting, "1") == 0;
        if (trace_) std::fprintf(stderr, "[ALEXZSilentFilmOFX] begin plugin=%s action=%s\n", plugin_, action_);
    }
    ~ActionTimer() {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started_).count();
        if (trace_ || ms >= 250)
            std::fprintf(stderr, "[ALEXZSilentFilmOFX] end plugin=%s action=%s elapsed_ms=%.3f\n", plugin_, action_, ms);
    }
};

struct Failure : std::runtime_error {
    OfxStatus status;
    Failure(OfxStatus value, const char* operation) : std::runtime_error(operation), status(value) {}
};

/** Convert host status failures into exceptions contained by the C entry point. */
void require(OfxStatus status, const char* operation)
{
    if (status != kOfxStatOK) {
        throw Failure(status, operation);
    }
}

/** Ignore an unknown optional property without hiding other host errors. */
void optionalProperty(OfxStatus status, const char* operation)
{
    if (status != kOfxStatErrUnknown) require(status, operation);
}

/** Retrieve the property set belonging to an effect or descriptor. */
OfxPropertySetHandle effectProperties(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle result = nullptr;
    require(effects->getPropertySet(effect, &result), "getPropertySet");
    return result;
}

struct Instance {
    OfxParamHandle exposureMap = nullptr, exposureBlend = nullptr, exposureOffset = nullptr;
    silentfilm::CrankExposureMapCache exposureMaps;
    EffectKind kind = EffectKind::Probe;
    OfxImageClipHandle source = nullptr;
    OfxImageClipHandle output = nullptr;
    OfxParamHandle gain = nullptr;
    OfxParamHandle mix = nullptr;
    OfxParamHandle offset = nullptr;
    OfxParamHandle exposure = nullptr, contrast = nullptr, shadow = nullptr, highlight = nullptr, fog = nullptr;
    OfxParamHandle halationStrength = nullptr, halationSize = nullptr;
    OfxParamHandle encoding = nullptr, positivePreview = nullptr;
    OfxParamHandle grainStrength = nullptr, grainSize = nullptr, grainSeed = nullptr;
    OfxParamHandle vignetteStrength = nullptr, edgeSoftness = nullptr, edgeStart = nullptr, edgeEnd = nullptr;
    OfxParamHandle cameraSoftness = nullptr, focusDrift = nullptr, focusSeed = nullptr;
    OfxParamHandle playbackMode = nullptr;
    OfxParamHandle captureFpsMin = nullptr, captureFpsMax = nullptr, crankVariation = nullptr;
    OfxParamHandle crankIrregularity = nullptr, shutterFraction = nullptr, motionBlurStrength = nullptr;
    OfxParamHandle exposureVariation = nullptr;
    OfxParamHandle exposureSamples = nullptr, cameraShake = nullptr, cameraSeed = nullptr;
    silentfilm::HandCrankSchedule crankSchedule;
    silentfilm::CameraFocusCache focusSignals;
    OfxParamHandle glowStrength = nullptr, glowSize = nullptr;
    OfxParamHandle gateWeave = nullptr, lampFlicker = nullptr, lampBreathing = nullptr, projectorSeed = nullptr;
    silentfilm::ProjectorSignalCache projectorSignals;
#ifdef SILENTFILM_ENABLE_CUDA
    silentfilm::ProjectorCudaCache projectorCuda;
#endif
    OfxParamHandle defectMaterial = nullptr, defectPattern = nullptr, defectStrength = nullptr, defectSize = nullptr;
    OfxParamHandle defectBlend = nullptr;
    OfxParamHandle defectMixResponse = nullptr;
    OfxParamHandle defectAmount = nullptr, defectPersistence = nullptr, defectSeed = nullptr;
    OfxParamHandle defectSizeSpread = nullptr, defectThicknessSpread = nullptr, defectEdgeBias = nullptr;
    OfxParamHandle defectScratchWander = nullptr, defectScratchWanderTime = nullptr;
    OfxParamHandle defectAmountVariation = nullptr, defectAmountDriftTime = nullptr;
};

/** Fetch instance state without storing image handles across render actions. */
Instance& instance(OfxImageEffectHandle effect)
{
    void* value = nullptr;
    require(properties->propGetPointer(effectProperties(effect), kOfxPropInstanceData, 0, &value), "instance data");
    if (!value) {
        throw Failure(kOfxStatErrBadHandle, "Missing effect instance");
    }
    return *static_cast<Instance*>(value);
}

struct Settings {
    double gainEV = 0;
    double mix = 0;
    int offset = 1;
};

/** Evaluate all animated parameters at the requested frame time. */
Settings settingsAt(Instance& state, double time)
{
    Settings value;
    require(parameters->paramGetValueAtTime(state.gain, time, &value.gainEV), "read gain");
    require(parameters->paramGetValueAtTime(state.mix, time, &value.mix), "read temporal mix");
    require(parameters->paramGetValueAtTime(state.offset, time, &value.offset), "read temporal offset");
    if (!std::isfinite(time) || !std::isfinite(value.gainEV) || !std::isfinite(value.mix)
        || value.gainEV < -4 || value.gainEV > 4 || value.mix < 0 || value.mix > 1
        || value.offset < 1 || value.offset > 8) {
        throw Failure(kOfxStatErrValue, "Invalid diagnostic parameter value");
    }
    if (value.mix > 0 && !temporalAccess) {
        throw Failure(kOfxStatErrUnsupported, "Host does not support temporal clip access");
    }
    return value;
}

void sourceFrameRange(OfxImageClipHandle clip, double range[2]);

/** Evaluate exposure/development controls independently from the diagnostic effect. */
silentfilm::EmulsionParameters filmSettingsAt(Instance& state, double time)
{
    double exposure = 0, contrast = 1, shadow = 0, highlight = 0, fog = 0, grainStrength = 0, grainSize = 2;
    double halationStrength = 0, halationSize = 0.15;
    int encoding = static_cast<int>(silentfilm::color::pipelineTransfer), preview = 0;
    int grainSeed = 1925;
    require(parameters->paramGetValueAtTime(state.exposure, time, &exposure), "read film exposure");
    require(parameters->paramGetValueAtTime(state.contrast, time, &contrast), "read film contrast");
    require(parameters->paramGetValueAtTime(state.shadow, time, &shadow), "read film shadow compression");
    require(parameters->paramGetValueAtTime(state.highlight, time, &highlight), "read film highlight compression");
    require(parameters->paramGetValueAtTime(state.fog, time, &fog), "read film fog");
    require(parameters->paramGetValueAtTime(state.halationStrength, time, &halationStrength), "read halation strength");
    require(parameters->paramGetValueAtTime(state.halationSize, time, &halationSize), "read halation size");
    require(parameters->paramGetValueAtTime(state.grainStrength, time, &grainStrength), "read grain strength");
    require(parameters->paramGetValueAtTime(state.grainSize, time, &grainSize), "read grain size");
    require(parameters->paramGetValueAtTime(state.grainSeed, time, &grainSeed), "read grain seed");
    require(parameters->paramGetValueAtTime(state.encoding, time, &encoding), "read film encoding");
    if (state.positivePreview) require(parameters->paramGetValueAtTime(state.positivePreview, time, &preview), "read positive preview");
    if (!std::isfinite(time) || !std::isfinite(exposure) || !std::isfinite(contrast)
        || !std::isfinite(shadow) || !std::isfinite(highlight) || !std::isfinite(fog)
        || !std::isfinite(grainStrength) || !std::isfinite(grainSize)
        || !std::isfinite(halationStrength) || !std::isfinite(halationSize)
        || exposure < -4 || exposure > 4 || contrast < 0.2 || contrast > 3
        || shadow < 0 || shadow > 5 || highlight < 0 || highlight > 1 || fog < 0 || fog > 1
        || grainStrength < 0 || grainStrength > 0.2 || grainSize < 0.1 || grainSize > 16
        || halationStrength < 0 || halationStrength > 1 || halationSize < 0.01 || halationSize > 5
        || encoding < 0 || encoding > 2 || preview < 0 || preview > 1) {
        throw Failure(kOfxStatErrValue, "Invalid film parameter value");
    }
    silentfilm::EmulsionParameters value;
    value.stage = state.kind == EffectKind::Negative ? silentfilm::FilmStage::Negative : silentfilm::FilmStage::Print;
    value.transfer = static_cast<silentfilm::Transfer>(encoding);
    value.exposureEV = static_cast<float>(exposure);
    value.contrast = static_cast<float>(contrast);
    value.shadowSoftness = static_cast<float>(std::min(shadow, 1.0));
    value.shadowCurveScale = static_cast<float>(std::max(shadow, 1.0));
    value.highlightSoftness = static_cast<float>(highlight);
    value.fog = static_cast<float>(fog);
    value.halationStrength = static_cast<float>(halationStrength);
    value.halationSize = static_cast<float>(halationSize);
    value.grainStrength = static_cast<float>(grainStrength);
    value.grainSize = static_cast<float>(grainSize);
    value.grainSeed = static_cast<std::uint32_t>(grainSeed)
        ^ (state.kind == EffectKind::Negative ? 0x4e454741u : 0x5052494eu);
    double range[2]{};
    sourceFrameRange(state.source, range);
    value.grainFrame = static_cast<std::uint32_t>(std::floor(std::clamp(time - range[0], 0.0, 4294967294.0) + 1e-9));
    value.previewPositive = preview != 0;
    return value;
}

/** Read the host clip frame rate. */
double clipFrameRate(OfxImageClipHandle clip);

/** Evaluate static camera-lens shading controls at the requested time. */
silentfilm::CameraOpticsParameters cameraSettingsAt(Instance& state, double time, std::int64_t captureFrame)
{
    double strength = 0, softness = 0.15, start = 0.6, end = 1.0;
    require(parameters->paramGetValueAtTime(state.vignetteStrength, time, &strength), "read camera vignette strength");
    require(parameters->paramGetValueAtTime(state.edgeSoftness, time, &softness), "read camera edge softness");
    require(parameters->paramGetValueAtTime(state.edgeStart, time, &start), "read camera vignette start");
    require(parameters->paramGetValueAtTime(state.edgeEnd, time, &end), "read camera vignette end");
    if (!std::isfinite(time) || !std::isfinite(strength) || !std::isfinite(softness)
        || !std::isfinite(start) || !std::isfinite(end)
        || strength < 0 || strength > 1 || softness < 0 || softness > 1
        || start < 0 || start > 4 || end < 0.01 || end > 4
        || end <= start) {
        throw Failure(kOfxStatErrValue, "Invalid camera optics parameter value");
    }
    silentfilm::CameraOpticsParameters value;
    value.vignetteStrength = static_cast<float>(strength);
    value.edgeSoftness = static_cast<float>(softness);
    value.edgeStart = static_cast<float>(start);
    value.edgeEnd = static_cast<float>(end);
    double globalSoftness = 0, drift = 0;
    int seed = 1925;
    require(parameters->paramGetValueAtTime(state.cameraSoftness, time, &globalSoftness), "read camera softness");
    require(parameters->paramGetValueAtTime(state.focusDrift, time, &drift), "read camera focus drift");
    require(parameters->paramGetValueAtTime(state.focusSeed, time, &seed), "read camera focus seed");
    if (!std::isfinite(globalSoftness) || !std::isfinite(drift)
        || globalSoftness < 0 || globalSoftness > 1 || drift < 0 || drift > 0.5) {
        throw Failure(kOfxStatErrValue, "Invalid camera focus parameter value");
    }
    value.softness = static_cast<float>(globalSoftness);
    value.focusDrift = static_cast<float>(drift);
    if (drift > 0 && state.kind != EffectKind::CameraOptics)
        value.focusSignal = silentfilm::cameraFocusSignal(captureFrame, static_cast<std::uint32_t>(seed));

    return value;
}

/** Adapt the core's row-level cancellation callback to the OFX host suite. */
bool cameraAbort(void* context)
{
    return effects->abort(static_cast<OfxImageEffectHandle>(context)) != 0;
}

/** Read the action time without truncating sub-frame requests. */
double actionTime(OfxPropertySetHandle arguments)
{
    double time = 0;
    require(properties->propGetDouble(arguments, kOfxPropTime, 0, &time), "read action time");
    return time;
}

/** Clamp temporal requests to the source range advertised by the host. */
double sourceTime(OfxImageClipHandle clip, double requested)
{
    OfxPropertySetHandle clipProperties = nullptr;
    require(effects->clipGetPropertySet(clip, &clipProperties), "source properties");
    double range[2] = {};
    require(properties->propGetDoubleN(clipProperties, kOfxImageEffectPropFrameRange, 2, range), "source frame range");
    if (std::isnan(range[0]) || std::isnan(range[1]) || range[0] > range[1]) {
        throw Failure(kOfxStatErrValue, "Invalid source frame range");
    }
    return std::clamp(requested, range[0], range[1]);
}

struct CameraMechanics {
    silentfilm::HandCrankSettings crank;
    double shutterFraction = 0.65;
    double motionBlurStrength = 0.8;
    double exposureVariation = 1.0;
    int exposureSamples = 5;
    float shake = 0.25f;
    int playbackMode = 0;
};

CameraMechanics cameraMechanicsAt(Instance& state, double time)
{
    CameraMechanics value;
    require(parameters->paramGetValueAtTime(state.playbackMode, time, &value.playbackMode), "read camera playback mode");
    double minFps = 0, maxFps = 0, variation = 0, irregularity = 0;
    int seed = 1925, samples = 5;
    require(parameters->paramGetValueAtTime(state.captureFpsMin, time, &minFps), "read minimum camera rate");
    require(parameters->paramGetValueAtTime(state.captureFpsMax, time, &maxFps), "read maximum camera rate");
    require(parameters->paramGetValueAtTime(state.crankVariation, time, &variation), "read crank variation");
    require(parameters->paramGetValueAtTime(state.crankIrregularity, time, &irregularity), "read crank irregularity");
    require(parameters->paramGetValueAtTime(state.shutterFraction, time, &value.shutterFraction), "read shutter fraction");
    require(parameters->paramGetValueAtTime(state.motionBlurStrength, time, &value.motionBlurStrength), "read motion blur strength");
    require(parameters->paramGetValueAtTime(state.exposureVariation, time, &value.exposureVariation), "read exposure coupling");
    require(parameters->paramGetValueAtTime(state.exposureSamples, time, &samples), "read exposure samples");
    double shake = 0;
    require(parameters->paramGetValueAtTime(state.cameraShake, time, &shake), "read camera shake");
    require(parameters->paramGetValueAtTime(state.cameraSeed, time, &seed), "read camera seed");
    if (!std::isfinite(minFps) || !std::isfinite(maxFps) || !std::isfinite(variation)
        || !std::isfinite(irregularity) || !std::isfinite(value.shutterFraction)
        || !std::isfinite(value.motionBlurStrength)
        || !std::isfinite(value.exposureVariation) || !std::isfinite(shake)
        || minFps < 8 || maxFps > 36 || minFps > maxFps || variation < 0 || variation > 1
        || irregularity < 0 || irregularity > 1 || value.shutterFraction < 0 || value.shutterFraction > 1.5
        || value.motionBlurStrength < 0 || value.motionBlurStrength > 2
        || value.exposureVariation < 0 || value.exposureVariation > 1 || samples < 1 || samples > 9
        || shake < 0 || shake > 1 || value.playbackMode < 0 || value.playbackMode > 1) {
        throw Failure(kOfxStatErrValue, "Invalid hand-cranked camera parameter value");
    }
    value.crank.minimumFps = static_cast<float>(minFps);
    value.crank.maximumFps = static_cast<float>(maxFps);
    value.crank.rateVariation = static_cast<float>(variation);
    value.crank.irregularity = static_cast<float>(irregularity);
    value.crank.seed = static_cast<std::uint32_t>(seed);
    value.exposureSamples = samples;
    value.shake = static_cast<float>(shake);
    return value;
}

void sourceFrameRange(OfxImageClipHandle clip, double range[2])
{
    OfxPropertySetHandle clipProperties = nullptr;
    require(effects->clipGetPropertySet(clip, &clipProperties), "source properties");
    require(properties->propGetDoubleN(clipProperties, kOfxImageEffectPropFrameRange, 2, range), "source frame range");
    if (!std::isfinite(range[0]) || !std::isfinite(range[1]) || range[0] > range[1])
        throw Failure(kOfxStatErrValue, "Invalid source frame range");
}

/** Read the host clip frame rate for reproducible frame-based temporal effects. */
double clipFrameRate(OfxImageClipHandle clip)
{
    OfxPropertySetHandle clipProperties = nullptr;
    require(effects->clipGetPropertySet(clip, &clipProperties), "clip properties");
    double rate = 24.0;
    const auto status = properties->propGetDouble(clipProperties, kOfxImageEffectPropFrameRate, 0, &rate);
    optionalProperty(status, "clip frame rate");
    if (status != kOfxStatOK || !std::isfinite(rate) || rate <= 0) return 24.0;
    return rate;
}

class Image {
    OfxPropertySetHandle handle_ = nullptr;
    unsigned char* data_ = nullptr;
    int bounds_[4] = {};
    int rowBytes_ = 0;

public:
    /** Acquire a float RGBA image and release it if validation fails. */
    Image(OfxImageClipHandle clip, double time)
    {
        {
            ActionTimer timer("Host image", "clipGetImage");
            require(effects->clipGetImage(clip, time, nullptr, &handle_), "fetch image");
        }
        if (!handle_) {
            throw Failure(kOfxStatFailed, "Host returned an empty image handle");
        }
        try {
            void* data = nullptr;
            char* depth = nullptr;
            char* components = nullptr;
            require(properties->propGetPointer(handle_, kOfxImagePropData, 0, &data), "image data");
            require(properties->propGetIntN(handle_, kOfxImagePropBounds, 4, bounds_), "image bounds");
            require(properties->propGetInt(handle_, kOfxImagePropRowBytes, 0, &rowBytes_), "image row bytes");
            require(properties->propGetString(handle_, kOfxImageEffectPropPixelDepth, 0, &depth), "image depth");
            require(properties->propGetString(handle_, kOfxImageEffectPropComponents, 0, &components), "image components");
            if (!data || !depth || !components || std::strcmp(depth, kOfxBitDepthFloat)
                || std::strcmp(components, kOfxImageComponentRGBA)) {
                throw Failure(kOfxStatErrUnsupported, "SilentFilm requires float RGBA images");
            }
            if (bounds_[2] < bounds_[0] || bounds_[3] < bounds_[1]) {
                throw Failure(kOfxStatErrValue, "Invalid image bounds");
            }
            const auto width = static_cast<std::ptrdiff_t>(bounds_[2]) - bounds_[0];
            const auto stride = static_cast<std::ptrdiff_t>(rowBytes_);
            if ((stride < 0 ? -stride : stride) < width * static_cast<std::ptrdiff_t>(sizeof(silentfilm::Pixel))) {
                throw Failure(kOfxStatErrValue, "Image row stride is too small");
            }
            data_ = static_cast<unsigned char*>(data);
        } catch (...) {
            effects->clipReleaseImage(handle_);
            handle_ = nullptr;
            throw;
        }
    }

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    /** Release the host image before leaving the OFX action. */
    ~Image()
    {
        if (handle_) effects->clipReleaseImage(handle_);
    }

    /** Describe the buffer without dereferencing a possible CUDA device pointer. */
    silentfilm::ImageView view() const
    {
        return {data_, rowBytes_, bounds_[0], bounds_[1], bounds_[2], bounds_[3]};
    }

    /** Read the mandatory host alpha representation before nonlinear film processing. */
    bool premultiplied() const
    {
        char* value = nullptr;
        require(properties->propGetString(handle_, kOfxImageEffectPropPreMultiplication, 0, &value), "image premultiplication");
        if (value && std::strcmp(value, kOfxImagePreMultiplied) == 0) return true;
        if (value && (std::strcmp(value, kOfxImageUnPreMultiplied) == 0 || std::strcmp(value, kOfxImageOpaque) == 0)) return false;
        throw Failure(kOfxStatErrUnsupported, "Unknown image alpha representation");
    }

    /** Locate a pixel using its signed row stride and absolute image bounds. */
    unsigned char* address(int x, int y) const
    {
        if (x < bounds_[0] || y < bounds_[1] || x >= bounds_[2] || y >= bounds_[3]) return nullptr;
        return data_ + (static_cast<std::ptrdiff_t>(y) - bounds_[1]) * rowBytes_
            + (static_cast<std::ptrdiff_t>(x) - bounds_[0]) * sizeof(silentfilm::Pixel);
    }

    /** Return transparent black outside the fetched source bounds. */
    silentfilm::Pixel read(int x, int y) const
    {
        silentfilm::Pixel value{};
        if (const auto* pixel = address(x, y)) std::memcpy(&value, pixel, sizeof(value));
        return value;
    }
};

/** Obtain the mandatory host suites and negotiate temporal access. */
OfxStatus load()
{
    if (!host || !host->fetchSuite) return kOfxStatErrMissingHostFeature;
    properties = static_cast<const OfxPropertySuiteV1*>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1*>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    parameters = static_cast<const OfxParameterSuiteV1*>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
    messages = static_cast<const OfxMessageSuiteV1*>(host->fetchSuite(host->host, kOfxMessageSuite, 1));
    if (!properties || !effects || !parameters) return kOfxStatErrMissingHostFeature;
    int supported = 0;
    temporalAccess = properties->propGetInt(host->host, kOfxImageEffectPropTemporalClipAccess, 0, &supported) == kOfxStatOK && supported != 0;
    return kOfxStatOK;
}

/** Describe implemented CPU capabilities and optional CUDA support. */
OfxStatus describe(OfxImageEffectHandle effect, EffectKind kind)
{
    const auto props = effectProperties(effect);
    require(properties->propSetString(props, kOfxPropLabel, 0, effectName(kind)), "plugin label");
    optionalProperty(properties->propSetString(props, kOfxPropShortLabel, 0, effectName(kind)), "plugin short label");
    optionalProperty(properties->propSetString(props, kOfxPropLongLabel, 0, effectName(kind)), "plugin long label");
    require(properties->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "ALEXZ/SilentFilm"), "plugin group");
    const char* description = kind == EffectKind::Probe
        ? "Диагностический эффект для проверки усиления и доступа к соседним кадрам на CPU/CUDA. Это не плёночный вид."
        : kind == EffectKind::CrankExposure
        ? "Экспозиция ручки по точной карте выходных кадров Cadence. Выбери exposure.sfexposure результата Exposure via OFX, затем регулируй Blend без пересчёта каденса. Вход sRGB, до Camera Optics/Negative; CPU/CUDA."
        : kind == EffectKind::CameraOptics
        ? "Оптика подготовленных кадров: виньетка и краевой расфокус с общей маской edge_start/end, случайное гуляние фокуса и глобальная мягкость. Экспозиция и движение ручки рассчитываются в приложении Cadence."
        : kind == EffectKind::Camera
        ? "Чёрно-белая ручная съёмка: неравномерный каденс, связанная с вращением ручки выдержка и экспозиция, толчки камеры и гуляющий фокус."
        : kind == EffectKind::Projector
        ? "Проекция позитива: независимое дрожание плёнки в кадровом окне, мерцание и дыхание лампы, спектральный glow в линейном свете. CPU/CUDA."
        : kind == EffectKind::Defects
        ? "Самостоятельная пыль, царапины, пятна или неравномерность проявки на выбранном этапе плёнки."
        : "Калиброванная монохромная плёночная эмульсия с явным SDR-кодированием. Выход Negative — физическое пропускание плёнки.";
    optionalProperty(properties->propSetString(props, kOfxPropPluginDescription, 0, description), "plugin description");
    require(properties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter), "filter context");
    require(properties->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral), "general context");
    require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat), "float pixels");
    require(properties->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe), "thread safety");
    require(properties->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0), "multiple instances");
    require(properties->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0), "frame threading");
    require(properties->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0), "image tiles");
    require(properties->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 0), "multi resolution");
    require(properties->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0), "multiple depths");
    require(properties->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0), "multiple pixel aspects");
    require(properties->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0,
        (kind == EffectKind::Probe || kind == EffectKind::Camera) && temporalAccess ? 1 : 0), "temporal capability");
#ifdef SILENTFILM_ENABLE_CUDA
    if (kind == EffectKind::Probe || kind == EffectKind::Camera || kind == EffectKind::CameraOptics || kind == EffectKind::Projector
        || kind == EffectKind::Negative || kind == EffectKind::Print || kind == EffectKind::Defects) {
        optionalProperty(properties->propSetString(props, kOfxImageEffectPropCudaRenderSupported, 0, "true"), "CUDA capability");
        optionalProperty(properties->propSetString(props, kOfxImageEffectPropCudaStreamSupported, 0, "true"), "CUDA stream capability");
    }
#endif
    return kOfxStatOK;
}

/** Define one animated scalar diagnostic control. */
void defineDouble(OfxParamSetHandle set, const char* name, const char* label, const char* hint, double low, double high, double defaultValue = 0, const char* parent = nullptr)
{
    OfxPropertySetHandle props = nullptr;
    require(parameters->paramDefine(set, kOfxParamTypeDouble, name, &props), "define double");
    require(properties->propSetString(props, kOfxPropLabel, 0, label), "parameter label");
    optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0, hint), "parameter hint");
    require(properties->propSetDouble(props, kOfxParamPropDefault, 0, defaultValue), "parameter default");
    require(properties->propSetDouble(props, kOfxParamPropMin, 0, low), "parameter minimum");
    require(properties->propSetDouble(props, kOfxParamPropMax, 0, high), "parameter maximum");
    require(properties->propSetDouble(props, kOfxParamPropDisplayMin, 0, low), "parameter display minimum");
    require(properties->propSetDouble(props, kOfxParamPropDisplayMax, 0, high), "parameter display maximum");
    require(properties->propSetDouble(props, kOfxParamPropIncrement, 0, 0.01), "parameter increment");
    if (parent) require(properties->propSetString(props, kOfxParamPropParent, 0, parent), "parameter parent");
}

/** Declare the control page before its children and publish its preferred order. */
OfxPropertySetHandle defineControlsPage(OfxParamSetHandle set)
{
    OfxPropertySetHandle page = nullptr, setProperties = nullptr;
    require(parameters->paramDefine(set, kOfxParamTypePage, "Controls", &page), "define controls page");
    require(properties->propSetString(page, kOfxPropLabel, 0, "Controls"), "controls page label");
    require(parameters->paramSetGetPropertySet(set, &setProperties), "parameter set properties");
    optionalProperty(properties->propSetString(setProperties, kOfxPluginPropParamPageOrder, 0, "Controls"), "controls page order");
    return page;
}

/** Put standalone transfer overrides in a collapsed group while retaining saved parameter IDs. */
void defineAdvancedColor(OfxParamSetHandle set)
{
    OfxPropertySetHandle group = nullptr;
    require(parameters->paramDefine(set, kOfxParamTypeGroup, "advanced_color", &group), "define advanced color group");
    require(properties->propSetString(group, kOfxPropLabel, 0, "Advanced — Color Encoding"), "advanced color label");
    require(properties->propSetInt(group, kOfxParamPropGroupOpen, 0, 0), "advanced color collapsed");
    optionalProperty(properties->propSetString(group, kOfxParamPropHint, 0,
        "Обычная цепочка Comfy использует sRGB / Rec.709 на входе и после Print. Между Negative и Print идут физические данные плёнки. Ручная гамма нужна для отдельных нод или прежних проектов; она не преобразует автоматически рабочее пространство Resolve. Comfy color only меняет только гамму, сохраняя настройки эффекта."), "advanced color hint");
}

/** Disable the transfer override when this defect instance processes physical film data. */
void updateDefectColorUi(Instance& state, double time)
{
    if (state.kind != EffectKind::Defects) return;
    int material = 0;
    require(parameters->paramGetValueAtTime(state.defectMaterial, time, &material), "defect color UI material");
    const bool physical = silentfilm::color::filmTransmission(static_cast<silentfilm::DefectMaterial>(material));
    OfxPropertySetHandle props = nullptr;
    require(parameters->paramGetPropertySet(state.encoding, &props), "defect color UI properties");
    optionalProperty(properties->propSetInt(props, kOfxParamPropEnabled, 0, physical ? 0 : 1), "defect encoding enabled");
    optionalProperty(properties->propSetString(props, kOfxPropLabel, 0,
        physical ? "Film transmission — no gamma" : "Image Encoding"), "defect encoding role label");
    int pattern = 0;
    double wander = 0, variation = 0;
    require(parameters->paramGetValueAtTime(state.defectPattern, time, &pattern), "defect pattern UI value");
    require(parameters->paramGetValueAtTime(state.defectScratchWander, time, &wander), "defect wander UI value");
    require(parameters->paramGetValueAtTime(state.defectAmountVariation, time, &variation), "defect amount variation UI value");
    const bool particles = pattern != static_cast<int>(silentfilm::DefectPattern::Development);
    const bool scratches = pattern == static_cast<int>(silentfilm::DefectPattern::Scratches);
    const auto enabled = [](OfxParamHandle handle, bool value) {
        OfxPropertySetHandle prop = nullptr;
        require(parameters->paramGetPropertySet(handle, &prop), "defect shape UI properties");
        optionalProperty(properties->propSetInt(prop, kOfxParamPropEnabled, 0, value ? 1 : 0), "defect shape control enabled");
    };
    enabled(state.defectSizeSpread, particles && !scratches);
    enabled(state.defectThicknessSpread, scratches);
    enabled(state.defectEdgeBias, particles);
    enabled(state.defectScratchWander, scratches);
    enabled(state.defectScratchWanderTime, scratches && wander > 0);
    enabled(state.defectAmount, particles);
    enabled(state.defectAmountVariation, particles);
    enabled(state.defectAmountDriftTime, particles && variation > 0);
}

/** Map effect identity to the explicit workflow preset table. */
silentfilm::ReferenceStage referenceStage(EffectKind kind)
{
    switch (kind) {
    case EffectKind::Camera: return silentfilm::ReferenceStage::Camera;
    case EffectKind::CameraOptics: return silentfilm::ReferenceStage::CameraOptics;
    case EffectKind::Negative: return silentfilm::ReferenceStage::Negative;
    case EffectKind::Print: return silentfilm::ReferenceStage::Print;
    case EffectKind::Projector: return silentfilm::ReferenceStage::Projector;
    default: return silentfilm::ReferenceStage::Defects;
    }
}

/** Append user-triggered reference presets without changing existing project values on load. */
void defineReferenceControls(OfxParamSetHandle set, OfxPropertySetHandle page, EffectKind kind)
{
    OfxPropertySetHandle choice = nullptr, button = nullptr;
    require(parameters->paramDefine(set, kOfxParamTypeChoice, "reference_preset", &choice), "define reference preset");
    require(properties->propSetString(choice, kOfxPropLabel, 0, "Reference Preset"), "reference preset label");
    require(properties->propSetInt(choice, kOfxParamPropDefault, 0, 0), "reference preset default");
    optionalProperty(properties->propSetInt(choice, kOfxParamPropAnimates, 0, 0), "reference preset animation");
    const auto presets = silentfilm::referencePresets(referenceStage(kind));
    for (std::size_t i = 0; i < presets.size(); ++i)
        require(properties->propSetString(choice, kOfxParamPropChoiceOption, static_cast<int>(i), presets[i].label), "reference preset option");
    optionalProperty(properties->propSetString(choice, kOfxParamPropHint, 0,
        kind == EffectKind::Defects
            ? "Доступные параметры отдельных нод with_defects. Mapped означает частичный перенос: тип повреждения, мягкость и движение ещё не эквивалентны Comfy. Для точного сравнения зерна сначала отключи Defects."
            : "Настройки из Видео-в-Хронику-по-этапам_with_defects.json. Выбор списка сам по себе ничего не меняет: нажми Apply Reference Preset. Для чистого A/B используй новую ноду без анимации. Print в workflow имеет grain_strength=0; Grain only включает собственное зерно позитива отдельно."), "reference preset hint");
    require(parameters->paramDefine(set, kOfxParamTypePushButton, "apply_reference_preset", &button), "define apply reference preset");
    require(properties->propSetString(button, kOfxPropLabel, 0, "Apply Reference Preset"), "apply reference label");
    int children = 0;
    require(properties->propGetDimension(page, kOfxParamPropPageChild, &children), "reference page dimension");
    require(properties->propSetString(page, kOfxParamPropPageChild, children, "reference_preset"), "reference page choice");
    require(properties->propSetString(page, kOfxParamPropPageChild, children + 1, "apply_reference_preset"), "reference page button");
}

/** Apply a preset on its button action and group all parameter edits into one undo entry. */
OfxStatus changed(OfxImageEffectHandle effect, OfxPropertySetHandle in)
{
    char* name = nullptr;
    require(properties->propGetString(in, kOfxPropName, 0, &name), "changed parameter name");
    if (name && instance(effect).kind == EffectKind::CrankExposure
        && (std::strcmp(name, "exposure_map") == 0 || std::strcmp(name, "reload_map") == 0)) {
        instance(effect).exposureMaps.clear();
        return kOfxStatOK;
    }
    if (name && instance(effect).kind == EffectKind::Defects
        && (std::strcmp(name, "material") == 0 || std::strcmp(name, "pattern") == 0
            || std::strcmp(name, "scratch_wander") == 0 || std::strcmp(name, "amount_variation") == 0)) {
        updateDefectColorUi(instance(effect), actionTime(in));
        return kOfxStatOK;
    }
    if (!name || std::strcmp(name, "apply_reference_preset") != 0) return kOfxStatReplyDefault;
    OfxParamSetHandle set = nullptr;
    require(effects->getParamSet(effect, &set), "preset parameter set");
    OfxParamHandle choice = nullptr;
    require(parameters->paramGetHandle(set, "reference_preset", &choice, nullptr), "reference preset handle");
    int index = 0;
    require(parameters->paramGetValue(choice, &index), "reference preset selection");
    const auto presets = silentfilm::referencePresets(referenceStage(instance(effect).kind));
    if (index < 0 || static_cast<std::size_t>(index) >= presets.size()) return kOfxStatErrValue;
    require(parameters->paramEditBegin(set, "Apply SilentFilm reference preset"), "begin reference preset edit");
    try {
        for (const auto& value : presets[static_cast<std::size_t>(index)].values) {
            OfxParamHandle parameter = nullptr;
            require(parameters->paramGetHandle(set, value.id, &parameter, nullptr), "preset target handle");
            require(value.integer ? parameters->paramSetValue(parameter, static_cast<int>(value.value))
                : parameters->paramSetValue(parameter, value.value), "apply reference value");
        }
    } catch (...) { parameters->paramEditEnd(set); throw; }
    require(parameters->paramEditEnd(set), "end reference preset edit");
    if (instance(effect).kind == EffectKind::Defects) updateDefectColorUi(instance(effect), actionTime(in));
    return kOfxStatOK;
}

/** Define readable material controls and an explicit input/output transfer. */
void defineFilmControls(OfxParamSetHandle set, EffectKind kind)
{
    const auto page = defineControlsPage(set);
    const bool negative = kind == EffectKind::Negative;
    OfxPropertySetHandle props = nullptr;
    defineDouble(set, "exposure_ev", negative ? "Negative Exposure (EV)" : "Print Exposure (EV)",
        negative ? "Нейтральное значение — 0 EV. Меняйте по 0,1–0,25 EV. Плюс увеличивает экспозицию негатива: сам негатив становится плотнее и темнее, а отпечаток после Print — светлее. Минус действует наоборот."
                 : "Нейтральное значение — 0 EV. Меняйте по 0,1–0,25 EV. Плюс увеличивает экспозицию позитивной плёнки и затемняет отпечаток; минус уменьшает плотность и осветляет его. Это экспозиция печати, не яркость проектора.", -4, 4);
    defineDouble(set, "contrast", negative ? "Development Contrast" : "Print Contrast",
        "Значение 1 — нейтральная проявка. Ниже 1 контраст мягче, выше 1 — жёстче относительно калиброванного среднего серого. Для сравнения меняйте контраст только на одном этапе за раз; контрасты негатива и печати действуют совместно.", 0.2, 3, 1);
    defineDouble(set, "shadow_softness", "Shadow Compression",
        "Сжимает тональные различия только в глубоких тенях, не размывает изображение. 0 — выключено; 0–1 — исходная шкала; значения выше 1 сильнее и шире воздействуют на тени, максимум 5. Для проверки сравните 0, 1 и 5 на тёмной шкале. Правильный Input Encoding важен для Negative.", 0, 5);
    defineDouble(set, "highlight_softness", "Highlight Compression",
        "Смягчает тональную характеристику в яркой области материала; 0 — выключено, 1 — максимальное значение. Не восстанавливает детали, уже срезанные до входа эффекта, и не размывает изображение. Начните с 0, затем попробуйте 0,1–0,3.", 0, 1);
    defineDouble(set, "fog_strength", "Base Fog",
        negative ? "Дополнительная базовая плотность негатива: 0 — чистая основа, 1 добавляет 0,25 единицы плотности. Через Print повышает яркость позитива. Не является подъёмом чёрного; начните с 0."
                 : "Дополнительная базовая плотность позитивной плёнки: 0 — чистая основа, 1 добавляет 0,25 единицы плотности и затемняет отпечаток. Не заменяет экспозицию или контраст; начните с 0.", 0, 1);
    defineDouble(set, "halation_strength", "Halation Strength",
        "Возвратный свет внутри эмульсии до проявки. 0 выключает; начните с 0,03–0,15. Для проверки используйте точечный яркий источник на тёмном поле. Это ореол плёнки, не glow проектора.", 0, 1);
    defineDouble(set, "halation_size", "Halation Size",
        "Sigma возвратного света в процентах ширины кадра: 0,15 даёт около 3 px при 1920 px. Ореол круглый и рассчитан в линейном/физическом сигнале. Размер не зависит от render window.", 0.01, 5, 0.15);
    defineDouble(set, "grain_strength", "Emulsion Grain",
        negative ? "Кластерное зерно в оптической плотности негатива, как в Comfy: четыре нормализованных масштаба. В рабочем workflow сила 0,01 и размер 2. Через Print это зерно остаётся в позитиве, даже если собственное зерно Print выключено. 0 выключает. Не путайте с отдельными пятнами проявки и повреждениями."
                 : "Собственное кластерное зерно позитивной эмульсии, добавляемое в плотность после печати. В рабочем Comfy workflow оно выключено (0), зерно негатива уже отпечатано. Пресет Grain only включает 0,01 для отдельного сравнения. Seed позитивной эмульсии независим от негатива.", 0, 0.2);
    defineDouble(set, "grain_size", "Grain Size",
        "Масштаб структуры относительно ширины кадра 1920 px. Значение 2 — стартовое; 1 даёт более мелкую, 3 — более крупную фактуру. Масштаб меняется вместе с разрешением и proxy; это не диаметр отдельного зерна.", 0.1, 16, 2);
    require(parameters->paramDefine(set, kOfxParamTypeInteger, "grain_seed", &props), "define grain seed");
    require(properties->propSetString(props, kOfxPropLabel, 0, "Grain Seed"), "grain seed label");
    optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0, "Фиксированный seed воспроизводит тот же рисунок при повторном render того же времени и координат. Negative и Print используют независимые потоки зерна."), "grain seed hint");
    require(properties->propSetInt(props, kOfxParamPropDefault, 0, 1925), "grain seed default");
    require(properties->propSetInt(props, kOfxParamPropMin, 0, 0), "grain seed minimum");
    require(properties->propSetInt(props, kOfxParamPropMax, 0, 2147483647), "grain seed maximum");
    require(properties->propSetInt(props, kOfxParamPropDisplayMin, 0, 0), "grain seed display minimum");
    require(properties->propSetInt(props, kOfxParamPropDisplayMax, 0, 2147483647), "grain seed display maximum");
    defineAdvancedColor(set);
    require(parameters->paramDefine(set, kOfxParamTypeChoice, "encoding", &props), "define film encoding");
    require(properties->propSetString(props, kOfxParamPropParent, 0, "advanced_color"), "film encoding group");
    require(properties->propSetString(props, kOfxPropLabel, 0, negative ? "Input Encoding" : "Output Encoding"), "encoding label");
    optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0,
        negative ? "Для обычной цепочки оставь sRGB / Rec.709, как у Camera Optics и Comfy. Входной CST должен действительно привести изображение к этому сигналу; выбор пункта сам по себе не преобразует пространство Resolve. Linear/Gamma 2.4 нужны только при самостоятельном использовании Negative с соответствующим входом."
                 : "Обычный выход цепочки — sRGB / Rec.709, как ожидают Positive Defects и Projector. На входе Print физическое пропускание: оно не декодируется этой настройкой. CST обратно в пространство таймлайна ставится после Projector. Ручной override сохраняется для отдельных нод и старых проектов."), "encoding hint");
    require(properties->propSetInt(props, kOfxParamPropDefault, 0, static_cast<int>(silentfilm::color::pipelineTransfer)), "encoding default");
    require(properties->propSetString(props, kOfxParamPropChoiceOption, 0, "Linear Rec.709"), "linear encoding");
    require(properties->propSetString(props, kOfxParamPropChoiceOption, 1, "sRGB"), "sRGB encoding");
    require(properties->propSetString(props, kOfxParamPropChoiceOption, 2, "Rec.709 Gamma 2.4"), "gamma encoding");
    if (negative) {
        require(parameters->paramDefine(set, kOfxParamTypeBoolean, "preview_positive", &props), "define positive preview");
        require(properties->propSetString(props, kOfxPropLabel, 0, "View Positive (preview only)"), "preview label");
        optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0, "Показывает пропускание негатива через фиксированную нейтральную печать. Для настройки временно отключите Print. Перед подачей сигнала в SilentFilm Print выключите этот режим: preview — уже позитив, а не негатив."), "preview hint");
        require(properties->propSetInt(props, kOfxParamPropDefault, 0, 0), "preview default");
    }
    props = page;
    int index = 0;
    for (const char* name : {"exposure_ev", "contrast", "shadow_softness", "highlight_softness", "fog_strength", "halation_strength", "halation_size", "grain_strength", "grain_size", "grain_seed", "encoding"}) {
        require(properties->propSetString(props, kOfxParamPropPageChild, index++, name), "film page control");
    }
    if (negative) require(properties->propSetString(props, kOfxParamPropPageChild, index, "preview_positive"), "film page preview");
    defineReferenceControls(set, page, kind);
}

/** Define the first stateless camera-optics controls independently from cadence. */
void defineCameraControls(OfxParamSetHandle set, bool opticsOnly = false)
{
    const auto page = defineControlsPage(set);
    if (opticsOnly) {
        for (const auto* name : {"lens_edges", "focus_optics"}) {
            OfxPropertySetHandle group = nullptr;
            require(parameters->paramDefine(set, kOfxParamTypeGroup, name, &group), "define optics group");
            require(properties->propSetString(group, kOfxPropLabel, 0,
                std::strcmp(name, "lens_edges") == 0 ? "Vignette and Edge Blur" : "Focus and Softness"), "optics group label");
            require(properties->propSetInt(group, kOfxParamPropGroupOpen, 0, 1), "optics group open");
        }
    }
    if (!opticsOnly) {
        OfxPropertySetHandle playbackProps = nullptr;
        require(parameters->paramDefine(set, kOfxParamTypeChoice, "playback_mode", &playbackProps), "define camera playback mode");
        require(properties->propSetString(playbackProps, kOfxPropLabel, 0, "Playback Mode"), "camera playback label");
        optionalProperty(properties->propSetString(playbackProps, kOfxParamPropHint, 0,
            "Проекция снятых кадров — движение ускоряется при медленном вращении ручки и замедляется при быстром. Снятые кадры показываются равномерно; один общий коэффициент сохраняет длительность клипа. При 10–30 FPS и Variation=1 эффект выраженный. Сохранение исходного времени — прежний режим: меняются повторы/смаз, средняя скорость действия сохраняется."), "camera playback hint");
        require(properties->propSetString(playbackProps, kOfxParamPropChoiceOption, 0, "Проекция снятых кадров"), "camera projected mode");
        require(properties->propSetString(playbackProps, kOfxParamPropChoiceOption, 1, "Сохранение исходного времени"), "camera real-time mode");
        require(properties->propSetInt(playbackProps, kOfxParamPropDefault, 0, 0), "camera playback default");
    }
    defineDouble(set, "vignette_strength", opticsOnly ? "vignette_strength" : "Vignette Strength",
        "Затемняет изображение по краям до заданной доли. 0 — выключено; начните с 0,10–0,15, значение 0,6 уже выраженное. Радиус и ширину перехода задают Edge Start и Edge End. Альфа не изменяется.",
        0, 1, 0.12, opticsOnly ? "lens_edges" : nullptr);
    defineDouble(set, "edge_start", opticsOnly ? "edge_start" : "Edge Start",
        "Радиус начала виньетки и краевого расфокуса в единицах половины ширины. До этого радиуса центр сохраняется. Старт 0,6; edge_end должен быть больше edge_start.",
        0, 4, 0.6, opticsOnly ? "lens_edges" : nullptr);
    defineDouble(set, "edge_end", opticsOnly ? "edge_end" : "Edge End",
        "Радиус полной силы виньетки и краевого расфокуса. Разность edge_end − edge_start задаёт ширину перехода. Старт 1,0.",
        0.01, 4, 1.0, opticsOnly ? "lens_edges" : nullptr);
    defineDouble(set, "edge_softness", opticsOnly ? "edge_softness" : "Edge Softness",
        "Переменный расфокус по виньеточной маске: в центре резкость сохраняется, к краям радиус размытия растёт. 0 выключает; начните с 0,02–0,04, 0,15 уже заметно. При 1920 px значение 0,15 даёт максимальную sigma около 7,2 px. Маску задают edge_start и edge_end.", 0, 1, 0.15, opticsOnly ? "lens_edges" : nullptr);
    defineDouble(set, "camera_softness", opticsOnly ? "softness" : "Global Softness",
        "Равномерный расфокус всего кадра, включая центр. 0 выключает; 0,05–0,12 — лёгкая мягкость. Sigma = ширина / 1920 × softness × 4: при 1920 px значение 0,1 даёт 0,4 px, 0,5 даёт 2 px. Не зависит от виньетки. Гуляющий фокус добавляется поверх этой мягкости; альфа сохраняется.", 0, 1, 0.1, opticsOnly ? "focus_optics" : nullptr);
    defineDouble(set, "focus_drift", opticsOnly ? "focus_drift_strength" : "Focus Drift",
        opticsOnly
            ? "Гуляющий расфокус поверх softness: сглаженный случайный сигнал по кадрам клипа, как в Comfy. 0 выключает; 0,02–0,04 — деликатно, 0,1–0,3 — для проверки. Максимальная sigma = ширина / 1920 × сила × 8; фактическая меняется от кадра к кадру. При перемотке рисунок воспроизводится. Нормализация зависит от длины клипа; на одном кадре эффект выключен. Seed выбирает рисунок."
            : "Медленно гуляющий фокус, привязанный к кадрам ручной съёмки. 0 выключает; 0,02–0,04 — деликатное дыхание резкости, 0,1 уже заметно. Фокус держится вместе с виртуальным кадром и воспроизводится после перемотки.",
        0, 0.5, 0.03, opticsOnly ? "focus_optics" : nullptr);
    OfxPropertySetHandle focusProps = nullptr;
    require(parameters->paramDefine(set, kOfxParamTypeInteger, "focus_seed", &focusProps), "define focus seed");
    require(properties->propSetString(focusProps, kOfxPropLabel, 0, opticsOnly ? "seed" : "Focus Seed"), "focus seed label");
    if (opticsOnly) require(properties->propSetString(focusProps, kOfxParamPropParent, 0, "focus_optics"), "focus seed parent");
    optionalProperty(properties->propSetString(focusProps, kOfxParamPropHint, 0, "Seed рисунка гуляющего фокуса. При одинаковом seed и кадре фокус воспроизводится; рисунок независим от зерна."), "focus seed hint");
    require(properties->propSetInt(focusProps, kOfxParamPropDefault, 0, 1925), "focus seed default");
    require(properties->propSetInt(focusProps, kOfxParamPropMin, 0, 0), "focus seed minimum");
    require(properties->propSetInt(focusProps, kOfxParamPropMax, 0, 2147483647), "focus seed maximum");
    require(properties->propSetInt(focusProps, kOfxParamPropDisplayMin, 0, 0), "focus seed display minimum");
    require(properties->propSetInt(focusProps, kOfxParamPropDisplayMax, 0, 2147483647), "focus seed display maximum");
    if (!opticsOnly) {
        defineDouble(set, "capture_fps_min", "Capture FPS Min",
            "Нижняя скорость съёмки, когда ручка замедляется. В режиме проекции это ускоряет действие. Старт 16 кадров/с; для проверки выберите Min=10, Max=30, Variation=1. Общая длительность клипа сохраняется.", 8, 36, 16);
        defineDouble(set, "capture_fps_max", "Capture FPS Max",
            "Верхняя скорость ручной съёмки. В режиме проекции это замедляет действие. Старт 20 кадров/с. Должна быть не ниже Capture FPS Min; полный размах включается при Crank Rate Variation=1.", 8, 36, 20);
        defineDouble(set, "crank_variation", "Crank Rate Variation",
            "Размах циклической скорости внутри min/max. 0 — ровная ручка; 0,6–0,8 — заметный, но естественный ход; 1 — весь заданный диапазон. Один оборот связан с продвижением восьми кадров плёнки, как в Comfy.", 0, 1, 0.7);
        defineDouble(set, "crank_irregularity", "Crank Irregularity",
            "Небольшое неповторяющееся отклонение темпа от оборота к обороту поверх основного цикла. 0 — чистая циклическая ручка; 0,1–0,25 — лёгкая механическая неровность; 1 — выраженная. Задаётся Camera Seed.", 0, 1, 0.15);
        defineDouble(set, "shutter_fraction", "Shutter Fraction",
            "Доля периода виртуального кадра, в течение которой затвор открыт. 0,5 — половина периода; 1 — полный период. Медленнее ручка — длиннее выдержка. Для полного отключения временного смаза поставьте Motion Blur Strength=0.", 0, 1.5, 0.65);
        defineDouble(set, "motion_blur_strength", "Motion Blur Strength",
            "Масштабирует временную интеграцию и небольшое движение выборки внутри удерживаемого кадра по модели Comfy. 0 выключает смаз; 0,8 — естественный старт; 1,22 — заметный след движения. Не меняет сам диапазон скорости ручки.", 0, 2, 0.8);
        defineDouble(set, "exposure_variation", "Crank Exposure Link",
            "Связь скорости ручки с экспозицией: медленнее вращение удлиняет экспозицию и осветляет кадр, быстрее — сокращает её и затемняет. 0 — постоянная яркость; 0,3–0,5 — деликатно; 1 — полный закон выдержки. Базовая экспозиция калибруется по середине диапазона FPS.", 0, 1, 1);
        OfxPropertySetHandle captureProps = nullptr;
        require(parameters->paramDefine(set, kOfxParamTypeInteger, "exposure_samples", &captureProps), "define exposure samples");
        require(properties->propSetString(captureProps, kOfxPropLabel, 0, "Exposure Samples"), "exposure samples label");
        optionalProperty(properties->propSetString(captureProps, kOfxParamPropHint, 0, "Число временных отсчётов внутри выдержки. Больше — плавнее смаз, но медленнее render. Начните с 5; 1 оставляет один временной отсчёт."), "exposure samples hint");
        require(properties->propSetInt(captureProps, kOfxParamPropDefault, 0, 5), "exposure samples default");
        require(properties->propSetInt(captureProps, kOfxParamPropMin, 0, 1), "exposure samples minimum");
        require(properties->propSetInt(captureProps, kOfxParamPropMax, 0, 9), "exposure samples maximum");
        require(properties->propSetInt(captureProps, kOfxParamPropDisplayMin, 0, 1), "exposure samples display minimum");
        require(properties->propSetInt(captureProps, kOfxParamPropDisplayMax, 0, 9), "exposure samples display maximum");
        defineDouble(set, "camera_shake", "Crank Camera Shake",
            "Нерегулярные сдвиги и лёгкие наклоны от усилия руки, привязанные к фазе той же ручки, что задаёт каденс и выдержку. 0 выключает; 0,15–0,3 — небольшие толчки; 1 — сильные. Не путать с дрожанием проектора.", 0, 1, 0.25);
        OfxPropertySetHandle crankSeedProps = nullptr;
        require(parameters->paramDefine(set, kOfxParamTypeInteger, "camera_seed", &crankSeedProps), "define camera seed");
        require(properties->propSetString(crankSeedProps, kOfxPropLabel, 0, "Camera Seed"), "camera seed label");
        optionalProperty(properties->propSetString(crankSeedProps, kOfxParamPropHint, 0, "Общий seed ручки: фиксирует нерегулярность темпа и связанные с оборотом толчки камеры. Можно менять независимо от Focus Seed."), "camera seed hint");
        require(properties->propSetInt(crankSeedProps, kOfxParamPropDefault, 0, 1925), "camera seed default");
        require(properties->propSetInt(crankSeedProps, kOfxParamPropMin, 0, 0), "camera seed minimum");
        require(properties->propSetInt(crankSeedProps, kOfxParamPropMax, 0, 2147483647), "camera seed maximum");
        require(properties->propSetInt(crankSeedProps, kOfxParamPropDisplayMin, 0, 0), "camera seed display minimum");
        require(properties->propSetInt(crankSeedProps, kOfxParamPropDisplayMax, 0, 2147483647), "camera seed display maximum");
    }
    OfxPropertySetHandle props = page;
    const char* children[] = {"playback_mode", "capture_fps_min", "capture_fps_max", "crank_variation", "crank_irregularity", "shutter_fraction", "motion_blur_strength", "exposure_variation", "exposure_samples", "camera_shake", "camera_seed", "focus_drift", "focus_seed", "vignette_strength", "edge_softness", "camera_softness", "edge_start", "edge_end"};
    const char* opticsChildren[] = {"vignette_strength", "edge_start", "edge_end", "edge_softness", "camera_softness", "focus_drift", "focus_seed"};
    for (int index = 0; index < (opticsOnly ? 7 : 18); ++index) {
        require(properties->propSetString(props, kOfxParamPropPageChild, index,
            opticsOnly ? opticsChildren[index] : children[index]), "camera page control");
    }
    defineReferenceControls(set, page, opticsOnly ? EffectKind::CameraOptics : EffectKind::Camera);
}

/** Define shared clip contracts and the UI specific to each registered effect. */
OfxStatus describeContext(OfxImageEffectHandle effect, EffectKind kind)
{
    for (const char* name : {"Source", "Output"}) {
        OfxPropertySetHandle props = nullptr;
        require(effects->clipDefine(effect, name, &props), "define clip");
        require(properties->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA), "clip components");
        require(properties->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0), "clip tiles");
        if (std::strcmp(name, "Source") == 0) {
            require(properties->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0,
                (kind == EffectKind::Probe || kind == EffectKind::Camera) && temporalAccess ? 1 : 0), "source temporal access");
        }
    }
    OfxParamSetHandle set = nullptr;
    require(effects->getParamSet(effect, &set), "parameter set");
    if (kind == EffectKind::CrankExposure) {
        const auto page = defineControlsPage(set);
        defineDouble(set, "blend", "Blend", "Сила сохранённой экспозиции ручки. 0 — без эффекта, 1 — полный расчёт Cadence. Меняется без пересчёта каденса. Применять перед Camera Optics/Negative, только к результату Exposure via OFX; RGB ожидается в sRGB.", 0, 1, 1);
        OfxPropertySetHandle props = nullptr;
        require(parameters->paramDefine(set, kOfxParamTypeString, "exposure_map", &props), "define exposure map");
        require(properties->propSetString(props, kOfxPropLabel, 0, "Exposure Map"), "exposure map label");
        require(properties->propSetString(props, kOfxParamPropDefault, 0, ""), "exposure map default");
        require(properties->propSetString(props, kOfxParamPropStringMode, 0, kOfxParamStringIsFilePath), "exposure file picker");
        require(properties->propSetInt(props, kOfxParamPropAnimates, 0, 0), "exposure map not animated");
        optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0, "Выбери exposure.sfexposure из папки результата Cadence. Приложение показывает путь после импорта. Это точная карта выходных кадров, не новый случайный шум. Запечённая карта Bake отклоняется. Сохраняй файл вместе с клипом."), "exposure map hint");
        require(parameters->paramDefine(set, kOfxParamTypeInteger, "frame_offset", &props), "define exposure offset");
        require(properties->propSetString(props, kOfxPropLabel, 0, "Frame Offset"), "exposure offset label");
        require(properties->propSetInt(props, kOfxParamPropDefault, 0, 0), "exposure offset default");
        require(properties->propSetInt(props, kOfxParamPropMin, 0, -10000000), "exposure offset min");
        require(properties->propSetInt(props, kOfxParamPropMax, 0, 10000000), "exposure offset max");
        require(properties->propSetInt(props, kOfxParamPropDisplayMin, 0, -10000), "exposure offset display min");
        require(properties->propSetInt(props, kOfxParamPropDisplayMax, 0, 10000), "exposure offset display max");
        require(properties->propSetInt(props, kOfxParamPropAnimates, 0, 0), "exposure offset not animated");
        optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0, "Номер кадра карты = time минус начало Source Frame Range плюс Frame Offset. После подрезки, если Resolve переобнуляет время OFX, укажи число срезанных кадров. Обычное перемещение клипа не требует сдвига. Изменение скорости готового результата пока не поддержано."), "exposure offset hint");
        require(parameters->paramDefine(set, kOfxParamTypePushButton, "reload_map", &props), "define map reload");
        require(properties->propSetString(props, kOfxPropLabel, 0, "Reload Map"), "reload map label");
        const char* children[] = {"blend", "exposure_map", "frame_offset", "reload_map"};
        for (int i = 0; i < 4; ++i) require(properties->propSetString(page, kOfxParamPropPageChild, i, children[i]), "exposure page control");
        return kOfxStatOK;
    }
    if (kind == EffectKind::Camera || kind == EffectKind::CameraOptics) {
        defineCameraControls(set, kind == EffectKind::CameraOptics);
        return kOfxStatOK;
    }
    if (kind == EffectKind::Projector) {
        const auto page = defineControlsPage(set);
        defineDouble(set, "gate_weave_px", "Gate Weave (px)",
            "Дрожание плёнки в кадровом окне проектора. Сдвигает весь позитив, зерно, пыль и царапины перед glow. 0 выключает; 0,5–1 px — деликатно, 2–4 — заметно. Пиксели полного выходного кадра; proxy масштабируется. Независимо от ручки Camera; альфа движется вместе с плёнкой.", 0, 8, 0.8);
        defineDouble(set, "lamp_flicker", "Lamp Flicker",
            "Быстрое межкадровое мерцание лампы в линейном свете. Резкое изображение и glow меняют яркость вместе. 0 выключает; 0,02–0,035 — старт, 0,1 — выраженно. Это колебания потока лампы при проекции; они не связаны с ручкой Camera.", 0, 0.3, 0.025);
        defineDouble(set, "lamp_breathing", "Lamp Breathing",
            "Медленное дыхание яркости лампы поверх быстрого Flicker. 0 выключает; 0,01–0,015 — деликатно, 0,05 — заметно. Если свет слишком нестабилен, сначала выключите Breathing и настройте Flicker отдельно.", 0, 0.2, 0.01);
        OfxPropertySetHandle seedProps = nullptr;
        require(parameters->paramDefine(set, kOfxParamTypeInteger, "projector_seed", &seedProps), "define projector seed");
        require(properties->propSetString(seedProps, kOfxPropLabel, 0, "Projector Seed"), "projector seed label");
        optionalProperty(properties->propSetString(seedProps, kOfxParamPropHint, 0,
            "Независимый seed лампы и транспорта плёнки. При том же кадре и диапазоне Source рисунок повторяется после перемотки. Не меняет зерно или ручку Camera; native RNG не совпадает с Torch RNG Comfy."), "projector seed hint");
        require(properties->propSetInt(seedProps, kOfxParamPropDefault, 0, 1925), "projector seed default");
        require(properties->propSetInt(seedProps, kOfxParamPropMin, 0, 0), "projector seed min");
        require(properties->propSetInt(seedProps, kOfxParamPropMax, 0, 2147483647), "projector seed max");
        require(properties->propSetInt(seedProps, kOfxParamPropDisplayMin, 0, 0), "projector seed display min");
        require(properties->propSetInt(seedProps, kOfxParamPropDisplayMax, 0, 2147483647), "projector seed display max");
        defineDouble(set, "glow_strength", "Glow Strength",
            "Доля света, перераспределяемая рассеянием проекционной оптики. 0 выключает; начните с 0,05–0,15. Расчёт в линейном свете без порога и автоконтраста.", 0, 1, 0);
        defineDouble(set, "glow_size", "Glow Size",
            "Sigma ореола в процентах ширины кадра. Значение 1 соответствует около 19 px при ширине 1920. Масштабируется с разрешением.", 0.05, 5, 1);
        OfxPropertySetHandle transfer = nullptr;
        defineAdvancedColor(set);
        require(parameters->paramDefine(set, kOfxParamTypeChoice, "encoding", &transfer), "define projector transfer");
        require(properties->propSetString(transfer, kOfxParamPropParent, 0, "advanced_color"), "projector encoding group");
        require(properties->propSetString(transfer, kOfxPropLabel, 0, "Input / Output Encoding"), "projector transfer label");
        optionalProperty(properties->propSetString(transfer, kOfxParamPropHint, 0, "Фактическая transfer curve позитивной копии. После транспорта плёнки RGB декодируется в линейный свет для лампы и glow, затем кодируется обратно. Согласуйте с выходом Print."), "projector transfer hint");
        require(properties->propSetInt(transfer, kOfxParamPropDefault, 0, static_cast<int>(silentfilm::color::pipelineTransfer)), "projector transfer default");
        require(properties->propSetString(transfer, kOfxParamPropChoiceOption, 0, "Linear Rec.709"), "projector linear transfer");
        require(properties->propSetString(transfer, kOfxParamPropChoiceOption, 1, "sRGB"), "projector sRGB transfer");
        require(properties->propSetString(transfer, kOfxParamPropChoiceOption, 2, "Rec.709 Gamma 2.4"), "projector gamma transfer");
        transfer = page;
        const char* controls[] = {"gate_weave_px", "lamp_flicker", "lamp_breathing", "projector_seed", "glow_strength", "glow_size", "encoding"};
        for (int i = 0; i < 7; ++i) require(properties->propSetString(transfer, kOfxParamPropPageChild, i, controls[i]), "projector page control");
        defineReferenceControls(set, page, kind);
        return kOfxStatOK;
    }
    if (kind == EffectKind::Defects) {
        const auto page = defineControlsPage(set);
        defineDouble(set, "blend", "Blend",
            "Общее наложение всей ноды: 0 — исходник, 0,5 — половина результата, 1 — полный результат при выбранном Strength. В режиме Perceptual смешивание негатива учитывает нейтральную печать, чтобы царапины не становились чёрными от малых значений. Альфа сохраняется; 0 полностью пропускает расчёт.", 0, 1, 1);
        OfxPropertySetHandle choice = nullptr;
        require(parameters->paramDefine(set, kOfxParamTypeChoice, "mix_response", &choice), "define defect mix response");
        require(properties->propSetString(choice, kOfxPropLabel, 0, "Mix Response"), "defect mix response label");
        require(properties->propSetInt(choice, kOfxParamPropDefault, 0, 0), "defect mix response default");
        require(properties->propSetString(choice, kOfxParamPropChoiceOption, 0, "Perceptual — smooth fades"), "perceptual mix option");
        require(properties->propSetString(choice, kOfxParamPropChoiceOption, 1, "Physical material"), "physical mix option");
        optionalProperty(properties->propSetString(choice, kOfxParamPropHint, 0,
            "Perceptual: Strength и Blend плавно регулируют видимый результат. Для Negative/Printing используется нейтральная позитивная калибровка sRGB; физическое пропускание на выходе сохраняется. Physical: прежняя сила маски/плотности и смешивание самого материала; после Print малые значения могут давать сильные полосы. Для A/B физической математики Comfy выбирай Physical. Нестандартная коррекция Print может изменить видимый ход Perceptual."), "defect mix response hint");
        require(parameters->paramDefine(set, kOfxParamTypeChoice, "material", &choice), "define defect material");
        require(properties->propSetString(choice, kOfxPropLabel, 0, "Material Stage"), "defect material label");
        optionalProperty(properties->propSetString(choice, kOfxParamPropHint, 0, "Выберите, где находится дефект: Camera, Negative, печатный этап, Positive или после Projector. Подключайте этот эффект в соответствующем месте цепочки."), "defect material hint");
        const char* stages[] = {"Capture / Camera", "Negative", "Printing", "Positive", "Projector Gate"};
        for (int i = 0; i < 5; ++i) require(properties->propSetString(choice, kOfxParamPropChoiceOption, i, stages[i]), "defect material option");
        require(properties->propSetInt(choice, kOfxParamPropDefault, 0, 0), "defect material default");
        require(parameters->paramDefine(set, kOfxParamTypeChoice, "pattern", &choice), "define defect pattern");
        require(properties->propSetString(choice, kOfxPropLabel, 0, "Pattern"), "defect pattern label");
        optionalProperty(properties->propSetString(choice, kOfxParamPropHint, 0, "Dust — пылинки, Scratches — продольные царапины, Stains — мягкие пятна, Development — медленная неоднородность плотности."), "defect pattern hint");
        const char* patterns[] = {"Dust", "Scratches", "Stains", "Development"};
        for (int i = 0; i < 4; ++i) require(properties->propSetString(choice, kOfxParamPropChoiceOption, i, patterns[i]), "defect pattern option");
        require(properties->propSetInt(choice, kOfxParamPropDefault, 0, 0), "defect pattern default");
        defineDouble(set, "strength", "Strength",
            "В Perceptual — непрозрачность полного рисунка: 0 выключено, 0,5 половина, 1 полная сила; итоговая доля также умножается на Blend. Размер, seed и рисунок сохраняются. В Physical — прежняя сила маски/плотности материала, её вид после печати может быть нелинейным.", 0, 1, 0.15);
        defineDouble(set, "size", "Size (% width)", "Размер частиц, ширина царапины или размер поля относительно ширины кадра. Для пыли начните с 0,05–0,15; для пятен проявки — 5–15.", 0.01, 50, 0.12);
        defineDouble(set, "size_spread", "Size Spread (%)",
            "Разброс размера пылинок/пятен относительно Size. 0 — одинаковый размер; 35 — от 65% до 135% заданного размера. Для царапин используется отдельный Thickness Spread. Не меняет количество и непрозрачность.", 0, 100, 35);
        defineDouble(set, "thickness_spread", "Thickness Spread (%)",
            "Разброс толщины между царапинами относительно Size. 0 — одинаковая толщина; 50 — от 50% до 150%. Толщина конкретной царапины сохраняется в пределах Persistence; seed и перемотка воспроизводимы.", 0, 100, 35);
        defineDouble(set, "edge_bias", "Edge Bias",
            "Распределение пыли, царапин и пятен: 0 — равномерно, 1 — только у левого и правого краёв, в полосе около 12% ширины кадра. По высоте распределение остаётся равномерным. Промежуточные значения смешивают два распределения.", 0, 1, 0);
        defineDouble(set, "scratch_wander", "Scratch Wander (px @1920)",
            "Боковое гуляние царапин по кадрам, без периодической синусоиды. 0 выключает; 2–8 px — старт. Пиксели заданы для ширины 1920 и масштабируются с изображением. Рисунок не зависит от порядка render. Небольшой статический наклон сохраняется отдельно.", 0, 64, 0);
        defineDouble(set, "scratch_wander_time", "Wander Time (frames)",
            "Плавность бокового гуляния: число кадров между случайными опорными точками. 1 — живее, как frame-to-frame path Comfy; 8–16 — медленнее и плавнее. Не меняет скорость клипа и Persistence.", 1, 240, 8);
        defineDouble(set, "amount", "Amount", "Среднее количество следов в кадре; дробные значения дают вероятностное число частиц. Учитывается только для пыли, царапин и пятен.", 0, 24, 3);
        defineDouble(set, "amount_variation", "Amount Variation (%)",
            "Разброс количества пыли/царапин во времени. 0 — постоянный Amount; 50 — примерно от 0,5 до 1,5 Amount; 100 допускает от 0 до 2 Amount. Плавный случайный ход, не синусоида. Это число следов, а не оптическая плотность или Strength.", 0, 100, 0);
        defineDouble(set, "amount_drift_time", "Amount Drift Time (frames)",
            "Число кадров между опорными значениями меняющегося количества. 48 при 25 fps — около 1,9 с. Больше — медленнее. Не меняет время жизни/геометрию уже активных следов; это задаёт Persistence.", 1, 10000, 48);
        require(parameters->paramDefine(set, kOfxParamTypeInteger, "persistence", &choice), "define defect persistence");
        require(properties->propSetString(choice, kOfxPropLabel, 0, "Persistence (frames)"), "defect persistence label");
        optionalProperty(properties->propSetString(choice, kOfxParamPropHint, 0, "Сколько кадров держится событие до смены рисунка. Одинаковое время и seed всегда возвращают тот же рисунок."), "defect persistence hint");
        require(properties->propSetInt(choice, kOfxParamPropDefault, 0, 48), "defect persistence default");
        require(properties->propSetInt(choice, kOfxParamPropMin, 0, 1), "defect persistence minimum");
        require(properties->propSetInt(choice, kOfxParamPropMax, 0, 10000), "defect persistence maximum");
        require(properties->propSetInt(choice, kOfxParamPropDisplayMin, 0, 1), "defect persistence display minimum");
        require(properties->propSetInt(choice, kOfxParamPropDisplayMax, 0, 10000), "defect persistence display maximum");
        require(parameters->paramDefine(set, kOfxParamTypeInteger, "seed", &choice), "define defect seed");
        require(properties->propSetString(choice, kOfxPropLabel, 0, "Seed"), "defect seed label");
        optionalProperty(properties->propSetString(choice, kOfxParamPropHint, 0, "Независимый seed эффекта. При перемотке назад рисунок воспроизводится без накопленного состояния."), "defect seed hint");
        require(properties->propSetInt(choice, kOfxParamPropDefault, 0, 1925), "defect seed default");
        require(properties->propSetInt(choice, kOfxParamPropMin, 0, 0), "defect seed minimum");
        require(properties->propSetInt(choice, kOfxParamPropMax, 0, 2147483647), "defect seed maximum");
        require(properties->propSetInt(choice, kOfxParamPropDisplayMin, 0, 0), "defect seed display minimum");
        require(properties->propSetInt(choice, kOfxParamPropDisplayMax, 0, 2147483647), "defect seed display maximum");
        defineAdvancedColor(set);
        require(parameters->paramDefine(set, kOfxParamTypeChoice, "encoding", &choice), "define defect transfer");
        require(properties->propSetString(choice, kOfxParamPropParent, 0, "advanced_color"), "defect encoding group");
        require(properties->propSetString(choice, kOfxPropLabel, 0, "Encoding"), "defect transfer label");
        for (int i = 0; i < 3; ++i) require(properties->propSetString(choice, kOfxParamPropChoiceOption, i,
            i == 0 ? "Linear Rec.709" : i == 1 ? "sRGB" : "Rec.709 Gamma 2.4"), "defect transfer option");
        require(properties->propSetInt(choice, kOfxParamPropDefault, 0, static_cast<int>(silentfilm::color::pipelineTransfer)), "defect transfer default");
        optionalProperty(properties->propSetString(choice, kOfxParamPropHint, 0,
            "В обычной цепочке оставь sRGB. У Negative/Printing gamma не применяется: это физическое пропускание и поле отключено. У Capture/Positive/Projector ручной override нужен только при использовании ноды отдельно. Comfy color only восстанавливает sRGB, не меняя рисунок/силу."), "defect transfer contract hint");
        choice = page;
        const char* children[] = {"blend", "mix_response", "material", "pattern", "strength", "size", "size_spread", "thickness_spread", "edge_bias",
            "scratch_wander", "scratch_wander_time", "amount", "amount_variation", "amount_drift_time", "persistence", "seed", "encoding"};
        for (int i = 0; i < 17; ++i) require(properties->propSetString(choice, kOfxParamPropPageChild, i, children[i]), "defect page control");
        defineReferenceControls(set, page, kind);
        return kOfxStatOK;
    }
    if (kind != EffectKind::Probe) {
        defineFilmControls(set, kind);
        return kOfxStatOK;
    }
    const auto page = defineControlsPage(set);
    defineDouble(set, gainName, "Diagnostic Gain (EV)", "Диагностическое усиление RGB без преобразования цветовой кривой: 0 — без изменения, +1 удваивает RGB, −1 делит RGB пополам. Альфа не меняется. Для Negative/Print используйте экспозицию плёнки.", -4, 4);
    defineDouble(set, mixName, "Temporal Mix", "Смешивание текущего кадра с соседними: 0 — только текущий; 1 — 50% текущего и по 25% соседних. Требуется temporal access хоста. Это проверка доступа к кадрам, не optical flow.", 0, 1);
    OfxPropertySetHandle props = nullptr;
    require(parameters->paramDefine(set, kOfxParamTypeInteger, offsetName, &props), "define offset");
    require(properties->propSetString(props, kOfxPropLabel, 0, "Neighbor Offset (frames)"), "offset label");
    optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0, "Расстояние до соседних кадров в единицах времени OFX (кадрах), от 1 до 8. Используется только при Temporal Mix > 0; запрос ограничивается началом и концом исходного клипа."), "offset hint");
    require(properties->propSetInt(props, kOfxParamPropDefault, 0, 1), "offset default");
    require(properties->propSetInt(props, kOfxParamPropMin, 0, 1), "offset minimum");
    require(properties->propSetInt(props, kOfxParamPropMax, 0, 8), "offset maximum");
    require(properties->propSetInt(props, kOfxParamPropDisplayMin, 0, 1), "offset display minimum");
    require(properties->propSetInt(props, kOfxParamPropDisplayMax, 0, 8), "offset display maximum");
    props = page;
    require(properties->propSetString(props, kOfxParamPropPageChild, 0, gainName), "page gain");
    require(properties->propSetString(props, kOfxParamPropPageChild, 1, mixName), "page mix");
    require(properties->propSetString(props, kOfxParamPropPageChild, 2, offsetName), "page offset");
    return kOfxStatOK;
}

/** Create a lightweight instance containing host handles only. */
OfxStatus create(OfxImageEffectHandle effect, EffectKind kind)
{
    auto state = std::make_unique<Instance>();
    state->kind = kind;
    require(effects->clipGetHandle(effect, "Source", &state->source, nullptr), "source clip");
    require(effects->clipGetHandle(effect, "Output", &state->output, nullptr), "output clip");
    OfxParamSetHandle set = nullptr;
    require(effects->getParamSet(effect, &set), "instance parameter set");
    if (kind == EffectKind::CrankExposure) {
        require(parameters->paramGetHandle(set, "exposure_map", &state->exposureMap, nullptr), "exposure map handle");
        require(parameters->paramGetHandle(set, "blend", &state->exposureBlend, nullptr), "exposure blend handle");
        require(parameters->paramGetHandle(set, "frame_offset", &state->exposureOffset, nullptr), "exposure offset handle");
    } else if (kind == EffectKind::Probe) {
        require(parameters->paramGetHandle(set, gainName, &state->gain, nullptr), "gain parameter");
        require(parameters->paramGetHandle(set, mixName, &state->mix, nullptr), "mix parameter");
        require(parameters->paramGetHandle(set, offsetName, &state->offset, nullptr), "offset parameter");
    } else if (kind == EffectKind::Camera || kind == EffectKind::CameraOptics) {
        require(parameters->paramGetHandle(set, "vignette_strength", &state->vignetteStrength, nullptr), "camera vignette strength parameter");
        require(parameters->paramGetHandle(set, "edge_softness", &state->edgeSoftness, nullptr), "camera edge softness parameter");
        require(parameters->paramGetHandle(set, "edge_start", &state->edgeStart, nullptr), "camera edge start parameter");
        require(parameters->paramGetHandle(set, "edge_end", &state->edgeEnd, nullptr), "camera edge end parameter");
        require(parameters->paramGetHandle(set, "camera_softness", &state->cameraSoftness, nullptr), "camera softness parameter");
        require(parameters->paramGetHandle(set, "focus_drift", &state->focusDrift, nullptr), "camera focus drift parameter");
        require(parameters->paramGetHandle(set, "focus_seed", &state->focusSeed, nullptr), "camera focus seed parameter");
        if (kind != EffectKind::CameraOptics) {
            require(parameters->paramGetHandle(set, "playback_mode", &state->playbackMode, nullptr), "camera playback mode parameter");
            require(parameters->paramGetHandle(set, "capture_fps_min", &state->captureFpsMin, nullptr), "camera minimum rate parameter");
            require(parameters->paramGetHandle(set, "capture_fps_max", &state->captureFpsMax, nullptr), "camera maximum rate parameter");
            require(parameters->paramGetHandle(set, "crank_variation", &state->crankVariation, nullptr), "camera rate variation parameter");
            require(parameters->paramGetHandle(set, "crank_irregularity", &state->crankIrregularity, nullptr), "camera crank irregularity parameter");
            require(parameters->paramGetHandle(set, "shutter_fraction", &state->shutterFraction, nullptr), "camera shutter parameter");
            require(parameters->paramGetHandle(set, "motion_blur_strength", &state->motionBlurStrength, nullptr), "camera motion blur parameter");
            require(parameters->paramGetHandle(set, "exposure_variation", &state->exposureVariation, nullptr), "camera exposure parameter");
            require(parameters->paramGetHandle(set, "exposure_samples", &state->exposureSamples, nullptr), "camera samples parameter");
            require(parameters->paramGetHandle(set, "camera_shake", &state->cameraShake, nullptr), "camera shake parameter");
            require(parameters->paramGetHandle(set, "camera_seed", &state->cameraSeed, nullptr), "camera seed parameter");
        }
    } else if (kind == EffectKind::Projector) {
        require(parameters->paramGetHandle(set, "gate_weave_px", &state->gateWeave, nullptr), "projector weave parameter");
        require(parameters->paramGetHandle(set, "lamp_flicker", &state->lampFlicker, nullptr), "projector flicker parameter");
        require(parameters->paramGetHandle(set, "lamp_breathing", &state->lampBreathing, nullptr), "projector breathing parameter");
        require(parameters->paramGetHandle(set, "projector_seed", &state->projectorSeed, nullptr), "projector seed parameter");
        require(parameters->paramGetHandle(set, "glow_strength", &state->glowStrength, nullptr), "projector glow strength parameter");
        require(parameters->paramGetHandle(set, "glow_size", &state->glowSize, nullptr), "projector glow size parameter");
        require(parameters->paramGetHandle(set, "encoding", &state->encoding, nullptr), "projector transfer parameter");
    } else if (kind == EffectKind::Defects) {
        require(parameters->paramGetHandle(set, "material", &state->defectMaterial, nullptr), "defect material parameter");
        require(parameters->paramGetHandle(set, "pattern", &state->defectPattern, nullptr), "defect pattern parameter");
        require(parameters->paramGetHandle(set, "strength", &state->defectStrength, nullptr), "defect strength parameter");
        require(parameters->paramGetHandle(set, "blend", &state->defectBlend, nullptr), "defect blend parameter");
        require(parameters->paramGetHandle(set, "mix_response", &state->defectMixResponse, nullptr), "defect mix response parameter");
        require(parameters->paramGetHandle(set, "size", &state->defectSize, nullptr), "defect size parameter");
        require(parameters->paramGetHandle(set, "size_spread", &state->defectSizeSpread, nullptr), "defect size spread parameter");
        require(parameters->paramGetHandle(set, "thickness_spread", &state->defectThicknessSpread, nullptr), "defect thickness spread parameter");
        require(parameters->paramGetHandle(set, "edge_bias", &state->defectEdgeBias, nullptr), "defect edge bias parameter");
        require(parameters->paramGetHandle(set, "scratch_wander", &state->defectScratchWander, nullptr), "defect scratch wander parameter");
        require(parameters->paramGetHandle(set, "scratch_wander_time", &state->defectScratchWanderTime, nullptr), "defect wander time parameter");
        require(parameters->paramGetHandle(set, "amount_variation", &state->defectAmountVariation, nullptr), "defect amount variation parameter");
        require(parameters->paramGetHandle(set, "amount_drift_time", &state->defectAmountDriftTime, nullptr), "defect amount drift time parameter");
        require(parameters->paramGetHandle(set, "amount", &state->defectAmount, nullptr), "defect amount parameter");
        require(parameters->paramGetHandle(set, "persistence", &state->defectPersistence, nullptr), "defect persistence parameter");
        require(parameters->paramGetHandle(set, "seed", &state->defectSeed, nullptr), "defect seed parameter");
        require(parameters->paramGetHandle(set, "encoding", &state->encoding, nullptr), "defect transfer parameter");
    } else {
        require(parameters->paramGetHandle(set, "exposure_ev", &state->exposure, nullptr), "film exposure parameter");
        require(parameters->paramGetHandle(set, "contrast", &state->contrast, nullptr), "film contrast parameter");
        require(parameters->paramGetHandle(set, "shadow_softness", &state->shadow, nullptr), "film shadow parameter");
        require(parameters->paramGetHandle(set, "highlight_softness", &state->highlight, nullptr), "film highlight parameter");
        require(parameters->paramGetHandle(set, "fog_strength", &state->fog, nullptr), "film fog parameter");
        require(parameters->paramGetHandle(set, "halation_strength", &state->halationStrength, nullptr), "halation strength parameter");
        require(parameters->paramGetHandle(set, "halation_size", &state->halationSize, nullptr), "halation size parameter");
        require(parameters->paramGetHandle(set, "grain_strength", &state->grainStrength, nullptr), "grain strength parameter");
        require(parameters->paramGetHandle(set, "grain_size", &state->grainSize, nullptr), "grain size parameter");
        require(parameters->paramGetHandle(set, "grain_seed", &state->grainSeed, nullptr), "grain seed parameter");
        require(parameters->paramGetHandle(set, "encoding", &state->encoding, nullptr), "film encoding parameter");
        if (kind == EffectKind::Negative) require(parameters->paramGetHandle(set, "preview_positive", &state->positivePreview, nullptr), "film preview parameter");
    }
    updateDefectColorUi(*state, 0);
    require(properties->propSetPointer(effectProperties(effect), kOfxPropInstanceData, 0, state.get()), "store instance");
    state.release();
    return kOfxStatOK;
}

/** Release instance state while leaving the host-owned clip/parameter handles alone. */
OfxStatus destroy(OfxImageEffectHandle effect)
{
    auto* state = &instance(effect);
    require(properties->propSetPointer(effectProperties(effect), kOfxPropInstanceData, 0, nullptr), "clear instance");
    delete state;
    return kOfxStatOK;
}

/** Release reconstructible instance caches when requested by the host. */
OfxStatus purgeCaches(OfxImageEffectHandle effect)
{
    auto& state = instance(effect);
    state.crankSchedule.clear();
    state.focusSignals.clear();
    state.projectorSignals.clear();
    state.exposureMaps.clear();
#ifdef SILENTFILM_ENABLE_CUDA
    const auto status = state.projectorCuda.clear();
    if (status.code) {
        std::fprintf(stderr, "[ALEXZSilentFilmOFX] purge CUDA code=%d: %s\n", status.code, status.message);
        return kOfxStatFailed;
    }
#endif
    return kOfxStatOK;
}

struct CameraRequest {
    CameraMechanics mechanics;
    silentfilm::CaptureInterval interval;
    double range[2]{};
    double frameRate = 24;
    double center = 0, duration = 0;
    int sampleCount = 1;
    bool continuous = false;

    /** Return a shutter sample position in OFX frame units. */
    double sampleTime(int index) const {
        const double offset = sampleCount > 1 ? static_cast<double>(index) / (sampleCount - 1) - 0.5 : 0;
        return std::clamp(center + offset * duration, range[0], range[1]);
    }

    /** Bound discrete input fetches while preserving fractional exposure sample positions. */
    double leftTime(int index) const {
        const double value = sampleTime(index);
        return continuous ? value : std::clamp(std::floor(value), range[0], range[1]);
    }

    double rightTime(int index) const {
        const double value = sampleTime(index);
        if (continuous || value - leftTime(index) <= 1e-6) return leftTime(index);
        return std::clamp(std::floor(value) + 1, range[0], range[1]);
    }

    /** Match the normalized triangular shutter weights of Comfy's simple sampler. */
    float sampleWeight(int index) const {
        if (sampleCount == 1) return 1;
        const float offset = static_cast<float>(index) / (sampleCount - 1) - 0.5f;
        return std::max(1e-4f, 1 - std::abs(offset) * 2);
    }
};

/** Convert OFX frame time to seconds only at the deterministic crank schedule boundary. */
CameraRequest cameraRequestAt(Instance& state, double time)
{
    if (!std::isfinite(time)) throw Failure(kOfxStatErrValue, "Invalid camera render time");
    if (!temporalAccess) throw Failure(kOfxStatErrUnsupported, "Host does not support temporal access required by SilentFilm Camera");
    CameraRequest request;
    request.mechanics = cameraMechanicsAt(state, time);
    sourceFrameRange(state.source, request.range);
    request.frameRate = clipFrameRate(state.output);
    OfxPropertySetHandle clipProperties = nullptr;
    require(effects->clipGetPropertySet(state.source, &clipProperties), "camera source properties");
    int continuous = 0;
    const auto continuousStatus = properties->propGetInt(clipProperties, kOfxImageClipPropContinuousSamples, 0, &continuous);
    optionalProperty(continuousStatus, "continuous source sampling");
    request.continuous = continuousStatus == kOfxStatOK && continuous != 0;
    const double relative = (std::clamp(time, request.range[0], request.range[1])
        - request.range[0] + 0.5) / request.frameRate;
    const double sourceDuration = (request.range[1] - request.range[0] + 1) / request.frameRate;
    request.interval = request.mechanics.playbackMode == 0
        ? state.crankSchedule.projectedIntervalAt(relative, sourceDuration, request.mechanics.crank)
        : state.crankSchedule.intervalAt(relative, request.mechanics.crank);
    const double intervalDuration = request.interval.end - request.interval.start;
    const double phase = std::clamp((time + 0.5 - request.range[0]
        - request.interval.start * request.frameRate) / (intervalDuration * request.frameRate), 0.0, 1.0);
    const double centerPhase = request.mechanics.playbackMode == 0 ? 0.5
        : 0.5 + (phase - 0.5) * std::min(0.45, 0.35 * request.mechanics.motionBlurStrength);
    request.center = request.range[0] + (request.interval.start + centerPhase * intervalDuration) * request.frameRate - 0.5;
    double duration = intervalDuration * request.mechanics.shutterFraction * request.mechanics.motionBlurStrength;
    if (request.mechanics.motionBlurStrength > 0) {
        const double intervalSpan = std::max(1.0, intervalDuration * request.frameRate);
        duration = std::max(duration, 0.28 * intervalSpan * request.mechanics.motionBlurStrength / request.frameRate);
    }
    request.duration = duration * request.frameRate;
    request.sampleCount = request.duration > 1e-4 ? request.mechanics.exposureSamples : 1;
    return request;
}

/** Fetch only shutter samples and dispatch the complete camera pipeline to the negotiated backend. */
OfxStatus renderCamera(OfxImageEffectHandle effect, Instance& state, double time,
    OfxPropertySetHandle in, Image& output, bool cudaEnabled, silentfilm::RenderWindow region)
{
    const auto request = cameraRequestAt(state, time);
    const auto& mechanics = request.mechanics;
    auto camera = cameraSettingsAt(state, time, request.interval.frame);
    camera.outputPremultiplied = output.premultiplied();
    const double referenceFps = 0.5 * (mechanics.crank.minimumFps + mechanics.crank.maximumFps);
    const float exposureGain = static_cast<float>(std::pow(referenceFps / request.interval.fps, mechanics.exposureVariation));
    std::vector<std::unique_ptr<Image>> images;
    std::vector<double> imageTimes;
    std::vector<silentfilm::CameraExposureSample> samples;
    images.reserve(2 * request.sampleCount); imageTimes.reserve(2 * request.sampleCount); samples.reserve(request.sampleCount);
    const auto fetch = [&](double sourceTime) -> Image& {
        const auto found = std::find(imageTimes.begin(), imageTimes.end(), sourceTime);
        std::size_t index = static_cast<std::size_t>(found - imageTimes.begin());
        if (found == imageTimes.end()) {
            images.push_back(std::make_unique<Image>(state.source, sourceTime));
            imageTimes.push_back(sourceTime);
        }
        return *images[index];
    };
    for (int i = 0; i < request.sampleCount; ++i) {
        if (effects->abort(effect)) return kOfxStatOK;
        const double leftTime = request.leftTime(i), rightTime = request.rightTime(i);
        const auto& image = fetch(leftTime);
        if (i == 0) camera.sourcePremultiplied = image.premultiplied();
        else if (image.premultiplied() != camera.sourcePremultiplied)
            throw Failure(kOfxStatErrUnsupported, "Camera source samples use different alpha representations");
        silentfilm::ImageView next{};
        float mix = 0;
        if (rightTime != leftTime) {
            const auto& right = fetch(rightTime);
            if (right.premultiplied() != camera.sourcePremultiplied)
                throw Failure(kOfxStatErrUnsupported, "Camera interpolation images use different alpha representations");
            next = right.view();
            mix = static_cast<float>((request.sampleTime(i) - leftTime) / (rightTime - leftTime));
        }
        const double relative = (request.sampleTime(i) - request.range[0] + 0.5) / request.frameRate;
        const float phase = static_cast<float>(state.crankSchedule.phaseAt(relative, mechanics.crank));
        samples.push_back({image.view(), phase, next, mix, request.sampleWeight(i)});
    }
    if (const auto* trace = std::getenv("ALEXZ_SILENTFILM_OFX_TRACE"); trace && std::strcmp(trace, "1") == 0) {
        std::fprintf(stderr, "[ALEXZSilentFilmOFX] %s camera time=%.6f playback=%s source_center=%.6f capture=%lld fps=%.3f exposure=%.3f shake=%.3f samples=%d images=%zu range=%.6f,%.6f\n",
            cudaEnabled ? "CUDA" : "CPU", time, mechanics.playbackMode == 0 ? "projected" : "source-time",
            request.center, static_cast<long long>(request.interval.frame), request.interval.fps,
            exposureGain, mechanics.shake, request.sampleCount, images.size(), request.sampleTime(0), request.sampleTime(request.sampleCount - 1));
    }
#ifdef SILENTFILM_ENABLE_CUDA
    if (cudaEnabled) {
        void* stream = nullptr;
        const auto streamStatus = properties->propGetPointer(in, kOfxImageEffectPropCudaStream, 0, &stream);
        optionalProperty(streamStatus, "CUDA stream");
        std::array<silentfilm::CameraSampleTransform, 9> transforms{};
        const auto bounds = samples.front().image;
        for (int i = 0; i < request.sampleCount; ++i)
            transforms[i] = silentfilm::cameraSampleTransform(samples[i].phase, mechanics.shake,
                mechanics.crank.seed, bounds.x2 - bounds.x1, bounds.y2 - bounds.y1);
        const auto status = silentfilm::renderCudaCamera(samples.data(), transforms.data(), request.sampleCount,
            output.view(), region, camera, exposureGain, mechanics.shake > 0, stream, streamStatus == kOfxStatOK);
        if (status.code) throw Failure(kOfxStatFailed, status.message ? status.message : "CUDA Camera render failed");
        return kOfxStatOK;
    }
#else
    (void)in;
#endif
    if (!silentfilm::renderCameraCaptureCpu(samples.data(), request.sampleCount, output.view(), region,
        camera, exposureGain, mechanics.shake, mechanics.crank.seed, cameraAbort, effect)) {
        if (effects->abort(effect)) return kOfxStatOK;
        throw Failure(kOfxStatFailed, "SilentFilm Camera temporal capture failed");
    }
    return kOfxStatOK;
}

/** Apply vignette, softness and independent focus drift to the current clip frame. */
OfxStatus renderCameraOptics(OfxImageEffectHandle effect, Instance& state, double time,
    OfxPropertySetHandle in, Image& output, bool cudaEnabled, silentfilm::RenderWindow region)
{
    if (!std::isfinite(time)) throw Failure(kOfxStatErrValue, "Invalid optics frame time");
    double range[2]{};
    sourceFrameRange(state.source, range);
    // OFX time в этом adapter измеряется в кадрах, как и Source frame range.
    const double count = std::ceil(range[1] - range[0] + 1);
    if (!std::isfinite(count) || count < 1 || count > 10000000)
        throw Failure(kOfxStatErrValue, "Unsupported camera optics clip frame range");
    const auto frame = static_cast<std::int64_t>(std::floor(std::clamp(time - range[0], 0.0, count - 1) + 1e-9));
    auto optics = cameraSettingsAt(state, time, frame);
    if (count <= 1) optics.focusDrift = 0;
    if (optics.focusDrift > 0) {
        int seed = 1925;
        require(parameters->paramGetValueAtTime(state.focusSeed, time, &seed), "read normalized focus seed");
        optics.focusSignal = state.focusSignals.at(static_cast<std::size_t>(frame),
            static_cast<std::size_t>(count), static_cast<std::uint32_t>(seed));
    }
    Image source(state.source, time);
    optics.sourcePremultiplied = source.premultiplied();
    optics.outputPremultiplied = output.premultiplied();
#ifdef SILENTFILM_ENABLE_CUDA
    if (cudaEnabled) {
        void* stream = nullptr;
        const auto streamStatus = properties->propGetPointer(in, kOfxImageEffectPropCudaStream, 0, &stream);
        optionalProperty(streamStatus, "optics CUDA stream");
        const silentfilm::CameraExposureSample sample{source.view(), 0, {}, 0, 1};
        const silentfilm::CameraSampleTransform transform{};
        const auto status = silentfilm::renderCudaCamera(&sample, &transform, 1, output.view(), region,
            optics, 1.0f, false, stream, streamStatus == kOfxStatOK);
        if (status.code) throw Failure(kOfxStatFailed, status.message ? status.message : "CUDA optics failed");
        return kOfxStatOK;
    }
#else
    (void)in;
    (void)cudaEnabled;
#endif
    if (!silentfilm::renderCameraOpticsCpu(source.view(), output.view(), region, optics, cameraAbort, effect)) {
        if (effects->abort(effect)) return kOfxStatOK;
        throw Failure(kOfxStatFailed, "Camera optics failed");
    }
    return kOfxStatOK;
}

/** Tell the host which source frame range is needed for this particular request. */
OfxStatus framesNeeded(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    auto& state = instance(effect);
    const double time = actionTime(in);
    if (state.kind == EffectKind::Camera) {
        const auto request = cameraRequestAt(state, time);
        const double range[2] = {request.leftTime(0), request.rightTime(request.sampleCount - 1)};
        require(properties->propSetDoubleN(out, "OfxImageClipPropFrameRange_Source", 2, range), "camera frames needed");
        return kOfxStatOK;
    }
    if (state.kind != EffectKind::Probe) return kOfxStatReplyDefault;
    const auto settings = settingsAt(state, time);
    const double range[2] = {settings.mix > 0 ? sourceTime(state.source, time - settings.offset) : time,
                             settings.mix > 0 ? sourceTime(state.source, time + settings.offset) : time};
    require(properties->propSetDoubleN(out, "OfxImageClipPropFrameRange_Source", 2, range), "frames needed");
    return kOfxStatOK;
}

/** Let Resolve bypass neutral probe/defect effects without fetching images or executing a render. */
OfxStatus identity(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    auto& state = instance(effect);
    const double time = actionTime(in);
    if (state.kind == EffectKind::CrankExposure) {
        double blend = 1;
        require(parameters->paramGetValueAtTime(state.exposureBlend, time, &blend), "identity exposure blend");
        if (blend != 0) return kOfxStatReplyDefault;
    } else if (state.kind == EffectKind::Defects) {
        double strength = 0, amount = 0, blend = 1;
        int pattern = 0;
        require(parameters->paramGetValueAtTime(state.defectStrength, time, &strength), "identity defect strength");
        require(parameters->paramGetValueAtTime(state.defectBlend, time, &blend), "identity defect blend");
        require(parameters->paramGetValueAtTime(state.defectAmount, time, &amount), "identity defect amount");
        require(parameters->paramGetValueAtTime(state.defectPattern, time, &pattern), "identity defect pattern");
        if (blend != 0 && strength != 0 && (pattern == static_cast<int>(silentfilm::DefectPattern::Development) || amount != 0))
            return kOfxStatReplyDefault;
    } else if (state.kind == EffectKind::Probe) {
        const auto settings = settingsAt(state, time);
        if (settings.gainEV != 0 || settings.mix != 0) return kOfxStatReplyDefault;
    } else return kOfxStatReplyDefault;
    require(properties->propSetString(out, kOfxPropName, 0, "Source"), "identity source");
    require(properties->propSetDouble(out, kOfxPropTime, 0, time), "identity time");
    return kOfxStatOK;
}

/** Render with bounded image lifetimes, signed strides and no accumulated frame state. */
OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in)
{
    auto& state = instance(effect);
    const double time = actionTime(in);
    const bool isProbe = state.kind == EffectKind::Probe;
    const bool isCamera = state.kind == EffectKind::Camera;
    const bool isProjector = state.kind == EffectKind::Projector;
    const bool isDefects = state.kind == EffectKind::Defects;
    const bool isEmulsion = state.kind == EffectKind::Negative || state.kind == EffectKind::Print;
    const auto settings = isProbe ? settingsAt(state, time) : Settings{};
    auto emulsion = isEmulsion ? filmSettingsAt(state, time) : silentfilm::EmulsionParameters{};
    silentfilm::ProjectorParameters projector;
    if (isProjector) {
        double glowStrength = 0, glowSize = 1, weave = 0, flicker = 0, breathing = 0;
        int transfer = 1, seed = 1925;
        require(parameters->paramGetValueAtTime(state.glowStrength, time, &glowStrength), "read projector glow strength");
        require(parameters->paramGetValueAtTime(state.glowSize, time, &glowSize), "read projector glow size");
        require(parameters->paramGetValueAtTime(state.encoding, time, &transfer), "read projector transfer");
        require(parameters->paramGetValueAtTime(state.gateWeave, time, &weave), "read projector weave");
        require(parameters->paramGetValueAtTime(state.lampFlicker, time, &flicker), "read projector flicker");
        require(parameters->paramGetValueAtTime(state.lampBreathing, time, &breathing), "read projector breathing");
        require(parameters->paramGetValueAtTime(state.projectorSeed, time, &seed), "read projector seed");
        if (!std::isfinite(time) || !std::isfinite(glowStrength) || !std::isfinite(glowSize)
            || !std::isfinite(weave) || !std::isfinite(flicker) || !std::isfinite(breathing)
            || glowStrength < 0 || glowStrength > 1 || glowSize < 0.05 || glowSize > 5
            || transfer < 0 || transfer > 2 || weave < 0 || weave > 8 || flicker < 0 || flicker > 0.3
            || breathing < 0 || breathing > 0.2)
            throw Failure(kOfxStatErrValue, "Invalid projector parameter value");
        double range[2]{};
        sourceFrameRange(state.source, range);
        const double count = std::ceil(range[1] - range[0] + 1);
        if (!std::isfinite(count) || count < 1 || count > 10000000)
            throw Failure(kOfxStatErrValue, "Unsupported projector clip frame range");
        const auto frame = static_cast<std::size_t>(std::floor(std::clamp(time - range[0], 0.0, count - 1) + 1e-9));
        projector.frame = state.projectorSignals.at(frame, static_cast<std::size_t>(count), static_cast<std::uint32_t>(seed),
            static_cast<float>(weave), static_cast<float>(flicker), static_cast<float>(breathing));
        double scale[2]{1, 1};
        optionalProperty(properties->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, scale), "projector render scale");
        if (!std::isfinite(scale[0]) || !std::isfinite(scale[1]) || scale[0] <= 0 || scale[1] <= 0)
            throw Failure(kOfxStatErrValue, "Invalid projector render scale");
        projector.frame.shiftX *= static_cast<float>(scale[0]);
        projector.frame.shiftY *= static_cast<float>(scale[1]);
        projector.transfer = static_cast<silentfilm::Transfer>(transfer);
        projector.glowStrength = static_cast<float>(glowStrength);
        projector.glowSize = static_cast<float>(glowSize);
    }
    silentfilm::DefectParameters defects;
    if (isDefects) {
        int material = 0, pattern = 0, persistence = 48, seed = 1925, transfer = 1, mixResponse = 0;
        double strength = 0.15, size = 0.12, amount = 3, blend = 1;
        double sizeSpread = 35, thicknessSpread = 35, edgeBias = 0, wander = 0, wanderTime = 8;
        double amountVariation = 0, amountDriftTime = 48;
        require(parameters->paramGetValueAtTime(state.defectMaterial, time, &material), "read defect material");
        require(parameters->paramGetValueAtTime(state.defectPattern, time, &pattern), "read defect pattern");
        require(parameters->paramGetValueAtTime(state.defectStrength, time, &strength), "read defect strength");
        require(parameters->paramGetValueAtTime(state.defectBlend, time, &blend), "read defect blend");
        require(parameters->paramGetValueAtTime(state.defectMixResponse, time, &mixResponse), "read defect mix response");
        require(parameters->paramGetValueAtTime(state.defectSize, time, &size), "read defect size");
        require(parameters->paramGetValueAtTime(state.defectSizeSpread, time, &sizeSpread), "read defect size spread");
        require(parameters->paramGetValueAtTime(state.defectThicknessSpread, time, &thicknessSpread), "read defect thickness spread");
        require(parameters->paramGetValueAtTime(state.defectEdgeBias, time, &edgeBias), "read defect edge bias");
        require(parameters->paramGetValueAtTime(state.defectScratchWander, time, &wander), "read defect wander");
        require(parameters->paramGetValueAtTime(state.defectScratchWanderTime, time, &wanderTime), "read defect wander time");
        require(parameters->paramGetValueAtTime(state.defectAmountVariation, time, &amountVariation), "read defect amount variation");
        require(parameters->paramGetValueAtTime(state.defectAmountDriftTime, time, &amountDriftTime), "read defect amount drift time");
        require(parameters->paramGetValueAtTime(state.defectAmount, time, &amount), "read defect amount");
        require(parameters->paramGetValueAtTime(state.defectPersistence, time, &persistence), "read defect persistence");
        require(parameters->paramGetValueAtTime(state.defectSeed, time, &seed), "read defect seed");
        require(parameters->paramGetValueAtTime(state.encoding, time, &transfer), "read defect transfer");
        if (material < 0 || material > 4 || pattern < 0 || pattern > 3 || persistence < 1
            || !std::isfinite(strength) || strength < 0 || strength > 1 || !std::isfinite(size) || size < 0.01 || size > 50
            || !std::isfinite(amount) || amount < 0 || amount > 24 || transfer < 0 || transfer > 2)
            throw Failure(kOfxStatErrValue, "Invalid defect parameter value");
        if (!std::isfinite(blend) || blend < 0 || blend > 1 || mixResponse < 0 || mixResponse > 1)
            throw Failure(kOfxStatErrValue, "Invalid defect blend value");
        const auto inRange = [](double value, double low, double high) {
            return std::isfinite(value) && value >= low && value <= high;
        };
        if (!inRange(sizeSpread, 0, 100) || !inRange(thicknessSpread, 0, 100) || !inRange(edgeBias, 0, 1)
            || !inRange(wander, 0, 64) || !inRange(wanderTime, 1, 240)
            || !inRange(amountVariation, 0, 100) || !inRange(amountDriftTime, 1, 10000))
            throw Failure(kOfxStatErrValue, "Invalid defect shape or timing value");
        defects.material = static_cast<silentfilm::DefectMaterial>(material);
        defects.pattern = static_cast<silentfilm::DefectPattern>(pattern);
        defects.transfer = silentfilm::color::defectTransfer(defects.material, static_cast<silentfilm::Transfer>(transfer));
        defects.strength = static_cast<float>(strength); defects.size = static_cast<float>(size);
        defects.blend = static_cast<float>(blend);
        defects.mixResponse = static_cast<silentfilm::DefectMixResponse>(mixResponse);
        defects.amount = static_cast<float>(amount); defects.persistence = persistence;
        defects.sizeSpreadPercent = static_cast<float>(sizeSpread);
        defects.thicknessSpreadPercent = static_cast<float>(thicknessSpread);
        defects.edgeBias = static_cast<float>(edgeBias);
        defects.scratchWander = static_cast<float>(wander);
        defects.scratchWanderTime = static_cast<int>(std::floor(wanderTime + 0.5));
        defects.amountVariationPercent = static_cast<float>(amountVariation);
        defects.amountDriftTime = static_cast<int>(std::floor(amountDriftTime + 0.5));
        defects.seed = static_cast<std::uint32_t>(seed);
        defects.frameRate = clipFrameRate(state.output);
        double range[2]{};
        sourceFrameRange(state.source, range);
        defects.time = std::max(0.0, time - range[0]) / defects.frameRate;
    }
    int cudaEnabled = 0;
    optionalProperty(properties->propGetInt(in, kOfxImageEffectPropCudaEnabled, 0, &cudaEnabled), "CUDA render mode");
#ifndef SILENTFILM_ENABLE_CUDA
    if (cudaEnabled) throw Failure(kOfxStatErrUnsupported, "This plugin was built without CUDA support");
#endif
    if (effects->abort(effect)) return kOfxStatOK;
    Image output(state.output, time);
    if (isCamera || state.kind == EffectKind::CameraOptics) {
        int window[4]{};
        require(properties->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, window), "render window");
        if (state.kind == EffectKind::CameraOptics)
            return renderCameraOptics(effect, state, time, in, output, cudaEnabled != 0,
                {window[0], window[1], window[2], window[3]});
        return renderCamera(effect, state, time, in, output, cudaEnabled != 0,
            {window[0], window[1], window[2], window[3]});
    }
    Image current(state.source, time);
    if (isEmulsion) {
        emulsion.sourcePremultiplied = current.premultiplied();
        emulsion.outputPremultiplied = output.premultiplied();
    } else if (isDefects) {
        defects.sourcePremultiplied = current.premultiplied();
        defects.outputPremultiplied = output.premultiplied();
    }
    const auto currentView = current.view();
    const int imageWidth = currentView.x2 - currentView.x1;
    std::unique_ptr<Image> previous, next;
    double previousTime = time, nextTime = time;
    if (settings.mix > 0) {
        previousTime = sourceTime(state.source, time - settings.offset);
        nextTime = sourceTime(state.source, time + settings.offset);
        if (previousTime != time) previous = std::make_unique<Image>(state.source, previousTime);
        if (nextTime != time) next = std::make_unique<Image>(state.source, nextTime);
    }
    if (const auto* trace = std::getenv("ALEXZ_SILENTFILM_OFX_TRACE"); trace && std::strcmp(trace, "1") == 0) {
        std::fprintf(stderr, "[ALEXZSilentFilmOFX] %s time=%.6f neighbors=%.6f,%.6f mix=%.3f effect=%s\n", cudaEnabled ? "CUDA" : "CPU", time, previousTime, nextTime, settings.mix, effectName(state.kind));
    }
    int window[4] = {};
    require(properties->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, window), "render window");
    if (state.kind == EffectKind::CrankExposure) {
        double blend = 1;
        int offset = 0;
        char* path = nullptr;
        require(parameters->paramGetValueAtTime(state.exposureBlend, time, &blend), "read exposure blend");
        require(parameters->paramGetValue(state.exposureOffset, &offset), "read exposure frame offset");
        if (!std::isfinite(blend) || blend < 0 || blend > 1) throw Failure(kOfxStatErrValue, "Invalid Crank Exposure Blend");
        float gain = 1;
        if (blend > 0) {
            require(parameters->paramGetValue(state.exposureMap, &path), "read exposure map path");
            try {
                const auto map = state.exposureMaps.load(path ? path : "");
                double range[2]{};
                sourceFrameRange(state.source, range);
                gain = map->at(time - range[0], offset, clipFrameRate(state.source));
            } catch (const Failure&) { throw; }
            catch (const std::runtime_error& error) { throw Failure(kOfxStatErrValue, error.what()); }
        }
        const silentfilm::RenderWindow region{window[0], window[1], window[2], window[3]};
#ifdef SILENTFILM_ENABLE_CUDA
        if (cudaEnabled) {
            void* stream = nullptr;
            const auto streamStatus = properties->propGetPointer(in, kOfxImageEffectPropCudaStream, 0, &stream);
            optionalProperty(streamStatus, "exposure CUDA stream");
            const auto status = silentfilm::renderCudaCrankExposure(current.view(), output.view(), region, gain,
                static_cast<float>(blend), current.premultiplied(), output.premultiplied(), stream, streamStatus == kOfxStatOK);
            if (status.code) throw Failure(kOfxStatFailed, status.message ? status.message : "CUDA Crank Exposure failed");
            return kOfxStatOK;
        }
#endif
        if (!silentfilm::renderCrankExposureCpu(current.view(), output.view(), region, gain, static_cast<float>(blend),
            current.premultiplied(), output.premultiplied(), cameraAbort, effect) && !effects->abort(effect))
            throw Failure(kOfxStatFailed, "Crank Exposure cancelled");
        return kOfxStatOK;
    }
    if (isProjector) {
        const silentfilm::RenderWindow region{window[0], window[1], window[2], window[3]};
        projector.sourcePremultiplied = current.premultiplied();
        projector.outputPremultiplied = output.premultiplied();
        if (const auto* trace = std::getenv("ALEXZ_SILENTFILM_OFX_TRACE"); trace && std::strcmp(trace, "1") == 0)
            std::fprintf(stderr, "[ALEXZSilentFilmOFX] %s projector time=%.6f shift=%.4f,%.4f lamp=%.5f glow=%.3f\n",
                cudaEnabled ? "CUDA" : "CPU", time, projector.frame.shiftX, projector.frame.shiftY,
                projector.frame.lampGain, projector.glowStrength);
        if (effects->abort(effect)) return kOfxStatOK;
#ifdef SILENTFILM_ENABLE_CUDA
        if (cudaEnabled) {
            void* stream = nullptr;
            const auto streamStatus = properties->propGetPointer(in, kOfxImageEffectPropCudaStream, 0, &stream);
            optionalProperty(streamStatus, "projector CUDA stream");
            const auto status = state.projectorCuda.render(current.view(), output.view(), region, projector,
                stream, streamStatus == kOfxStatOK);
            if (status.code) {
                std::fprintf(stderr, "[ALEXZSilentFilmOFX] Projector CUDA code=%d: %s\n", status.code,
                    status.message ? status.message : "CUDA Projector failed");
                throw Failure(kOfxStatFailed, status.message ? status.message : "CUDA Projector failed");
            }
            return kOfxStatOK;
        }
#endif
        if (!silentfilm::renderProjectorCpu(current.view(), output.view(), region, projector, cameraAbort, effect)) {
            if (effects->abort(effect)) return kOfxStatOK;
            throw Failure(kOfxStatFailed, "SilentFilm Projector processing failed");
        }
        return kOfxStatOK;
    }
    if (isDefects) {
        const silentfilm::RenderWindow region{window[0], window[1], window[2], window[3]};
#ifdef SILENTFILM_ENABLE_CUDA
        if (cudaEnabled) {
            void* stream = nullptr;
            const auto streamStatus = properties->propGetPointer(in, kOfxImageEffectPropCudaStream, 0, &stream);
            optionalProperty(streamStatus, "defects CUDA stream");
            const auto status = silentfilm::renderCudaDefects(current.view(), output.view(), region, defects,
                stream, streamStatus == kOfxStatOK);
            if (status.code) throw Failure(kOfxStatFailed, status.message ? status.message : "CUDA Defects failed");
            return kOfxStatOK;
        }
#endif
        if (!silentfilm::renderDefectsCpu(current.view(), output.view(), region, defects, cameraAbort, effect)
            && !effects->abort(effect)) throw Failure(kOfxStatFailed, "Defects processing failed");
        return kOfxStatOK;
    }
    if (isEmulsion) {
        const silentfilm::RenderWindow region{window[0], window[1], window[2], window[3]};
#ifdef SILENTFILM_ENABLE_CUDA
        if (cudaEnabled) {
            void* stream = nullptr;
            const auto streamStatus = properties->propGetPointer(in, kOfxImageEffectPropCudaStream, 0, &stream);
            optionalProperty(streamStatus, "emulsion CUDA stream");
            const auto status = silentfilm::renderCudaFilm(current.view(), output.view(), region, emulsion,
                state.projectorCuda, stream, streamStatus == kOfxStatOK);
            if (status.code) throw Failure(kOfxStatFailed, status.message ? status.message : "CUDA emulsion failed");
            return kOfxStatOK;
        }
#endif
        if (!silentfilm::renderEmulsionCpu(current.view(), output.view(), region, emulsion,
            emulsion.halationStrength, emulsion.halationSize, cameraAbort, effect) && !effects->abort(effect))
            throw Failure(kOfxStatFailed, "Emulsion processing failed");
        return kOfxStatOK;
    }
    const float gain = silentfilm::gainFromEV(settings.gainEV);
#ifdef SILENTFILM_ENABLE_CUDA
    if (cudaEnabled) {
        if (effects->abort(effect)) return kOfxStatOK;
        void* stream = nullptr;
        const auto streamStatus = properties->propGetPointer(in, kOfxImageEffectPropCudaStream, 0, &stream);
        optionalProperty(streamStatus, "CUDA stream");
        const silentfilm::RenderWindow region{window[0], window[1], window[2], window[3]};
        const auto status = isEmulsion
            ? silentfilm::renderCudaEmulsion(current.view(), output.view(), region, emulsion, stream, streamStatus == kOfxStatOK)
            : silentfilm::renderCudaProbe(current.view(), previous ? previous->view() : current.view(),
                next ? next->view() : current.view(), output.view(), region,
                gain, static_cast<float>(settings.mix), stream, streamStatus == kOfxStatOK);
        if (status.code != 0) {
            std::fprintf(stderr, "[ALEXZSilentFilmOFX] CUDA error %d: %s\n", status.code, status.message);
            throw Failure(kOfxStatFailed, status.message);
        }
        return kOfxStatOK;
    }
#endif
    for (int y = window[1]; y < window[3]; ++y) {
        if (effects->abort(effect)) return kOfxStatOK;
        for (int x = window[0]; x < window[2]; ++x) {
            auto* destination = output.address(x, y);
            if (!destination) continue;
            const auto center = current.read(x, y);
            const auto value = isEmulsion
                ? (current.address(x, y) ? silentfilm::emulsionPixel(center, emulsion, x, y, imageWidth) : silentfilm::Pixel{})
                : silentfilm::probePixel(center, previous ? previous->read(x, y) : center,
                    next ? next->read(x, y) : center, gain, static_cast<float>(settings.mix));
            std::memcpy(destination, &value, sizeof(value));
        }
    }
    return kOfxStatOK;
}

/** Contain all C++ exceptions at the OpenFX C ABI boundary. */
template<EffectKind kind>
OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    const auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    try {
        if (!action) return kOfxStatErrValue;
        ActionTimer timer(effectName(kind), action);
        if (std::strcmp(action, kOfxActionLoad) == 0) return load();
        if (std::strcmp(action, kOfxActionUnload) == 0) return kOfxStatOK;
        if (!properties || !effects || !parameters) return kOfxStatErrMissingHostFeature;
        if (std::strcmp(action, kOfxActionDescribe) == 0) return describe(effect, kind);
        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) return describeContext(effect, kind);
        if (std::strcmp(action, kOfxActionCreateInstance) == 0) return create(effect, kind);
        if (std::strcmp(action, kOfxActionDestroyInstance) == 0) return destroy(effect);
        if (std::strcmp(action, kOfxActionPurgeCaches) == 0) return purgeCaches(effect);
        if (std::strcmp(action, kOfxActionInstanceChanged) == 0) return changed(effect, in);
        if (std::strcmp(action, kOfxImageEffectActionGetFramesNeeded) == 0) return framesNeeded(effect, in, out);
        if (std::strcmp(action, kOfxImageEffectActionIsIdentity) == 0) return identity(effect, in, out);
        if (std::strcmp(action, kOfxImageEffectActionRender) == 0) return render(effect, in);
        return kOfxStatReplyDefault;
    } catch (const std::bad_alloc&) {
        return kOfxStatErrMemory;
    } catch (const Failure& error) {
        if (messages) messages->message(effect, kOfxMessageError, "alexz.silentfilm.probe", "%s (OFX status %d)", error.what(), error.status);
        std::fprintf(stderr, "[ALEXZSilentFilmOFX] %s: %d\n", error.what(), error.status);
        return error.status;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[ALEXZSilentFilmOFX] %s\n", error.what());
        return kOfxStatFailed;
    } catch (...) {
        return kOfxStatFailed;
    }
}

/** Retain the host bootstrap pointer without fetching suites before load. */
void setHost(OfxHost* value) { host = value; }

OfxPlugin plugins[] = {
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.crankexposure", 0, 26, setHost, entry<EffectKind::CrankExposure>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.hostprobe", 0, 6, setHost, entry<EffectKind::Probe>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.camera", 0, 25, setHost, entry<EffectKind::Camera>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.negative", 0, 25, setHost, entry<EffectKind::Negative>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.print", 0, 25, setHost, entry<EffectKind::Print>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.projector", 0, 25, setHost, entry<EffectKind::Projector>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.defects", 0, 25, setHost, entry<EffectKind::Defects>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.cameraoptics", 0, 25, setHost, entry<EffectKind::CameraOptics>},
};

constexpr int pluginCount = static_cast<int>(sizeof(plugins) / sizeof(plugins[0]));

} // namespace

#if defined(_WIN32)
#define ALEXZ_EXPORT __declspec(dllexport)
#else
#define ALEXZ_EXPORT __attribute__((visibility("default")))
#endif

/** Return all effects shipped by this bundle. */
extern "C" ALEXZ_EXPORT int OfxGetNumberOfPlugins() { return pluginCount; }

/** Return plugin metadata without exposing implementation symbols. */
extern "C" ALEXZ_EXPORT OfxPlugin* OfxGetPlugin(int index) { return index >= 0 && index < pluginCount ? &plugins[index] : nullptr; }
