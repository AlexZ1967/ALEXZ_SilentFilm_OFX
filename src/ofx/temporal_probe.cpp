#include <ofxImageEffect.h>
#include <ofxProperty.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
OfxHost* host = nullptr;
const OfxPropertySuiteV1* props = nullptr;
const OfxImageEffectSuiteV1* suite = nullptr;
const OfxParameterSuiteV1* params = nullptr;
struct Control { const char* id; const char* label; int defaultValue; int minimum; };
constexpr std::array<Control, 3> controls{{
    {"start_frame", "Start Frame (OFX)", 0, -1000000000},
    {"trimmed_end_frame", "Trimmed Frame (Last)", 0, -1000000000},
    {"full_source_end_frame", "Full Source Frame (Last)", 199, -1000000000},
}};
struct Instance {
    OfxImageClipHandle source = nullptr, output = nullptr;
    std::array<OfxParamHandle, controls.size()> timing{};
    OfxParamHandle autoTiming = nullptr;
    OfxParamHandle timingFile = nullptr;
    std::atomic<std::uint64_t> bridgeTiming{0};
};
constexpr const char* autoTimingName = "auto_timing";
constexpr const char* timingFileName = "timeline_timing_file";
constexpr auto invalidTiming = std::uint64_t{0xffffffffffffffffull};

template<class T> OfxStatus getSuite(const char* name, T*& result)
{
    result = static_cast<T*>(host->fetchSuite(host->host, name, 1));
    return result ? kOfxStatOK : kOfxStatErrMissingHostFeature;
}

OfxStatus load()
{
    if (!host || !host->fetchSuite) return kOfxStatErrMissingHostFeature;
    auto status = getSuite(kOfxPropertySuite, props);
    if (status != kOfxStatOK) return status;
    status = getSuite(kOfxImageEffectSuite, suite);
    if (status != kOfxStatOK) return status;
    return getSuite(kOfxParameterSuite, params);
}

OfxStatus describe(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle p = nullptr;
    auto status = suite->getPropertySet(effect, &p);
    if (status != kOfxStatOK) return status;
    props->propSetString(p, kOfxPropLabel, 0, "Temporal Probe");
    props->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "ALEXZ/SilentFilm/Diagnostics");
    props->propSetString(p, kOfxPropPluginDescription, 0,
        "Проверяет доступ Resolve к исходным кадрам за концом клипа. Красный — запросы за концом недоступны, зелёный — доступны.");
    props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    props->propSetInt(p, kOfxImageEffectPropSupportsTiles, 0, 0);
    props->propSetInt(p, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    return kOfxStatOK;
}

