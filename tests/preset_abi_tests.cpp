#include "silentfilm/user_presets.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

// Имитируем подмену C++ filesystem-символа библиотекой хоста. Здесь намеренно
// нет std::filesystem даже для тестовых путей: любой такой вызов — регрессия.
extern "C" void presetFilesystemTrap(void*) asm("_ZNSt10filesystem7__cxx114path14_M_split_cmptsEv");
extern "C" void presetFilesystemTrap(void*)
{
    std::fputs("Preset storage called an interposed std::filesystem path symbol\n", stderr);
    std::abort();
}

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::runtime_error("Pass the test output directory");
        const std::string root = std::string(argv[1]) + "/run-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string effect = "com.alexz.silentfilm.cameraoptics";
        struct Cleanup {
            std::string root, effect;
            ~Cleanup() {
                for (const char* name : {"My optics", "Моя оптика"})
                    ::unlink(silentfilm::userPresetPath(root, effect, name).c_str());
                ::rmdir((root + "/" + effect).c_str()); ::rmdir(root.c_str());
            }
        } cleanup{root, effect};
        const auto configured = silentfilm::userPresetRoot();
        if (configured.empty() || configured.front() != '/') throw std::runtime_error("Invalid user preset root");
        for (const char* name : {"My optics", "Моя оптика"}) {
            silentfilm::UserPreset preset{effect, name, {{"camera_softness", false, 0.15}, {"focus_seed", true, 1925}}};
            silentfilm::saveUserPreset(root, preset);
            const auto path = silentfilm::userPresetPath(root, effect, name);
            const auto loaded = silentfilm::loadUserPresetFile(path, effect);
            if (loaded.name != name || loaded.values.size() != 2 || loaded.values[0].value != 0.15)
                throw std::runtime_error("ASCII/Unicode preset round trip failed with a hostile filesystem symbol");
        }
        if (silentfilm::listUserPresets(root, effect).size() != 2) throw std::runtime_error("Library listing failed");
        std::cout << "Preset storage under filesystem symbol interposition: PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
