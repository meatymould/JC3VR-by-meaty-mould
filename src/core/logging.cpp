#include "core/logging.h"
#include <Windows.h>
#include <mutex>
#include <iterator>
#include <string>

namespace jc3vr {
namespace {
std::mutex g_mutex;
std::wstring g_logPath;

std::wstring LogDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (size && size < MAX_PATH) return std::wstring(buffer) + L"\\JC3VR";
    wchar_t currentDirectory[MAX_PATH]{};
    const DWORD currentLength = GetCurrentDirectoryW(MAX_PATH, currentDirectory);
    return currentLength && currentLength < MAX_PATH ? std::wstring(currentDirectory) + L"\\JC3VR" : L"JC3VR";
}
}

void InitializeLogging() {
    std::lock_guard lock(g_mutex);
    if (!g_logPath.empty()) return;
    const std::wstring directory = LogDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);
    g_logPath = directory + L"\\jc3vr.log";
}

void Log(std::string_view message) {
    InitializeLogging();
    std::lock_guard lock(g_mutex);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char prefix[64]{};
    const int count = snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    // Steam's process environment successfully writes the bootstrap trace via
    // Win32 but did not append through std::ofstream.  Use the same direct
    // append primitive for all runtime logs; it is deterministic and avoids
    // stream/CRT state differences between the proxy and game process.
    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, prefix, static_cast<DWORD>(count), &written, nullptr);
    WriteFile(file, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
    constexpr char newline = '\n';
    WriteFile(file, &newline, 1, &written, nullptr);
    CloseHandle(file);
}

void TraceRuntime(std::string_view message) {
    char base[MAX_PATH]{};
    const DWORD baseLength = GetEnvironmentVariableA("LOCALAPPDATA", base, static_cast<DWORD>(std::size(base)));
    if (!baseLength || baseLength >= std::size(base)) return;
    const std::string directory = std::string(base) + "\\JC3VR";
    CreateDirectoryA(directory.c_str(), nullptr);
    const std::string path = directory + "\\bootstrap_worker.log";
    HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char prefix[80]{};
    const int prefixLength = snprintf(prefix, sizeof(prefix), "[%02u:%02u:%02u.%03u] JC3VR runtime: ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    DWORD written = 0;
    WriteFile(file, prefix, static_cast<DWORD>(prefixLength), &written, nullptr);
    WriteFile(file, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
    constexpr char newline = '\n';
    WriteFile(file, &newline, 1, &written, nullptr);
    CloseHandle(file);
}
}
