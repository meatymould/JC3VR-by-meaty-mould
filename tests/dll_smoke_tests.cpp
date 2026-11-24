#include <Windows.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto dllPath = std::filesystem::path(executable).parent_path() / L"jc3vr.dll";
    const HMODULE module = LoadLibraryW(dllPath.c_str());
    assert(module != nullptr);
    using InitializeFn = void(WINAPI*)();
    const auto initialize = reinterpret_cast<InitializeFn>(GetProcAddress(module, "JC3VR_Initialize"));
    assert(initialize != nullptr);
    initialize();
    wchar_t localAppData[MAX_PATH]{};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    assert(size != 0 && size < MAX_PATH);
    const auto logPath = std::filesystem::path(localAppData) / L"JC3VR" / L"jc3vr.log";
    assert(std::filesystem::exists(logPath));
    std::ifstream log(logPath, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(log), {}};
    assert(contents.find("build fingerprint:") != std::string::npos);
    // This test host is not JC3: JC3VR must fail closed and install nothing.
    assert(contents.find("JC3VR disabled: unsupported JC3 build") != std::string::npos);
    assert(contents.find("OpenXR loader:") == std::string::npos);
    std::cout << "JC3VR DLL smoke test passed: " << logPath.string() << "\n";
}
