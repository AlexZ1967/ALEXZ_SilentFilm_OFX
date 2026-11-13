#include "silentfilm/user_presets.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F action)
{
    bool caught = false;
    try { action(); } catch (const std::exception&) { caught = true; }
    require(caught, "Invalid preset was accepted");
}
}

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "Pass the test output directory");
        const auto root = std::filesystem::path(argv[1])
            / ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{root};
        using namespace silentfilm;
        const std::string effect = "com.alexz.silentfilm.defects";
        require(listUserPresets(root.string(), effect).empty(), "A missing library should be empty");
        require(!std::filesystem::exists(root), "Listing presets must not create storage");
        UserPreset preset{effect, "Мягкая пыль", {{"strength", false, 0.12345678901234567},
            {"seed", true, 2147483647}, {"pattern", true, 0}, {"damage_type", true, 3}, {"blend", false, 1}}};
        saveUserPreset(root.string(), preset);
        const auto loaded = loadUserPreset(root.string(), effect, preset.name);
        const auto selected = (root / effect / (preset.name + ".sfpreset")).string();
        require(userPresetFileName(selected) == preset.name && loadUserPresetFile(selected, effect).name == preset.name,
            "File picker path lost the Unicode preset name");
        rejected([&] { loadUserPresetFile(selected, "com.alexz.silentfilm.print"); });
        require(loaded.values.size() == preset.values.size(), "Snapshot lost parameters");
        for (std::size_t i = 0; i < preset.values.size(); ++i)
            require(loaded.values[i].id == preset.values[i].id && loaded.values[i].integer == preset.values[i].integer
                && loaded.values[i].value == preset.values[i].value, "Typed snapshot round trip lost precision");
        auto other = preset; other.effect = "com.alexz.silentfilm.print";
        saveUserPreset(root.string(), other);
        require(listUserPresets(root.string(), effect).size() == 1 && listUserPresets(root.string(), other.effect).size() == 1,
            "Different effect libraries must be independent");
        preset.values[0].value = 0.75;
        saveUserPreset(root.string(), preset);
        require(loadUserPreset(root.string(), effect, preset.name).values[0].value == 0.75, "Explicit overwrite did not update the preset");
        require(listUserPresets(root.string(), effect).size() == 1, "Overwrite duplicated the preset");
        for (const auto& entry : std::filesystem::directory_iterator(root / effect))
            require(entry.path().extension() == ".sfpreset", "Atomic writer left a temporary file");
        for (const auto& name : {"", "../outside", ".", "..", "A/B", "A\\B", "bad\nname", " name", "name."})
            rejected([&] { validateUserPresetName(name); });
        auto broken = preset;
        broken.values[0].value = std::numeric_limits<double>::quiet_NaN();
        rejected([&] { saveUserPreset(root.string(), broken); });
        broken = preset; broken.values.push_back(broken.values[0]);
        rejected([&] { saveUserPreset(root.string(), broken); });
        broken = preset; broken.values[1].value = 1.5;
        rejected([&] { saveUserPreset(root.string(), broken); });
        const auto path = root / effect / (preset.name + ".sfpreset");
        const auto corrupt = [&](const std::string& text) { std::ofstream output(path); output << text; };
        for (const auto& text : {
            "SILENTFILM_OFX_PRESET 2\n",
            "SILENTFILM_OFX_PRESET 1\n\"wrong-effect\"\n\"Мягкая пыль\"\n1\n\"strength\" d 0.2\n",
            "SILENTFILM_OFX_PRESET 1\n\"com.alexz.silentfilm.defects\"\n\"Мягкая пыль\"\n129\n",
            "SILENTFILM_OFX_PRESET 1\n\"com.alexz.silentfilm.defects\"\n\"Мягкая пыль\"\n1\n\"strength\" x 0.2\n",
            "SILENTFILM_OFX_PRESET 1\n\"com.alexz.silentfilm.defects\"\n\"Мягкая пыль\"\n1\n\"strength\" d 0.2\ntrailing-data",
            "SILENTFILM_OFX_PRESET 1\n\"com.alexz.silentfilm.defects\"\n\"Мягкая пыль\"\n2\n\"strength\" d 0.2\n"}) {
            corrupt(text);
            rejected([&] { loadUserPreset(root.string(), effect, preset.name); });
        }
        std::filesystem::remove(path);
        std::filesystem::create_symlink(root / other.effect / (preset.name + ".sfpreset"), path);
        rejected([&] { loadUserPreset(root.string(), effect, preset.name); });
        require(listUserPresets(root.string(), effect).empty(), "Library listing followed a preset symlink");
        std::cout << "User preset storage: PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
