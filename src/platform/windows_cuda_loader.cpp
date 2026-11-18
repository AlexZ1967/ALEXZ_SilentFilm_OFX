#include <windows.h>
#include <delayimp.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
int moduleAnchor;
FARPROC WINAPI loadBundledCuda(unsigned event, PDelayLoadInfo info)
{
    if (event != dliNotePreLoadLibrary || std::strcmp(info->szDll, SILENTFILM_CUFFT_DLL) != 0) return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleAnchor), &module))
        throw std::runtime_error("SilentFilm: cannot locate CUDA module");
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("SilentFilm: invalid CUDA module path");
    path.resize(size);
    const auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) throw std::runtime_error("SilentFilm: missing CUDA module directory");
    path.resize(separator + 1);
    const std::string name = SILENTFILM_CUFFT_DLL;
    path.append(name.begin(), name.end());
    // Поиск транзитивных зависимостей разрешён в этой папке и стандартных каталогах.
    // Нет SetDllDirectory, изменений общего PATH или загрузки из текущей папки.
    const auto library = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) throw std::runtime_error("SilentFilm: cannot load bundled cuFFT (Win32 "
        + std::to_string(GetLastError()) + ")");
    return reinterpret_cast<FARPROC>(library);
}
} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = loadBundledCuda;
