// Проверяем настоящий adapter с минимальным host double, без запуска Resolve.
#include "../src/ofx/plugin.cpp"

#include <cstdarg>
#include <filesystem>
#include <iostream>

namespace fixture {
struct Param {
    std::string id, text;
    bool integer = false;
    double value = 0, minimum = 0, maximum = 1;
    unsigned int keys = 0;
};
int effectToken = 0, setToken = 0, propertyToken = 0, clipToken = 0;
void* instanceData = nullptr;
int resetCalls = 0, optionWrites = 0, valueWrites = 0, propertyReads = 0, artisticPropertyReads = 0, numericWrites = 0, edits = 0;
std::vector<std::string> labels{"—"};
std::map<std::string, Param> params;
std::string callbackName;

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Param& param(OfxParamHandle handle) { return *reinterpret_cast<Param*>(handle); }
OfxStatus effectProperties(OfxImageEffectHandle, OfxPropertySetHandle* value)
{ *value = reinterpret_cast<OfxPropertySetHandle>(&propertyToken); return kOfxStatOK; }
OfxStatus parameterSet(OfxImageEffectHandle, OfxParamSetHandle* value)
{ *value = reinterpret_cast<OfxParamSetHandle>(&setToken); return kOfxStatOK; }
OfxStatus clip(OfxImageEffectHandle, const char*, OfxImageClipHandle* value, OfxPropertySetHandle*)
{ *value = reinterpret_cast<OfxImageClipHandle>(&clipToken); return kOfxStatOK; }
OfxStatus parameter(OfxParamSetHandle, const char* name, OfxParamHandle* value, OfxPropertySetHandle* props)
{
    auto found = params.find(name);
    if (found == params.end()) return kOfxStatErrUnknown;
    *value = reinterpret_cast<OfxParamHandle>(&found->second);
    // Намеренно не предоставляем properties через GetHandle: Save не должен от них зависеть.
    if (props) *props = nullptr;
    return kOfxStatOK;
}
OfxStatus storePointer(OfxPropertySetHandle, const char*, int, void* value)
{ instanceData = value; return kOfxStatOK; }
OfxStatus readPointer(OfxPropertySetHandle, const char*, int, void** value)
{ *value = instanceData; return kOfxStatOK; }
OfxStatus parameterProperties(OfxParamHandle handle, OfxPropertySetHandle* value)
{
    ++propertyReads;
    if (param(handle).id != "user_preset") ++artisticPropertyReads;
    *value = reinterpret_cast<OfxPropertySetHandle>(handle); return kOfxStatOK;
}
OfxStatus reset(OfxPropertySetHandle, const char*)
{ ++resetCalls; return kOfxStatErrUnsupported; }
OfxStatus dimension(OfxPropertySetHandle, const char*, int* count)
{ *count = static_cast<int>(labels.size()); return kOfxStatOK; }
OfxStatus options(OfxPropertySetHandle, const char*, int count, const char* const* values)
{
    check(count >= static_cast<int>(labels.size()) && count >= 1 && params.at("user_preset").value == 0,
        "Preset Choice must remain nonempty and keep valid indices");
    ++optionWrites; labels.clear();
    for (int i = 0; i < count; ++i) labels.emplace_back(values[i]);
    return kOfxStatOK;
}
OfxStatus stringProperty(OfxPropertySetHandle, const char* key, int, char** value)
{
    if (std::strcmp(key, kOfxPropName) != 0) return kOfxStatErrUnknown;
    *value = callbackName.data(); return kOfxStatOK;
}
OfxStatus doubleProperty(OfxPropertySetHandle props, const char* key, int, double* value)
{
    auto& p = *reinterpret_cast<Param*>(props);
    if (std::strcmp(key, kOfxParamPropMin) == 0) *value = p.minimum;
    else if (std::strcmp(key, kOfxParamPropMax) == 0) *value = p.maximum;
    else return kOfxStatErrUnknown;
    return kOfxStatOK;
}
OfxStatus intProperty(OfxPropertySetHandle props, const char* key, int, int* value)
{
    auto& p = *reinterpret_cast<Param*>(props);
    if (std::strcmp(key, kOfxParamPropAnimates) == 0) *value = 1;
    else if (std::strcmp(key, kOfxParamPropMin) == 0) *value = static_cast<int>(p.minimum);
    else if (std::strcmp(key, kOfxParamPropMax) == 0) *value = static_cast<int>(p.maximum);
    else return kOfxStatErrUnknown;
    return kOfxStatOK;
}
OfxStatus getValue(OfxParamHandle handle, ...)
{
    va_list args; va_start(args, handle);
    if (param(handle).id == "user_preset") *va_arg(args, int*) = static_cast<int>(param(handle).value);
    else *va_arg(args, char**) = param(handle).text.data();
    va_end(args); return kOfxStatOK;
}
OfxStatus getValueAtTime(OfxParamHandle handle, OfxTime time, ...)
{
    va_list args; va_start(args, time);
    const auto& p = param(handle);
    if (p.integer) *va_arg(args, int*) = static_cast<int>(p.value);
    else *va_arg(args, double*) = p.value;
    va_end(args); return kOfxStatOK;
}
OfxStatus setValue(OfxParamHandle handle, ...)
{
    ++valueWrites;
    auto& p = param(handle);
    va_list args; va_start(args, handle);
    if (p.id == "user_preset") p.value = va_arg(args, int);
    else if (p.id.rfind("user_", 0) == 0) p.text = va_arg(args, const char*);
    else { ++numericWrites; p.value = p.integer ? static_cast<double>(va_arg(args, int)) : va_arg(args, double); }
    va_end(args);
    if (instanceData) {
        callbackName = p.id;
        check(changed(reinterpret_cast<OfxImageEffectHandle>(&effectToken), reinterpret_cast<OfxPropertySetHandle>(&propertyToken)) == kOfxStatOK,
            "Reentrant UI notification was not guarded");
    }
    return kOfxStatOK;
}
OfxStatus keys(OfxParamHandle handle, unsigned int* count) { *count = param(handle).keys; return kOfxStatOK; }
OfxStatus begin(OfxParamSetHandle, const char*) { ++edits; return kOfxStatOK; }
OfxStatus end(OfxParamSetHandle) { return kOfxStatOK; }
} // namespace fixture

