#include <Windows.h>
#include <initguid.h>
#include <dinput.h>
#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto proxyPath = std::filesystem::path(executable).parent_path() / L"dinput8.dll";
    const HMODULE proxy = LoadLibraryW(proxyPath.c_str());
    assert(proxy != nullptr);
    using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(proxy, "DirectInput8Create"));
    assert(create != nullptr);
    IDirectInput8W* directInput = nullptr;
    const HRESULT result = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&directInput), nullptr);
    assert(SUCCEEDED(result) && directInput != nullptr);
    directInput->Release();
    std::cout << "dinput8 proxy smoke test passed\n";
}
