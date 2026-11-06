#include "silentfilm/probe.hpp"
#include "silentfilm/cuda_backend.hpp"

#include <ofxImageEffect.h>
#include <ofxGPURender.h>
#include <ofxProperty.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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

enum class EffectKind { Probe, Camera, Negative, Print };

/** Keep independent plugin names and identifiers within one shared adapter. */
const char* effectName(EffectKind kind)
{
    if (kind == EffectKind::Negative) return "SilentFilm Negative";
    if (kind == EffectKind::Print) return "SilentFilm Print";
    if (kind == EffectKind::Camera) return "SilentFilm Camera";
    return "SilentFilm Host Probe";
}

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
    EffectKind kind = EffectKind::Probe;
    OfxImageClipHandle source = nullptr;
    OfxImageClipHandle output = nullptr;
    OfxParamHandle gain = nullptr;
    OfxParamHandle mix = nullptr;
    OfxParamHandle offset = nullptr;
    OfxParamHandle exposure = nullptr, contrast = nullptr, shadow = nullptr, highlight = nullptr, fog = nullptr;
    OfxParamHandle encoding = nullptr, positivePreview = nullptr;
    OfxParamHandle grainStrength = nullptr, grainSize = nullptr, grainSeed = nullptr;
    OfxParamHandle vignetteStrength = nullptr, edgeStart = nullptr, edgeEnd = nullptr;
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

