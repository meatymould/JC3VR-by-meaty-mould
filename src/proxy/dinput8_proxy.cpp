#include <Windows.h>
#include <dinput.h>
#include <cstdio>
#include <iterator>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

INIT_ONCE g_systemOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_modOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_systemDinput = nullptr;
DirectInput8CreateFn g_originalCreate = nullptr;
HMODULE g_jc3vr = nullptr;

void TraceProxy(const char* event, DWORD error = ERROR_SUCCESS) {
    char message[512]{};
    const int count = snprintf(message, sizeof(message), "JC3VR proxy: %s (last_error=%lu)\r\n", event, static_cast<unsigned long>(error));
    OutputDebugStringA(message);
    // This is never called from DllMain.  It is intentionally independent of
    // jc3vr.dll so a failed module load still leaves a durable trace.
    char base[MAX_PATH]{};
    const DWORD baseLength = GetEnvironmentVariableA("LOCALAPPDATA", base, static_cast<DWORD>(std::size(base)));
    if (!baseLength || baseLength >= std::size(base)) return;
    const std::string directory = std::string(base) + "\\JC3VR";
    CreateDirectoryA(directory.c_str(), nullptr);
    const std::string path = directory + "\\proxy_bootstrap.log";
    HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(count), &written, nullptr);
    CloseHandle(file);
}

std::wstring ModuleDirectory(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    std::wstring result(path);
    return result.substr(0, result.find_last_of(L"\\/"));
}

BOOL CALLBACK InitializeSystemProxy(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t systemPath[MAX_PATH]{};
    if (GetSystemDirectoryW(systemPath, static_cast<UINT>(std::size(systemPath))) == 0) return FALSE;
    const std::wstring realDll = std::wstring(systemPath) + L"\\dinput8.dll";
    g_systemDinput = LoadLibraryW(realDll.c_str());
    if (!g_systemDinput) return FALSE;
    g_originalCreate = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(g_systemDinput, "DirectInput8Create"));
    return g_originalCreate != nullptr;
}

BOOL CALLBACK InitializeMod(PINIT_ONCE, PVOID, PVOID*) {
    // This is intentionally run from the bootstrap thread or a DirectInput
    // fallback call, never from DllMain under loader lock.
    const std::wstring directory = ModuleDirectory(reinterpret_cast<HMODULE>(&__ImageBase));
    const std::wstring mod = directory + L"\\mods\\jc3vr.dll";
    SetLastError(ERROR_SUCCESS);
    g_jc3vr = LoadLibraryW(mod.c_str());
    if (g_jc3vr) TraceProxy("LoadLibrary(mods\\jc3vr.dll) succeeded");
    else {
        const DWORD firstError = GetLastError();
        TraceProxy("LoadLibrary(mods\\jc3vr.dll) failed; trying root fallback", firstError);
        SetLastError(ERROR_SUCCESS);
        g_jc3vr = LoadLibraryW((directory + L"\\jc3vr.dll").c_str());
        TraceProxy(g_jc3vr ? "LoadLibrary(root jc3vr.dll) succeeded" : "LoadLibrary(root jc3vr.dll) failed", g_jc3vr ? ERROR_SUCCESS : GetLastError());
    }
    return TRUE;
}

void RequestJc3vrStart(const char* caller) {
    TraceProxy(caller);
    InitOnceExecuteOnce(&g_modOnce, InitializeMod, nullptr, nullptr);
    if (!g_jc3vr) { TraceProxy("JC3VR start skipped: no module"); return; }
    using StartFn = BOOL(WINAPI*)();
    SetLastError(ERROR_SUCCESS);
    const auto start = reinterpret_cast<StartFn>(GetProcAddress(g_jc3vr, "JC3VR_Start"));
    if (!start) { TraceProxy("GetProcAddress(JC3VR_Start) failed", GetLastError()); return; }
    TraceProxy("calling JC3VR_Start outside loader lock");
    SetLastError(ERROR_SUCCESS);
    const BOOL started = start();
    TraceProxy(started ? "JC3VR_Start returned success" : "JC3VR_Start returned failure", started ? ERROR_SUCCESS : GetLastError());
}

DWORD WINAPI BootstrapModThread(void*) {
    RequestJc3vrStart("fallback bootstrap-thread entry");
    return 0;
}
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output, LPUNKNOWN outer) {
    InitOnceExecuteOnce(&g_systemOnce, InitializeSystemProxy, nullptr, nullptr);
    RequestJc3vrStart("first DirectInput8Create");
    return g_originalCreate ? g_originalCreate(instance, version, iid, output, outer) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // DllMain only schedules the work. The new thread cannot execute the
        // loader operation until the loader lock has been released.
        OutputDebugStringA("JC3VR proxy: DLL_PROCESS_ATTACH; scheduling fallback bootstrap thread\n");
        SetLastError(ERROR_SUCCESS);
        if (HANDLE thread = CreateThread(nullptr, 0, BootstrapModThread, nullptr, 0, nullptr)) {
            OutputDebugStringA("JC3VR proxy: fallback bootstrap thread created\n");
            CloseHandle(thread);
        } else {
            char message[160]{};
            snprintf(message, sizeof(message), "JC3VR proxy: fallback bootstrap thread creation failed (last_error=%lu)\n", static_cast<unsigned long>(GetLastError()));
            OutputDebugStringA(message);
        }
    }
    return TRUE;
}
