#include "silentfilm/user_presets.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace silentfilm {
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
[[noreturn]] void fileError(const char* message) { throw std::runtime_error(std::string(message) + ": " + std::strerror(errno)); }

std::string join(const std::string& parent, const std::string& child)
{
    require(!parent.empty() && parent.find('\0') == std::string::npos, "Некорректная папка пресетов");
    return parent + (parent.back() == '/' ? "" : "/") + child;
}

void ensureDirectories(const std::string& path)
{
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') continue;
        const auto part = path.substr(0, i);
        if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) fileError("Не удалось создать папку пресетов");
        struct stat info{};
        if (::stat(part.c_str(), &info) != 0) fileError("Не удалось проверить папку пресетов");
        require(S_ISDIR(info.st_mode), "Вместо папки пресетов найден файл");
    }
}

void validateValues(const UserPreset& preset)
{
    require(!preset.values.empty() && preset.values.size() <= 128, "Недопустимое число параметров пресета");
    std::set<std::string> names;
    for (const auto& value : preset.values) {
        validateUserPresetName(value.id);
        require(names.insert(value.id).second && std::isfinite(value.value), "Повторяющийся или неконечный параметр пресета");
        require(!value.integer || std::floor(value.value) == value.value, "Дробное значение целого параметра пресета");
    }
}

struct File {
    int descriptor = -1;
    explicit File(int value) : descriptor(value) {}
    ~File() { if (descriptor >= 0) ::close(descriptor); }
};

struct DirectoryCloser {
    void operator()(DIR* directory) const noexcept { if (directory) ::closedir(directory); }
};
} // namespace

std::string userPresetRoot()
{
    if (const char* path = std::getenv("XDG_DATA_HOME"); path && *path == '/')
        return join(path, "ALEXZSilentFilm/ofx-presets");
    const char* home = std::getenv("HOME");
    require(home && *home == '/', "Не удалось определить пользовательскую папку пресетов");
    return join(home, ".local/share/ALEXZSilentFilm/ofx-presets");
}

void validateUserPresetName(const std::string& name)
{
    require(!name.empty() && name.size() <= 120 && name != "." && name != ".."
        && name.front() != ' ' && name.back() != ' ' && name.back() != '.', "Некорректное имя пресета или параметра");
    for (const unsigned char c : name)
        require(c >= 32 && c != 127 && std::string("/\\<>:\"|?*").find(static_cast<char>(c)) == std::string::npos,
            "В имени пресета нельзя использовать разделители пути и служебные символы");
}

std::string userPresetPath(const std::string& root, const std::string& effect, const std::string& name)
{
    validateUserPresetName(effect); validateUserPresetName(name);
    return join(join(root, effect), name + ".sfpreset");
}

std::string userPresetFileName(const std::string& path)
{
    require(path.find('\0') == std::string::npos, "Некорректный путь пресета");
    const auto separator = path.find_last_of('/');
    const auto file = path.substr(separator == std::string::npos ? 0 : separator + 1);
    constexpr std::size_t suffixSize = 9;
    require(file.size() > suffixSize && file.compare(file.size() - suffixSize, suffixSize, ".sfpreset") == 0,
        "Выбери файл с расширением .sfpreset");
    const auto name = file.substr(0, file.size() - suffixSize);
    validateUserPresetName(name);
    return name;
}

std::vector<std::string> listUserPresets(const std::string& root, const std::string& effect)
{
    validateUserPresetName(effect);
    const auto folder = join(root, effect);
    DIR* raw = ::opendir(folder.c_str());
    if (!raw) { if (errno == ENOENT) return {}; fileError("Не удалось прочитать библиотеку пресетов"); }
    const std::unique_ptr<DIR, DirectoryCloser> directory(raw);
    std::vector<std::string> names;
    while (true) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) { if (errno != 0) fileError("Не удалось прочитать каталог пресетов"); break; }
        const auto file = join(folder, entry->d_name);
        struct stat info{};
        if (::lstat(file.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        try { names.push_back(userPresetFileName(file)); } catch (const std::runtime_error&) {}
    }
    std::sort(names.begin(), names.end());
    return names;
}

