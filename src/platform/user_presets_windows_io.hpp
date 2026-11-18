#pragma once

// Внутренняя Windows-ветка хранилища. std::filesystem в OFX здесь не используется.
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <stdexcept>

namespace silentfilm::preset_windows {
[[noreturn]] inline void error(const char* message)
{
    throw std::runtime_error(std::string(message) + " (Win32 " + std::to_string(GetLastError()) + ")");
}
inline std::wstring wide(const std::string& value)
{
    if (value.empty() || value.find('\0') != std::string::npos) throw std::runtime_error("Некорректный путь UTF-8");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) error("Некорректный UTF-8");
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size))
        error("Не удалось преобразовать UTF-8");
    return result;
}
inline std::string utf8(const std::wstring& value)
{
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) error("Некорректный путь UTF-16");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr))
        error("Не удалось преобразовать UTF-16");
    return result;
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
inline std::string root()
{
    PWSTR path = nullptr;
    const HRESULT status = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path);
    if (FAILED(status)) throw std::runtime_error("Не удалось определить LocalAppData для пресетов");
    const std::wstring result(path);
    CoTaskMemFree(path);
    return utf8(result) + "/ALEXZSilentFilm/ofx-presets";
}
inline void directories(const std::string& path)
{
    const auto input = wide(path);
    const DWORD size = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (!size) error("Не удалось определить папку пресетов");
    std::wstring absolute(size, L'\0');
    const DWORD written = GetFullPathNameW(input.c_str(), size, absolute.data(), nullptr);
    if (!written || written >= size) error("Не удалось определить папку пресетов");
    absolute.resize(written);
    const int status = SHCreateDirectoryExW(nullptr, absolute.c_str(), nullptr);
    if (status != ERROR_SUCCESS && status != ERROR_ALREADY_EXISTS && status != ERROR_FILE_EXISTS) {
        SetLastError(status); error("Не удалось создать папку пресетов");
    }
    const DWORD attributes = GetFileAttributesW(absolute.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw std::runtime_error("Вместо папки пресетов найден файл");
}
inline std::vector<std::string> files(const std::string& folder)
{
    WIN32_FIND_DATAW info{};
    const HANDLE search = FindFirstFileW(wide(folder + "/*").c_str(), &info);
    if (search == INVALID_HANDLE_VALUE) {
        const auto status = GetLastError();
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return {};
        error("Не удалось прочитать библиотеку пресетов");
    }
    struct SearchCloser { HANDLE handle; ~SearchCloser() { FindClose(handle); } } closer{search};
    std::vector<std::string> result;
    do {
        if (!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            result.push_back(utf8(info.cFileName));
    } while (FindNextFileW(search, &info));
    if (GetLastError() != ERROR_NO_MORE_FILES) error("Не удалось прочитать каталог пресетов");
    return result;
}
inline void write(const std::string& destination, const std::string& temporary, const std::string& data)
{
    const auto temp = wide(temporary), final = wide(destination);
    Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) error("Не удалось создать пресет");
    try {
        DWORD written = 0;
        if (!WriteFile(file.value, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) || written != data.size())
            error("Не удалось записать пресет");
        if (!FlushFileBuffers(file.value)) error("Не удалось завершить запись пресета");
        const HANDLE handle = file.value; file.value = INVALID_HANDLE_VALUE;
        if (!CloseHandle(handle)) error("Не удалось закрыть пресет");
        if (!MoveFileExW(temp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            error("Не удалось сохранить пресет");
    } catch (...) {
        if (file.value != INVALID_HANDLE_VALUE) { CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE; }
        DeleteFileW(temp.c_str()); throw;
    }
}
inline std::string read(const std::string& path)
{
    Handle file(CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) error("Не удалось открыть пресет");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) error("Не удалось проверить пресет");
    if (GetFileType(file.value) != FILE_TYPE_DISK || info.nFileSizeHigh || info.nFileSizeLow > 65536
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("Пресет имеет недопустимый размер или тип файла");
    std::string result(info.nFileSizeLow, '\0');
    DWORD count = 0;
    if (!ReadFile(file.value, result.data(), static_cast<DWORD>(result.size()), &count, nullptr) || count != result.size())
        error("Не удалось прочитать пресет");
    return result;
}
} // namespace silentfilm::preset_windows
