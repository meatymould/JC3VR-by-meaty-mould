// Read-only: find where a specific run of floats lives in JustCause3.exe.
//   find_floats <tolerance> <f0> <f1> ... [--context]
// Scans committed writable memory once for consecutive floats all within
// tolerance, prints each hit, and with --context lists rigid 4x4 transforms
// and 4x4 projection-like matrices within +-0x400 bytes of the hit, so the
// owning struct's layout can be read. ReadProcessMemory only.
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
DWORD FindProcess() {
    PROCESSENTRY32W entry{sizeof(entry)};
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;
    if (snapshot != INVALID_HANDLE_VALUE && Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"JustCause3.exe") == 0) { pid = entry.th32ProcessID; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    return pid;
}

bool Rigid(const float* m) {
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    return std::abs(len(m) - 1) < 0.01f && std::abs(len(m + 4) - 1) < 0.01f && std::abs(len(m + 8) - 1) < 0.01f &&
           std::abs(dot(m, m + 4)) < 0.01f && std::abs(dot(m, m + 8)) < 0.01f && std::abs(dot(m + 4, m + 8)) < 0.01f &&
           std::abs(m[3]) < 1e-4f && std::abs(m[7]) < 1e-4f && std::abs(m[11]) < 1e-4f && std::abs(m[15] - 1) < 1e-4f &&
           std::abs(m[12]) + std::abs(m[13]) + std::abs(m[14]) > 10.0f;
}
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: find_floats <tolerance> <f0> <f1> ... [--context]\n"); return 1; }
    const float tolerance = static_cast<float>(std::atof(argv[1]));
    std::vector<float> pattern;
    bool context = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--context") == 0) context = true;
        else pattern.push_back(static_cast<float>(std::atof(argv[i])));
    }
    const DWORD pid = FindProcess();
    const HANDLE process = pid ? OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid) : nullptr;
    if (!process) { std::fprintf(stderr, "JustCause3.exe not readable\n"); return 2; }
    std::vector<std::byte> chunk(16 << 20);
    std::uint64_t scanned = 0;
    int hits = 0;
    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t address = 0x10000; VirtualQueryEx(process, reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info);
         address = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
        const DWORD protection = info.Protect & 0xff;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) ||
            !(protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READWRITE)) continue;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        for (std::uintptr_t offset = 0; offset < info.RegionSize; offset += chunk.size() - 256) {
            const std::size_t size = static_cast<std::size_t>(std::min<std::uintptr_t>(chunk.size(), info.RegionSize - offset));
            SIZE_T got = 0;
            if (!ReadProcessMemory(process, reinterpret_cast<const void*>(base + offset), chunk.data(), size, &got) || got < pattern.size() * 4) continue;
            scanned += got;
            const auto* f = reinterpret_cast<const float*>(chunk.data());
            const std::size_t n = got / 4;
            for (std::size_t i = 0; i + pattern.size() <= n; ++i) {
                if (!(std::abs(f[i] - pattern[0]) <= tolerance)) continue;
                bool all = true;
                for (std::size_t k = 1; k < pattern.size() && all; ++k) all = std::abs(f[i + k] - pattern[k]) <= tolerance;
                if (!all) continue;
                const std::uintptr_t hit = base + offset + i * 4;
                if (++hits > 64) continue;
                std::printf("hit 0x%016llx\n", static_cast<unsigned long long>(hit));
                if (!context) continue;
                std::vector<float> around(0x800 / 4);
                const std::uintptr_t from = hit - 0x400;
                if (!ReadProcessMemory(process, reinterpret_cast<const void*>(from), around.data(), 0x800, &got) || got != 0x800) continue;
                for (std::size_t j = 0; j + 16 <= around.size(); ++j) {
                    const float* m = around.data() + j;
                    const long long rel = static_cast<long long>(j * 4) - 0x400;
                    if (Rigid(m))
                        std::printf("  rigid @%+lld pos=[%.3f,%.3f,%.3f] back=[%.3f,%.3f,%.3f]\n", rel, m[12], m[13], m[14], m[8], m[9], m[10]);
                }
            }
        }
    }
    std::printf("scanned %.1f MB, %d hits\n", scanned / 1048576.0, hits);
    CloseHandle(process);
    return 0;
}
