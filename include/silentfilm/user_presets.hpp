#pragma once

#include <string>
#include <vector>

namespace silentfilm {

struct UserPresetValue {
    std::string id;
    bool integer = false;
    double value = 0;
};

struct UserPreset {
    std::string effect;
    std::string name;
    std::vector<UserPresetValue> values;
};

/** Resolve the per-user library without creating directories or modifying the environment. */
std::string userPresetRoot();
/** Validate a portable filename component while retaining Unicode labels. */
void validateUserPresetName(const std::string& name);
/** List only regular preset files belonging to the requested effect. */
std::vector<std::string> listUserPresets(const std::string& root, const std::string& effect);
/** Atomically save a bounded typed snapshot; explicitly named presets may be overwritten. */
void saveUserPreset(const std::string& root, const UserPreset& preset);
/** Read a strict versioned file and reject mismatching effect identities. */
UserPreset loadUserPreset(const std::string& root, const std::string& effect, const std::string& name);
/** Read a selected preset file without importing it into the library or changing its effect identity. */
UserPreset loadUserPresetFile(const std::string& path, const std::string& effect);
/** Join path text without invoking std::filesystem in the host process. */
std::string userPresetPath(const std::string& root, const std::string& effect, const std::string& name);
/** Extract a validated preset label from an explicitly selected filename. */
std::string userPresetFileName(const std::string& path);

} // namespace silentfilm