void saveUserPreset(const std::string& root, const UserPreset& preset)
{
    const auto destination = userPresetPath(root, preset.effect, preset.name);
    validateValues(preset);
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "SILENTFILM_OFX_PRESET 1\n" << std::quoted(preset.effect) << '\n' << std::quoted(preset.name)
        << '\n' << preset.values.size() << '\n' << std::setprecision(17);
    for (const auto& value : preset.values)
        output << std::quoted(value.id) << ' ' << (value.integer ? 'i' : 'd') << ' ' << value.value << '\n';
    const auto data = output.str();
    require(static_cast<bool>(output) && data.size() <= 65536, "Не удалось сформировать пользовательский пресет");
    ensureDirectories(join(root, preset.effect));
    static std::atomic<unsigned long long> sequence{0};
    const auto temporary = destination + ".tmp-" + std::to_string(::getpid()) + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence.fetch_add(1));
    File file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
    if (file.descriptor < 0) fileError("Не удалось создать файл пресета");
    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto written = ::write(file.descriptor, data.data() + offset, data.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) fileError("Не удалось записать пресет");
            require(written > 0, "Запись пресета прервана");
            offset += static_cast<std::size_t>(written);
        }
        const int descriptor = file.descriptor; file.descriptor = -1;
        if (::close(descriptor) != 0) fileError("Не удалось завершить запись пресета");
        if (::rename(temporary.c_str(), destination.c_str()) != 0) fileError("Не удалось сохранить пресет");
    } catch (...) { ::unlink(temporary.c_str()); throw; }
}

UserPreset loadUserPreset(const std::string& root, const std::string& effect, const std::string& name)
{
    return loadUserPresetFile(userPresetPath(root, effect, name), effect);
}

UserPreset loadUserPresetFile(const std::string& path, const std::string& effect)
{
    const auto name = userPresetFileName(path);
    validateUserPresetName(effect);
    File file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
    if (file.descriptor < 0) fileError("Не удалось открыть пресет");
    struct stat info{};
    if (::fstat(file.descriptor, &info) != 0) fileError("Не удалось проверить пресет");
    require(S_ISREG(info.st_mode) && info.st_size >= 0 && info.st_size <= 65536,
        "Пресет имеет недопустимый размер или тип файла");
    std::string data;
    char buffer[4096];
    while (true) {
        const auto count = ::read(file.descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fileError("Не удалось прочитать пресет");
        if (count == 0) break;
        data.append(buffer, static_cast<std::size_t>(count));
        require(data.size() <= 65536, "Пресет превышает допустимый размер");
    }
    std::istringstream input(data);
    input.imbue(std::locale::classic());
    std::string magic;
    int version = 0;
    UserPreset preset;
    std::size_t count = 0;
    require(static_cast<bool>(input >> magic >> version) && magic == "SILENTFILM_OFX_PRESET" && version == 1,
        "Неизвестная версия пользовательского пресета");
    require(static_cast<bool>(input >> std::quoted(preset.effect) >> std::quoted(preset.name) >> count)
        && preset.effect == effect && preset.name == name && count > 0 && count <= 128,
        "Пресет принадлежит другой ноде, переименован или повреждён");
    for (std::size_t i = 0; i < count; ++i) {
        UserPresetValue value;
        char type = 0;
        require(static_cast<bool>(input >> std::quoted(value.id) >> type >> value.value) && (type == 'i' || type == 'd'),
            "Повреждён параметр пользовательского пресета");
        value.integer = type == 'i'; preset.values.push_back(value);
    }
    input >> std::ws;
    require(input.eof(), "Лишние данные в пользовательском пресете");
    validateValues(preset);
    return preset;
}
} // namespace silentfilm