int main(int argc, char** argv)
{
    try {
        fixture::check(argc == 2, "Pass the test output directory");
        const auto root = std::filesystem::path(argv[1]) / ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{root};
        for (const auto& pair : std::map<std::string, double>{{"camera_softness", 0.15}, {"edge_end", 1.3}, {"edge_start", 0.6},
                {"edge_softness", 0.05}, {"focus_drift", 0.04}, {"focus_seed", 1925}, {"vignette_strength", 0.6}}) {
            fixture::Param p; p.id = pair.first; p.value = pair.second;
            p.integer = p.id == "focus_seed";
            p.maximum = p.integer ? 2147483647.0 : p.id == "edge_end" || p.id == "edge_start" ? 4.0 : 1.0;
            fixture::params.emplace(p.id, p);
        }
        for (const char* id : {"user_preset", "user_preset_name", "user_preset_file", "user_preset_catalog", "user_preset_status"}) {
            fixture::Param p; p.id = id; fixture::params.emplace(id, p);
        }
        OfxPropertySuiteV1 propertySuite{};
        propertySuite.propSetPointer = fixture::storePointer; propertySuite.propGetPointer = fixture::readPointer;
        propertySuite.propReset = fixture::reset; propertySuite.propSetStringN = fixture::options;
        propertySuite.propGetDimension = fixture::dimension;
        propertySuite.propGetString = fixture::stringProperty;
        propertySuite.propGetDouble = fixture::doubleProperty; propertySuite.propGetInt = fixture::intProperty;
        OfxImageEffectSuiteV1 effectSuite{};
        effectSuite.getPropertySet = fixture::effectProperties; effectSuite.getParamSet = fixture::parameterSet;
        effectSuite.clipGetHandle = fixture::clip;
        OfxParameterSuiteV1 parameterSuite{};
        parameterSuite.paramGetHandle = fixture::parameter; parameterSuite.paramGetPropertySet = fixture::parameterProperties;
        parameterSuite.paramSetValue = fixture::setValue; parameterSuite.paramGetValue = fixture::getValue;
        parameterSuite.paramGetValueAtTime = fixture::getValueAtTime; parameterSuite.paramGetNumKeys = fixture::keys;
        parameterSuite.paramEditBegin = fixture::begin; parameterSuite.paramEditEnd = fixture::end;
        properties = &propertySuite; effects = &effectSuite; parameters = &parameterSuite;
        const auto effect = reinterpret_cast<OfxImageEffectHandle>(&fixture::effectToken);
        fixture::check(create(effect, EffectKind::CameraOptics) == kOfxStatOK, "CreateInstance failed");
        fixture::check(fixture::instanceData && fixture::resetCalls == 0 && fixture::optionWrites == 0
            && fixture::valueWrites == 0 && fixture::propertyReads == 0, "CreateInstance must not mutate the preset UI");
        auto& state = instance(effect);
        fixture::params.at("user_preset_name").text = "Моя оптика";
        userPresetChanged(effect, state, "save_user_preset", 12, root.string());
        const auto file = root / "com.alexz.silentfilm.cameraoptics" / "Моя оптика.sfpreset";
        fixture::check(std::filesystem::is_regular_file(file), "Camera Optics Save with a Russian name did not write its file");
        fixture::check(fixture::artisticPropertyReads == 0 && fixture::optionWrites == 1 && fixture::resetCalls == 0
            && fixture::labels.size() == 2 && fixture::labels[1] == "Моя оптика",
            "Save must publish selectable names without introspecting artistic metadata or resetting choices");
        fixture::check(fixture::numericWrites == 0 && fixture::params.at("user_preset_file").text == file.string(),
            "Save must only update string UI fields and retain the exact Unicode path");
        const auto saved = silentfilm::loadUserPresetFile(file.string(), "com.alexz.silentfilm.cameraoptics");
        fixture::check(saved.values.size() == 7 && saved.name == "Моя оптика", "Typed Camera Optics snapshot lost parameters");
        fixture::params.at("user_preset").value = 1;
        userPresetChanged(effect, state, "user_preset", 12, root.string());
        fixture::check(fixture::params.at("user_preset_name").text == "Моя оптика"
            && fixture::params.at("user_preset_file").text == file.string() && fixture::numericWrites == 0,
            "Selecting a preset must assign its name/path without applying it");
        fixture::params.at("camera_softness").value = 0.7;
        userPresetChanged(effect, state, "apply_user_preset", 12, root.string());
        fixture::check(fixture::params.at("camera_softness").value == 0.15 && fixture::edits == 1,
            "Apply must restore the snapshot in one edit");
        userPresetChanged(effect, state, "refresh_user_presets", 12, root.string());
        fixture::check(fixture::params.at("user_preset_catalog").text == "Моя оптика"
            && fixture::optionWrites == 2 && fixture::resetCalls == 0, "Refresh must publish selectable names without property reset");
        fixture::params.at("camera_softness").keys = 1;
        const auto writes = fixture::numericWrites;
        userPresetChanged(effect, state, "apply_user_preset", 12, root.string());
        fixture::check(fixture::numericWrites == writes && fixture::params.at("user_preset_status").text.find("Ошибка:") == 0,
            "Animated targets must be rejected before any numeric write");
        auto second = saved; second.name = "Вторая оптика";
        silentfilm::saveUserPreset(root.string(), second);
        userPresetChanged(effect, state, "refresh_user_presets", 12, root.string());
        fixture::check(fixture::labels.size() == 3, "New library file did not become selectable");
        std::filesystem::remove(root / saved.effect / (second.name + ".sfpreset"));
        userPresetChanged(effect, state, "refresh_user_presets", 12, root.string());
        fixture::check(fixture::labels.size() == 3 && fixture::labels[2] == "—" && state.userPresetNames.size() == 1,
            "Removed files must become neutral slots without changing host indices");
        fixture::params.at("user_preset").value = 2;
        userPresetChanged(effect, state, "user_preset", 12, root.string());
        fixture::check(fixture::params.at("user_preset_name").text == "Моя оптика", "A stale slot selected a different file");
        fixture::check(!state.editingUserPreset && fixture::resetCalls == 0, "Preset callback guard or choice lifetime failed");
        fixture::check(destroy(effect) == kOfxStatOK && !fixture::instanceData, "DestroyInstance did not clear state");
        std::cout << "OFX Camera Optics preset save/apply lifecycle: PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
