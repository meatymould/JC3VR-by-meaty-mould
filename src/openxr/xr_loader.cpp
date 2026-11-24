#include "openxr/xr_loader.h"
#include <filesystem>
#include <vector>

namespace jc3vr {
namespace {
std::wstring ReadActiveRuntime() {
    wchar_t value[32768]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) return value;
    if (RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) return value;
    return {};
}
}

HMODULE LoadActiveOpenXrLoader(std::string& detail) {
    if (HMODULE alreadyLoaded = GetModuleHandleW(L"openxr_loader.dll")) { detail = "existing process loader"; return alreadyLoaded; }
    if (HMODULE defaultLoader = LoadLibraryW(L"openxr_loader.dll")) { detail = "default DLL search path"; return defaultLoader; }

    const std::wstring runtime = ReadActiveRuntime();
    if (runtime.empty()) { detail = "no active OpenXR runtime registration"; return nullptr; }
    const auto runtimeManifest = std::filesystem::path(runtime);
    const auto root = runtimeManifest.parent_path();
    const std::vector<std::filesystem::path> candidates{
        root / L"openxr_loader.dll",
        root / L"bin" / L"openxr_loader.dll",
        root / L"bin" / L"win64" / L"openxr_loader.dll",
    };
    for (const auto& candidate : candidates) {
        if (!std::filesystem::exists(candidate)) continue;
        if (HMODULE loader = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
            detail = "active runtime directory: " + candidate.string();
            return loader;
        }
    }
    detail = "active runtime is registered but no compatible openxr_loader.dll was found beside it";
    return nullptr;
}
}