/** Evaluate exposure/development controls independently from the diagnostic effect. */
silentfilm::EmulsionParameters filmSettingsAt(Instance& state, double time)
{
    double exposure = 0, contrast = 1, shadow = 0, highlight = 0, fog = 0, grainStrength = 0, grainSize = 2;
    int encoding = 0, preview = 0;
    int grainSeed = 1925;
    require(parameters->paramGetValueAtTime(state.exposure, time, &exposure), "read film exposure");
    require(parameters->paramGetValueAtTime(state.contrast, time, &contrast), "read film contrast");
    require(parameters->paramGetValueAtTime(state.shadow, time, &shadow), "read film shadow compression");
    require(parameters->paramGetValueAtTime(state.highlight, time, &highlight), "read film highlight compression");
    require(parameters->paramGetValueAtTime(state.fog, time, &fog), "read film fog");
    require(parameters->paramGetValueAtTime(state.grainStrength, time, &grainStrength), "read grain strength");
    require(parameters->paramGetValueAtTime(state.grainSize, time, &grainSize), "read grain size");
    require(parameters->paramGetValueAtTime(state.grainSeed, time, &grainSeed), "read grain seed");
    require(parameters->paramGetValueAtTime(state.encoding, time, &encoding), "read film encoding");
    if (state.positivePreview) require(parameters->paramGetValueAtTime(state.positivePreview, time, &preview), "read positive preview");
    if (!std::isfinite(time) || !std::isfinite(exposure) || !std::isfinite(contrast)
        || !std::isfinite(shadow) || !std::isfinite(highlight) || !std::isfinite(fog)
        || !std::isfinite(grainStrength) || !std::isfinite(grainSize)
        || exposure < -4 || exposure > 4 || contrast < 0.2 || contrast > 3
        || shadow < 0 || shadow > 5 || highlight < 0 || highlight > 1 || fog < 0 || fog > 1
        || grainStrength < 0 || grainStrength > 0.2 || grainSize < 0.1 || grainSize > 16
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
    value.grainStrength = static_cast<float>(grainStrength);
    value.grainSize = static_cast<float>(grainSize);
    value.grainSeed = static_cast<std::uint32_t>(grainSeed)
        ^ (state.kind == EffectKind::Negative ? 0x4e454741u : 0x5052494eu);
    const float frameTime = static_cast<float>(time);
    std::memcpy(&value.grainFrame, &frameTime, sizeof(frameTime));
    value.previewPositive = preview != 0;
    return value;
}

/** Evaluate static camera-lens shading controls at the requested time. */
silentfilm::CameraOpticsParameters cameraSettingsAt(Instance& state, double time)
{
    double strength = 0, start = 0.6, end = 1.0;
    require(parameters->paramGetValueAtTime(state.vignetteStrength, time, &strength), "read camera vignette strength");
    require(parameters->paramGetValueAtTime(state.edgeStart, time, &start), "read camera vignette start");
    require(parameters->paramGetValueAtTime(state.edgeEnd, time, &end), "read camera vignette end");
    if (!std::isfinite(time) || !std::isfinite(strength) || !std::isfinite(start) || !std::isfinite(end)
        || strength < 0 || strength > 1 || start < 0 || start > 4 || end < 0.01 || end > 4 || end <= start) {
        throw Failure(kOfxStatErrValue, "Invalid camera optics parameter value");
    }
    return {static_cast<float>(strength), static_cast<float>(start), static_cast<float>(end)};
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

class Image {
    OfxPropertySetHandle handle_ = nullptr;
    unsigned char* data_ = nullptr;
    int bounds_[4] = {};
    int rowBytes_ = 0;

public:
    /** Acquire a float RGBA image and release it if validation fails. */
    Image(OfxImageClipHandle clip, double time)
    {
        require(effects->clipGetImage(clip, time, nullptr, &handle_), "fetch image");
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
        : kind == EffectKind::Camera
        ? "Статическая круговая виньетка съёмочного объектива с масштабом от ширины кадра. Не меняет каденс и не размывает изображение."
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
    require(properties->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, kind == EffectKind::Probe && temporalAccess ? 1 : 0), "temporal capability");
#ifdef SILENTFILM_ENABLE_CUDA
    optionalProperty(properties->propSetString(props, kOfxImageEffectPropCudaRenderSupported, 0, "true"), "CUDA capability");
    optionalProperty(properties->propSetString(props, kOfxImageEffectPropCudaStreamSupported, 0, "true"), "CUDA stream capability");
#endif
    return kOfxStatOK;
}

/** Define one animated scalar diagnostic control. */
void defineDouble(OfxParamSetHandle set, const char* name, const char* label, const char* hint, double low, double high, double defaultValue = 0)
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
}

/** Define readable material controls and an explicit input/output transfer. */
void defineFilmControls(OfxParamSetHandle set, EffectKind kind)
{
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
    defineDouble(set, "grain_strength", "Emulsion Grain",
        negative ? "Сила многослойного зерна в оптической плотности негатива. 0 — выключено; начните с 0,02–0,04. Зерно негатива проходит через Print и появляется в позитиве. Для повторяемого рисунка используйте один Grain Seed."
                 : "Сила многослойного зерна позитивной эмульсии. 0 — выключено; начните с 0–0,015, поскольку зерно негатива уже отпечатано. Зерно добавляется в плотность и зависит от Grain Size и Grain Seed.", 0, 0.2);
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
    require(parameters->paramDefine(set, kOfxParamTypeChoice, "encoding", &props), "define film encoding");
    require(properties->propSetString(props, kOfxPropLabel, 0, negative ? "Input Encoding" : "Output Encoding"), "encoding label");
    optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0,
        negative ? "Кодирование входного SDR-сигнала с первичными Rec.709. Выберите Linear Rec.709, sRGB или Gamma 2.4 по фактическому сигналу до ноды. Не угадывайте по виду картинки. Для Log, DaVinci Wide Gamut или ACES сначала задайте явный CST в Rec.709 Linear."
                 : "Кодирование готового SDR-позитива. Для нейтральной пары согласуйте его с Input Encoding у Negative. На вход Print получает физическое пропускание негатива: не ставьте CST, LUT или цветокоррекцию между Negative и Print."), "encoding hint");
    require(properties->propSetInt(props, kOfxParamPropDefault, 0, 0), "encoding default");
    require(properties->propSetString(props, kOfxParamPropChoiceOption, 0, "Linear Rec.709"), "linear encoding");
    require(properties->propSetString(props, kOfxParamPropChoiceOption, 1, "sRGB"), "sRGB encoding");
    require(properties->propSetString(props, kOfxParamPropChoiceOption, 2, "Rec.709 Gamma 2.4"), "gamma encoding");
    if (negative) {
        require(parameters->paramDefine(set, kOfxParamTypeBoolean, "preview_positive", &props), "define positive preview");
        require(properties->propSetString(props, kOfxPropLabel, 0, "View Positive (preview only)"), "preview label");
        optionalProperty(properties->propSetString(props, kOfxParamPropHint, 0, "Показывает пропускание негатива через фиксированную нейтральную печать. Для настройки временно отключите Print. Перед подачей сигнала в SilentFilm Print выключите этот режим: preview — уже позитив, а не негатив."), "preview hint");
        require(properties->propSetInt(props, kOfxParamPropDefault, 0, 0), "preview default");
    }
    require(parameters->paramDefine(set, kOfxParamTypePage, "Controls", &props), "define film page");
    int index = 0;
    for (const char* name : {"exposure_ev", "contrast", "shadow_softness", "highlight_softness", "fog_strength", "grain_strength", "grain_size", "grain_seed", "encoding"}) {
        require(properties->propSetString(props, kOfxParamPropPageChild, index++, name), "film page control");
    }
    if (negative) require(properties->propSetString(props, kOfxParamPropPageChild, index, "preview_positive"), "film page preview");
}