OfxStatus describeContext(OfxImageEffectHandle effect, OfxPropertySetHandle in)
{
    char* context = nullptr;
    if (props->propGetString(in, kOfxImageEffectPropContext, 0, &context) != kOfxStatOK
        || !context || std::strcmp(context, kOfxImageEffectContextFilter) != 0) return kOfxStatErrUnsupported;
    auto status = kOfxStatOK;
    OfxPropertySetHandle clip = nullptr;
    status = suite->clipDefine(effect, "Source", &clip);
    if (status != kOfxStatOK) return status;
    props->propSetInt(clip, kOfxImageClipPropOptional, 0, 0);
    props->propSetInt(clip, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    props->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    status = suite->clipDefine(effect, "Output", &clip);
    if (status != kOfxStatOK) return status;
    props->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    OfxParamSetHandle set = nullptr;
    status = suite->getParamSet(effect, &set);
    if (status != kOfxStatOK) return status;
    OfxPropertySetHandle page = nullptr, automatic = nullptr;
    status = params->paramDefine(set, kOfxParamTypePage, "timing", &page);
    if (status != kOfxStatOK) return status;
    props->propSetString(page, kOfxPropLabel, 0, "Timing");
    status = params->paramDefine(set, kOfxParamTypeBoolean, autoTimingName, &automatic);
    if (status != kOfxStatOK) return status;
    props->propSetString(automatic, kOfxPropLabel, 0, "Auto Timing");
    props->propSetInt(automatic, kOfxParamPropAnimates, 0, 0);
    props->propSetInt(automatic, kOfxParamPropDefault, 0, 1);
    props->propSetString(page, kOfxParamPropPageChild, 0, autoTimingName);
    OfxPropertySetHandle file = nullptr;
    status = params->paramDefine(set, kOfxParamTypeString, timingFileName, &file);
    if (status != kOfxStatOK) return status;
    props->propSetString(file, kOfxPropLabel, 0, "Timeline Timing File");
    props->propSetString(file, kOfxParamPropStringMode, 0, kOfxParamStringIsFilePath);
    props->propSetString(file, kOfxParamPropDefault, 0, "");
    props->propSetInt(file, kOfxParamPropAnimates, 0, 0);
    props->propSetString(page, kOfxParamPropPageChild, 1, timingFileName);
    for (std::size_t i = 0; i < controls.size(); ++i) {
        const auto& control = controls[i];
        OfxPropertySetHandle descriptor = nullptr;
        status = params->paramDefine(set, kOfxParamTypeInteger, control.id, &descriptor);
        if (status != kOfxStatOK) return status;
        props->propSetString(descriptor, kOfxPropLabel, 0, control.label);
        props->propSetInt(descriptor, kOfxParamPropAnimates, 0, 0);
        props->propSetInt(descriptor, kOfxParamPropDefault, 0, control.defaultValue);
        props->propSetInt(descriptor, kOfxParamPropMin, 0, control.minimum);
        props->propSetInt(descriptor, kOfxParamPropMax, 0, 1000000000);
        props->propSetString(page, kOfxParamPropPageChild, static_cast<int>(i + 2), control.id);
    }
    return kOfxStatOK;
}

/** Read the two frame counts from the Resolve script's bounded file format. */
std::uint64_t readTimingFile(const char* path)
{
    auto* file = std::fopen(path, "rb");
    if (!file) return invalidTiming;
    char bytes[96]{};
    const auto size = std::fread(bytes, 1, sizeof(bytes), file);
    const bool complete = std::feof(file) && !std::ferror(file);
    std::fclose(file);
    constexpr char header[] = "TemporalProbeTiming 1\n";
    constexpr auto headerSize = sizeof(header) - 1;
    if (!complete || size <= headerSize || std::memcmp(bytes, header, headerSize)) return invalidTiming;
    const char* end = bytes + size;
    unsigned int full = 0, trimmed = 0;
    const auto first = std::from_chars(bytes + headerSize, end, full);
    if (first.ec != std::errc{} || first.ptr == end || *first.ptr != ' ') return invalidTiming;
    const auto second = std::from_chars(first.ptr + 1, end, trimmed);
    if (second.ec != std::errc{} || end - second.ptr != 1 || *second.ptr != '\n'
        || trimmed < 2 || trimmed >= full || full > 1000000000u) return invalidTiming;
    return (static_cast<std::uint64_t>(full) << 32) | trimmed;
}

/** Publish new timing on the parameter action, outside render. */
OfxStatus refreshTiming(Instance& state)
{
    char* path = nullptr;
    const auto status = params->paramGetValue(state.timingFile, &path);
    if (status != kOfxStatOK) {
        state.bridgeTiming.store(invalidTiming);
        return status;
    }
    state.bridgeTiming.store(path && *path ? readTimingFile(path) : 0);
    return kOfxStatOK;
}

OfxStatus create(OfxImageEffectHandle effect)
{
    auto* state = new (std::nothrow) Instance;
    if (!state) return kOfxStatErrMemory;
    auto status = suite->clipGetHandle(effect, "Source", &state->source, nullptr);
    if (status == kOfxStatOK) status = suite->clipGetHandle(effect, "Output", &state->output, nullptr);
    OfxParamSetHandle set = nullptr;
    if (status == kOfxStatOK) status = suite->getParamSet(effect, &set);
    if (status == kOfxStatOK) status = params->paramGetHandle(set, autoTimingName, &state->autoTiming, nullptr);
    if (status == kOfxStatOK) status = params->paramGetHandle(set, timingFileName, &state->timingFile, nullptr);
    for (std::size_t i = 0; i < controls.size() && status == kOfxStatOK; ++i)
        status = params->paramGetHandle(set, controls[i].id, &state->timing[i], nullptr);
    if (status == kOfxStatOK) status = refreshTiming(*state);
    OfxPropertySetHandle p = nullptr;
    if (status == kOfxStatOK) status = suite->getPropertySet(effect, &p);
    if (status == kOfxStatOK) status = props->propSetPointer(p, kOfxPropInstanceData, 0, state);
    if (status != kOfxStatOK) delete state;
    return status;
}

OfxStatus changed(OfxImageEffectHandle effect, OfxPropertySetHandle in)
{
    char* name = nullptr;
    if (props->propGetString(in, kOfxPropName, 0, &name) != kOfxStatOK
        || !name || std::strcmp(name, timingFileName)) return kOfxStatReplyDefault;
    OfxPropertySetHandle p = nullptr;
    auto status = suite->getPropertySet(effect, &p);
    void* instance = nullptr;
    if (status == kOfxStatOK) status = props->propGetPointer(p, kOfxPropInstanceData, 0, &instance);
    if (status != kOfxStatOK || !instance) return kOfxStatErrBadHandle;
    return refreshTiming(*static_cast<Instance*>(instance));
}

OfxStatus destroy(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle p = nullptr;
    auto status = suite->getPropertySet(effect, &p);
    if (status != kOfxStatOK) return status;
    void* value = nullptr;
    status = props->propGetPointer(p, kOfxPropInstanceData, 0, &value);
    if (status == kOfxStatOK) delete static_cast<Instance*>(value);
    return status;
}

struct Request { const char* name; double time; };
constexpr std::size_t requestCount = 6;

/** List source requests in OFX frame units for both actions. */
std::array<Request, requestCount> requests(double current, double maximum)
{
    return {{{"current", current}, {"current+10", current + 10}, {"current+60", current + 60},
             {"max", maximum}, {"max+10", maximum + 10}, {"max+60", maximum + 60}}};
}

struct ImageView {
    unsigned char* data = nullptr;
    int bounds[4]{};
    int stride = 0;

    /** Read a CPU float RGBA image layout. */
    bool read(OfxPropertySetHandle image)
    {
        void* pixels = nullptr;
        char* depth = nullptr;
        char* components = nullptr;
        if (props->propGetPointer(image, kOfxImagePropData, 0, &pixels) != kOfxStatOK || !pixels
            || props->propGetIntN(image, kOfxImagePropBounds, 4, bounds) != kOfxStatOK
            || props->propGetInt(image, kOfxImagePropRowBytes, 0, &stride) != kOfxStatOK
            || props->propGetString(image, kOfxImageEffectPropPixelDepth, 0, &depth) != kOfxStatOK
            || !depth || std::strcmp(depth, kOfxBitDepthFloat)
            || props->propGetString(image, kOfxImageEffectPropComponents, 0, &components) != kOfxStatOK
            || !components || std::strcmp(components, kOfxImageComponentRGBA)) return false;
        const auto width = static_cast<std::ptrdiff_t>(bounds[2]) - bounds[0];
        const auto pitch = static_cast<std::ptrdiff_t>(stride);
        if (width <= 0 || bounds[3] <= bounds[1] || width > (pitch < 0 ? -pitch : pitch) / pixelBytes)
            return false;
        data = static_cast<unsigned char*>(pixels);
        return true;
    }

    static constexpr auto pixelBytes = static_cast<std::ptrdiff_t>(4 * sizeof(float));

    unsigned char* pixel(int x, int y) const
    {
        return data + (static_cast<std::ptrdiff_t>(y) - bounds[1]) * stride
                    + (static_cast<std::ptrdiff_t>(x) - bounds[0]) * pixelBytes;
    }
};

/** Hash a grid of source pixels without retaining the image. */
bool fingerprint(OfxPropertySetHandle image, std::uint64_t& hash)
{
    ImageView view;
    if (!view.read(image)) return false;
    const auto width = static_cast<std::ptrdiff_t>(view.bounds[2]) - view.bounds[0];
    const auto height = static_cast<std::ptrdiff_t>(view.bounds[3]) - view.bounds[1];
    hash = 14695981039346656037ull;
    constexpr int grid = 32;
    for (int y = 0; y < grid; ++y) {
        const auto sampleY = static_cast<int>(view.bounds[1] + ((2 * y + 1) * height / (2 * grid)));
        for (int x = 0; x < grid; ++x) {
            const auto sampleX = static_cast<int>(view.bounds[0] + ((2 * x + 1) * width / (2 * grid)));
            const auto* pixel = view.pixel(sampleX, sampleY);
            for (std::ptrdiff_t i = 0; i < ImageView::pixelBytes; ++i) {
                hash ^= pixel[i];
                hash *= 1099511628211ull;
            }
        }
    }
    return true;
}

bool fetch(OfxImageClipHandle source, const Request& request, std::array<char, 128>& log)
{
    OfxPropertySetHandle image = nullptr;
    const auto status = suite->clipGetImage(source, request.time, nullptr, &image);
    const bool returned = status == kOfxStatOK && image;
    std::uint64_t hash = 0;
    char hashText[17] = "-";
    if (returned && fingerprint(image, hash))
        std::snprintf(hashText, sizeof(hashText), "%016llx", static_cast<unsigned long long>(hash));
    if (returned) suite->clipReleaseImage(image);
    std::snprintf(log.data(), log.size(), "%s@%.9g:%s(status=%d,hash=%s)",
        request.name, request.time, returned ? "ok" : "fail", status, hashText);
    return returned;
}

struct Stretch {
    bool enabled = false, ready = false, automatic = false;
    double sourceTime = 0;
    double source[2]{}, output[2]{}, duration = 0;
    bool sourceKnown = false, outputKnown = false, durationKnown = false;
    const char* timing = "manual";
    double fullFrames = 0, trimmedFrames = 0;
    double start = 0, trimmedEnd = 0, fullEnd = 0;
};

/** Read a finite clip range in OFX frame units. */
bool clipRange(OfxImageClipHandle clip, double range[2])
{
    OfxPropertySetHandle p = nullptr;
    return suite->clipGetPropertySet(clip, &p) == kOfxStatOK && p
        && props->propGetDoubleN(p, kOfxImageEffectPropFrameRange, 2, range) == kOfxStatOK
        && std::isfinite(range[0]) && std::isfinite(range[1]) && range[0] <= range[1];
}

/** Map inclusive OFX endpoints, sampling the nearest source frame. */
void mapEndpoints(Stretch& stretch, double time, double start, double trimmedEnd, double fullEnd)
{
    stretch.start = start;
    stretch.trimmedEnd = trimmedEnd;
    stretch.fullEnd = fullEnd;
    if (trimmedEnd <= start || fullEnd < trimmedEnd) return;
    stretch.trimmedFrames = trimmedEnd - start + 1;
    stretch.fullFrames = fullEnd - start + 1;
    if (time < start - 1e-6 || time > trimmedEnd + 1e-6) return;
    const double progress = std::clamp((time - start) / (trimmedEnd - start), 0.0, 1.0);
    stretch.sourceTime = std::min(fullEnd, start + std::round(progress * (fullEnd - start)));
    stretch.ready = true;
}

/** Resolve the endpoints for manual, script, or OFX timing. */
OfxStatus stretchTime(const Instance& state, OfxPropertySetHandle effect, double time, Stretch& stretch)
{
    int automatic = 0;
    auto status = params->paramGetValue(state.autoTiming, &automatic);
    if (status != kOfxStatOK) return status;
    if (automatic) {
        stretch.automatic = stretch.enabled = true;
        stretch.sourceKnown = clipRange(state.source, stretch.source);
        const auto supplied = state.bridgeTiming.load();
        if (supplied) {
            stretch.timing = supplied == invalidTiming ? "invalid-file" : "resolve-script";
            if (supplied == invalidTiming || !stretch.sourceKnown) return kOfxStatOK;
            const double full = static_cast<double>(supplied >> 32);
            const double trimmed = static_cast<double>(supplied & 0xffffffffu);
            const double start = stretch.source[0];
            mapEndpoints(stretch, time, start, start + trimmed - 1, start + full - 1);
            return kOfxStatOK;
        }
        stretch.timing = "ofx";
        stretch.outputKnown = clipRange(state.output, stretch.output);
        stretch.durationKnown = props->propGetDouble(effect, kOfxImageEffectInstancePropEffectDuration, 0,
            &stretch.duration) == kOfxStatOK && std::isfinite(stretch.duration) && stretch.duration >= 2;
        if (!stretch.sourceKnown || !stretch.outputKnown
            || std::abs(stretch.source[0] - stretch.output[0]) > 1e-6) return kOfxStatOK;
        const double full = stretch.source[1] - stretch.source[0] + 1;
        double trimmed = stretch.output[1] - stretch.output[0] + 1;
        if (stretch.durationKnown) {
            // Если Output повторяет весь Source, отдельная длительность может задать конец подрезки.
            if (std::abs(trimmed - full) <= 1e-6) trimmed = stretch.duration;
            else if (std::abs(trimmed - stretch.duration) > 1e-6) return kOfxStatOK;
        }
        if (trimmed < 2 || trimmed >= full) return kOfxStatOK;
        mapEndpoints(stretch, time, stretch.output[0], stretch.output[0] + trimmed - 1, stretch.source[1]);
        return kOfxStatOK;
    }
    int start = 0, trimmedEnd = 0, fullEnd = 0;
    status = params->paramGetValue(state.timing[0], &start);
    if (status == kOfxStatOK) status = params->paramGetValue(state.timing[1], &trimmedEnd);
    if (status == kOfxStatOK) status = params->paramGetValue(state.timing[2], &fullEnd);
    if (status != kOfxStatOK) return status;
    if (trimmedEnd == start) return kOfxStatOK;
    stretch.enabled = true;
    mapEndpoints(stretch, time, start, trimmedEnd, fullEnd);
    return kOfxStatOK;
}

struct HostImage {
    OfxPropertySetHandle handle = nullptr;
    ~HostImage() { if (handle) suite->clipReleaseImage(handle); }
};

OfxStatus renderStretch(const Instance& state, double time, const Stretch& stretch, const int window[4])
{
    HostImage source, output;
    const auto result = stretch.ready
        ? suite->clipGetImage(state.source, stretch.sourceTime, nullptr, &source.handle)
        : kOfxStatReplyDefault;
    ImageView input;
    const bool readable = result == kOfxStatOK && source.handle && input.read(source.handle);
    if (auto* log = std::fopen("/tmp/TemporalProbe.log", "a")) {
        std::uint64_t hash = 0;
        char hashText[17] = "-";
        if (readable && fingerprint(source.handle, hash))
            std::snprintf(hashText, sizeof(hashText), "%016llx", static_cast<unsigned long long>(hash));
        std::fprintf(log, "probe=1.5 mode=stretch auto=%d timing=%s start=%.12g trimmed_end=%.12g source_end=%.12g "
            "full=%.12g trimmed=%.12g "
            "ready=%d time=%.12g request=%.12g status=%d readable=%d hash=%s "
            "source=%d[%.12g,%.12g] output=%d[%.12g,%.12g] duration=%d:%.12g\n",
            stretch.automatic, stretch.timing, stretch.start, stretch.trimmedEnd, stretch.fullEnd,
            stretch.fullFrames, stretch.trimmedFrames,
            stretch.ready, time, stretch.sourceTime, result, readable, hashText,
            stretch.sourceKnown, stretch.source[0], stretch.source[1], stretch.outputKnown,
            stretch.output[0], stretch.output[1], stretch.durationKnown, stretch.duration);
        std::fclose(log);
    }
    const auto status = suite->clipGetImage(state.output, time, nullptr, &output.handle);
    if (status != kOfxStatOK) return status;
    ImageView destination;
    if (!output.handle || !destination.read(output.handle)) return kOfxStatFailed;

    const float missing[] = {1, 0, 0, 1};
    for (int y = std::max(window[1], destination.bounds[1]); y < std::min(window[3], destination.bounds[3]); ++y) {
        for (int x = std::max(window[0], destination.bounds[0]); x < std::min(window[2], destination.bounds[2]); ++x) {
            const bool covered = readable && x >= input.bounds[0] && x < input.bounds[2]
                && y >= input.bounds[1] && y < input.bounds[3];
            const void* pixel = covered ? static_cast<const void*>(input.pixel(x, y)) : missing;
            std::memcpy(destination.pixel(x, y), pixel, sizeof(missing));
        }
    }
    return kOfxStatOK;
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in)
{
    double time = 0;
    int renderWindow[4]{};
    if (props->propGetDouble(in, kOfxPropTime, 0, &time) != kOfxStatOK
        || props->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, renderWindow) != kOfxStatOK) return kOfxStatErrValue;
    OfxPropertySetHandle effectProps = nullptr, sourceProps = nullptr, dst = nullptr;
    auto status = suite->getPropertySet(effect, &effectProps);
    void* opaque = nullptr;
    if (status == kOfxStatOK) status = props->propGetPointer(effectProps, kOfxPropInstanceData, 0, &opaque);
    if (status != kOfxStatOK || !opaque) return kOfxStatErrBadHandle;
    auto& state = *static_cast<Instance*>(opaque);
    if (!std::isfinite(time)) return kOfxStatErrValue;
    Stretch stretch;
    status = stretchTime(state, effectProps, time, stretch);
    if (status != kOfxStatOK) return status;
    if (stretch.enabled) return renderStretch(state, time, stretch, renderWindow);
    status = suite->clipGetPropertySet(state.source, &sourceProps);
    double range[2]{};
    if (status == kOfxStatOK) status = props->propGetDoubleN(sourceProps, kOfxImageEffectPropFrameRange, 2, range);
    if (status != kOfxStatOK) return status;
    if (!std::isfinite(time) || !std::isfinite(range[0]) || !std::isfinite(range[1]) || range[0] > range[1])
        return kOfxStatErrValue;
    const auto times = requests(time, range[1]);
    std::array<std::array<char, 128>, requestCount> details{};
    bool passed = false;
    for (std::size_t i = 0; i < times.size(); ++i) {
        const bool returned = fetch(state.source, times[i], details[i]);
        if (i >= 4 && returned) passed = true;
    }
    std::FILE* log = std::fopen("/tmp/TemporalProbe.log", "a");
    if (log) {
        std::fprintf(log, "probe=1.5 mode=probe instance=%p time=%.9g range=[%.9g,%.9g] %s %s %s %s %s %s\n",
            static_cast<void*>(&state), time, range[0], range[1], details[0].data(), details[1].data(),
            details[2].data(), details[3].data(), details[4].data(), details[5].data());
        std::fflush(log); std::fclose(log);
    }
    status = suite->clipGetImage(state.output, time, nullptr, &dst);
    if (status != kOfxStatOK || !dst) return status == kOfxStatOK ? kOfxStatFailed : status;
    void* data = nullptr;
    int bounds[4]{}, rowBytes = 0;
    status = props->propGetPointer(dst, kOfxImagePropData, 0, &data);
    if (status == kOfxStatOK) status = props->propGetIntN(dst, kOfxImagePropBounds, 4, bounds);
    if (status == kOfxStatOK) status = props->propGetInt(dst, kOfxImagePropRowBytes, 0, &rowBytes);
    if (status == kOfxStatOK && data) {
        auto* pixels = static_cast<float*>(data);
        for (int y = std::max(bounds[1], renderWindow[1]); y < std::min(bounds[3], renderWindow[3]); ++y) {
            auto* row = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(pixels) + (y - bounds[1]) * rowBytes);
            for (int x = std::max(bounds[0], renderWindow[0]); x < std::min(bounds[2], renderWindow[2]); ++x) {
                auto* px = row + (x - bounds[0]) * 4;
                px[0] = passed ? 0.0f : 1.0f; px[1] = passed ? 1.0f : 0.0f; px[2] = 0.0f; px[3] = 1.0f;
            }
        }
    }
    suite->clipReleaseImage(dst);
    return status;
}