/** Define the first stateless camera-optics controls independently from cadence. */
void defineCameraControls(OfxParamSetHandle set)
{
    defineDouble(set, "vignette_strength", "Vignette Strength",
        "Затемняет изображение по краям до заданной доли. 0 — выключено; начните с 0,10–0,15, значение 0,6 уже выраженное. Радиус и ширину перехода задают Edge Start и Edge End. Альфа не изменяется.", 0, 1, 0.12);
    defineDouble(set, "edge_start", "Edge Start",
        "Радиус начала виньетки в единицах половины ширины кадра. До этого радиуса центр не затемняется. Старт 0,6; меньшее значение распространяет переход ближе к центру.", 0, 4, 0.6);
    defineDouble(set, "edge_end", "Edge End",
        "Радиус полной силы виньетки в единицах половины ширины кадра. Старт 1,0; вместе с Edge Start задаёт плавность перехода. Должен быть больше Edge Start; если выходит за углы кадра, полная сила может не достигаться.", 0.01, 4, 1.0);
    OfxPropertySetHandle props = nullptr;
    require(parameters->paramDefine(set, kOfxParamTypePage, "Controls", &props), "define camera page");
    for (int index = 0; index < 3; ++index) {
        const char* name = index == 0 ? "vignette_strength" : index == 1 ? "edge_start" : "edge_end";
        require(properties->propSetString(props, kOfxParamPropPageChild, index, name), "camera page control");
    }
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
            require(properties->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, kind == EffectKind::Probe && temporalAccess ? 1 : 0), "source temporal access");
        }
    }
    OfxParamSetHandle set = nullptr;
    require(effects->getParamSet(effect, &set), "parameter set");
    if (kind == EffectKind::Camera) {
        defineCameraControls(set);
        return kOfxStatOK;
    }
    if (kind != EffectKind::Probe) {
        defineFilmControls(set, kind);
        return kOfxStatOK;
    }
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
    require(parameters->paramDefine(set, kOfxParamTypePage, "Controls", &props), "define page");
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
    if (kind == EffectKind::Probe) {
        require(parameters->paramGetHandle(set, gainName, &state->gain, nullptr), "gain parameter");
        require(parameters->paramGetHandle(set, mixName, &state->mix, nullptr), "mix parameter");
        require(parameters->paramGetHandle(set, offsetName, &state->offset, nullptr), "offset parameter");
    } else if (kind == EffectKind::Camera) {
        require(parameters->paramGetHandle(set, "vignette_strength", &state->vignetteStrength, nullptr), "camera vignette strength parameter");
        require(parameters->paramGetHandle(set, "edge_start", &state->edgeStart, nullptr), "camera edge start parameter");
        require(parameters->paramGetHandle(set, "edge_end", &state->edgeEnd, nullptr), "camera edge end parameter");
    } else {
        require(parameters->paramGetHandle(set, "exposure_ev", &state->exposure, nullptr), "film exposure parameter");
        require(parameters->paramGetHandle(set, "contrast", &state->contrast, nullptr), "film contrast parameter");
        require(parameters->paramGetHandle(set, "shadow_softness", &state->shadow, nullptr), "film shadow parameter");
        require(parameters->paramGetHandle(set, "highlight_softness", &state->highlight, nullptr), "film highlight parameter");
        require(parameters->paramGetHandle(set, "fog_strength", &state->fog, nullptr), "film fog parameter");
        require(parameters->paramGetHandle(set, "grain_strength", &state->grainStrength, nullptr), "grain strength parameter");
        require(parameters->paramGetHandle(set, "grain_size", &state->grainSize, nullptr), "grain size parameter");
        require(parameters->paramGetHandle(set, "grain_seed", &state->grainSeed, nullptr), "grain seed parameter");
        require(parameters->paramGetHandle(set, "encoding", &state->encoding, nullptr), "film encoding parameter");
        if (kind == EffectKind::Negative) require(parameters->paramGetHandle(set, "preview_positive", &state->positivePreview, nullptr), "film preview parameter");
    }
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