OfxStatus framesNeeded(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    OfxPropertySetHandle p = nullptr;
    auto status = suite->getPropertySet(effect, &p);
    void* opaque = nullptr;
    if (status == kOfxStatOK) status = props->propGetPointer(p, kOfxPropInstanceData, 0, &opaque);
    if (status != kOfxStatOK || !opaque) return kOfxStatErrBadHandle;
    auto& state = *static_cast<Instance*>(opaque);
    double time = 0;
    status = props->propGetDouble(in, kOfxPropTime, 0, &time);
    if (status != kOfxStatOK) return status;
    if (!std::isfinite(time)) return kOfxStatErrValue;
    Stretch stretch;
    status = stretchTime(state, p, time, stretch);
    if (status != kOfxStatOK) return status;
    if (stretch.enabled) {
        if (!stretch.ready) return kOfxStatReplyDefault;
        const double needed[] = {stretch.sourceTime, stretch.sourceTime};
        return props->propSetDoubleN(out, "OfxImageClipPropFrameRange_Source", 2, needed);
    }
    status = suite->clipGetPropertySet(state.source, &p);
    double range[2]{};
    if (status == kOfxStatOK) status = props->propGetDoubleN(p, kOfxImageEffectPropFrameRange, 2, range);
    if (status != kOfxStatOK) return status;
    if (!std::isfinite(time) || !std::isfinite(range[1])) return kOfxStatErrValue;
    const auto times = requests(time, range[1]);
    std::array<double, requestCount> sorted{};
    std::transform(times.begin(), times.end(), sorted.begin(), [](const Request& request) { return request.time; });
    std::sort(sorted.begin(), sorted.end());
    const auto end = std::unique(sorted.begin(), sorted.end());
    std::array<double, 2 * requestCount> needed{};
    int count = 0;
    for (auto it = sorted.begin(); it != end; ++it) {
        needed[count++] = *it;
        needed[count++] = *it;
    }
    return props->propSetDoubleN(out, "OfxImageClipPropFrameRange_Source", count, needed.data());
}

OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    if (!action) return kOfxStatErrValue;
    if (!std::strcmp(action, kOfxActionLoad)) return load();
    if (!props || !suite || !params) return kOfxStatErrMissingHostFeature;
    auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect);
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeContext(effect, in);
    if (!std::strcmp(action, kOfxActionCreateInstance)) return create(effect);
    if (!std::strcmp(action, kOfxActionDestroyInstance)) return destroy(effect);
    if (!std::strcmp(action, kOfxActionInstanceChanged)) return changed(effect, in);
    if (!std::strcmp(action, kOfxImageEffectActionGetFramesNeeded)) return framesNeeded(effect, in, out);
    if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, in);
    return kOfxStatReplyDefault;
}

void setHost(OfxHost* value) { host = value; }
OfxPlugin plugin = {kOfxImageEffectPluginApi, 1, "com.alexz.silentfilm.temporalprobe", 1, 5, setHost, entry};

} // namespace

#if defined(_WIN32)
#define ALEXZ_EXPORT __declspec(dllexport)
#else
#define ALEXZ_EXPORT __attribute__((visibility("default")))
#endif

extern "C" ALEXZ_EXPORT int OfxGetNumberOfPlugins() { return 1; }
extern "C" ALEXZ_EXPORT OfxPlugin* OfxGetPlugin(int index) { return index == 0 ? &plugin : nullptr; }