/** Tell the host which source frame range is needed for this particular request. */
OfxStatus framesNeeded(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    auto& state = instance(effect);
    if (state.kind != EffectKind::Probe) return kOfxStatReplyDefault;
    const double time = actionTime(in);
    const auto settings = settingsAt(state, time);
    const double range[2] = {settings.mix > 0 ? sourceTime(state.source, time - settings.offset) : time,
                             settings.mix > 0 ? sourceTime(state.source, time + settings.offset) : time};
    require(properties->propSetDoubleN(out, "OfxImageClipPropFrameRange_Source", 2, range), "frames needed");
    return kOfxStatOK;
}

/** Let Resolve bypass an exactly neutral probe without fetching any images. */
OfxStatus identity(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    if (instance(effect).kind != EffectKind::Probe) return kOfxStatReplyDefault;
    const double time = actionTime(in);
    const auto settings = settingsAt(instance(effect), time);
    if (settings.gainEV != 0 || settings.mix != 0) return kOfxStatReplyDefault;
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
    const bool isEmulsion = state.kind == EffectKind::Negative || state.kind == EffectKind::Print;
    const auto settings = isProbe ? settingsAt(state, time) : Settings{};
    auto emulsion = isEmulsion ? filmSettingsAt(state, time) : silentfilm::EmulsionParameters{};
    const auto camera = isCamera ? cameraSettingsAt(state, time) : silentfilm::CameraOpticsParameters{};
    int cudaEnabled = 0;
    optionalProperty(properties->propGetInt(in, kOfxImageEffectPropCudaEnabled, 0, &cudaEnabled), "CUDA render mode");
#ifndef SILENTFILM_ENABLE_CUDA
    if (cudaEnabled) throw Failure(kOfxStatErrUnsupported, "This plugin was built without CUDA support");
#endif
    if (effects->abort(effect)) return kOfxStatOK;
    Image output(state.output, time);
    Image current(state.source, time);
    if (isEmulsion) {
        emulsion.sourcePremultiplied = current.premultiplied();
        emulsion.outputPremultiplied = output.premultiplied();
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
            : isCamera
            ? silentfilm::renderCudaCamera(current.view(), output.view(), region, camera, stream, streamStatus == kOfxStatOK)
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
                : isCamera
                ? silentfilm::cameraPixel(center, x, y, currentView.x1, currentView.y1,
                    currentView.x2, currentView.y2, camera)
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
        if (std::strcmp(action, kOfxActionLoad) == 0) return load();
        if (std::strcmp(action, kOfxActionUnload) == 0) return kOfxStatOK;
        if (!properties || !effects || !parameters) return kOfxStatErrMissingHostFeature;
        if (std::strcmp(action, kOfxActionDescribe) == 0) return describe(effect, kind);
        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) return describeContext(effect, kind);
        if (std::strcmp(action, kOfxActionCreateInstance) == 0) return create(effect, kind);
        if (std::strcmp(action, kOfxActionDestroyInstance) == 0) return destroy(effect);
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
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.hostprobe", 0, 4, setHost, entry<EffectKind::Probe>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.camera", 0, 4, setHost, entry<EffectKind::Camera>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.negative", 0, 4, setHost, entry<EffectKind::Negative>},
    {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.print", 0, 4, setHost, entry<EffectKind::Print>},
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
