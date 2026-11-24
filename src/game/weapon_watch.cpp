#include "game/weapon_watch.h"
#include "core/logging.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace jc3vr {
namespace {
constexpr std::uintptr_t kT1Translation = 0x198 + 0x30;
constexpr std::uintptr_t kT0Translation = 0x158 + 0x30;
constexpr ULONGLONG kWindowMs = 1500;

// One distinct accessing instruction: where it is, how it was reached, what
// it did (bit 0 write to T1, bit 1 read/write of T1, bit 2 read/write of T0).
struct Site {
    std::atomic<std::uintptr_t> rip{0};
    std::uintptr_t callers[3]{};
    std::atomic<std::uint32_t> kinds{0};
    std::atomic<std::uint32_t> count{0};
    std::atomic<DWORD> thread{0};
};
constexpr int kSites = 48;
Site g_sites[kSites];
std::atomic<std::uint32_t> g_overflow{0};
std::atomic<bool> g_watching{false};
PVOID g_handler = nullptr;
ULONGLONG g_until = 0;

// Up to three return addresses above the faulting frame (RtlVirtualUnwind
// over a copy of the trapped context).
void Unwind(const CONTEXT& trapped, std::uintptr_t out[3]) {
    CONTEXT context = trapped;
    for (int i = 0; i != 3; ++i) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function) {
            // Leaf function: the return address is at [rsp].
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        } else {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        }
        out[i] = context.Rip;
        if (!context.Rip) break;
    }
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* context = info->ContextRecord;
    const DWORD64 status = context->Dr6;
    if (!(status & 0x7)) return EXCEPTION_CONTINUE_SEARCH;  // not one of ours
    context->Dr6 = 0;
    // Data breakpoints trap after the access: Rip is the next instruction.
    const std::uintptr_t rip = context->Rip;
    std::uint32_t kind = 0;
    if (status & 1) kind |= 1;
    if ((status & 2) && !(status & 1)) kind |= 2;
    if (status & 4) kind |= 4;
    for (Site& site : g_sites) {
        std::uintptr_t expected = 0;
        if (site.rip.load() == rip || site.rip.compare_exchange_strong(expected, rip)) {
            if (site.count.fetch_add(1) == 0) {
                __try {
                    Unwind(*context, site.callers);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                site.thread = GetCurrentThreadId();
            }
            site.kinds.fetch_or(kind);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    ++g_overflow;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (or clears, when data == 0) the three watches on every other thread.
int ApplyToThreads(std::uintptr_t data) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{sizeof(entry)};
    int applied = 0;
    const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                if (data) {
                    // DR0: write to T1 translation; DR1: read/write of the same
                    // 8 bytes; DR2: read/write of T0 translation. Length 8 (10b).
                    context.Dr0 = data + kT1Translation;
                    context.Dr1 = data + kT1Translation;
                    context.Dr2 = data + kT0Translation;
                    context.Dr7 = (1ull << 0) | (1ull << 2) | (1ull << 4) |
                                  (0b01ull << 16) | (0b10ull << 18) |  // DR0 write, 8 bytes
                                  (0b11ull << 20) | (0b10ull << 22) |  // DR1 read/write
                                  (0b11ull << 24) | (0b10ull << 26);   // DR2 read/write
                } else {
                    context.Dr0 = context.Dr1 = context.Dr2 = 0;
                    context.Dr7 &= ~((0x3Full) | (0xFFFull << 16));
                }
                context.Dr6 = 0;
                if (SetThreadContext(thread, &context)) ++applied;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return applied;
}

std::string Describe(std::uintptr_t address) {
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    HMODULE module = nullptr;
    char text[96];
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(address), &module) && module) {
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        if (base == exe) std::snprintf(text, sizeof(text), "exe+0x%llX", static_cast<unsigned long long>(address - base));
        else {
            char name[MAX_PATH]{};
            GetModuleFileNameA(module, name, MAX_PATH);
            const char* leaf = std::strrchr(name, '\\');
            std::snprintf(text, sizeof(text), "%s+0x%llX", leaf ? leaf + 1 : name, static_cast<unsigned long long>(address - base));
        }
    } else {
        std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(address));
    }
    return text;
}

void Report() {
    int listed = 0;
    for (Site& site : g_sites) {
        if (!site.rip.load()) continue;
        const std::uint32_t kinds = site.kinds.load();
        std::string what = (kinds & 1) ? "WRITE T1" : (kinds & 2) ? "read T1" : "";
        if (kinds & 4) what += what.empty() ? "T0" : " +T0";
        TraceRuntime("weapon watch: " + Describe(site.rip.load()) + " (next insn) " + what + " x" + std::to_string(site.count.load()) +
            " thread=" + std::to_string(site.thread.load()) + " <- " + Describe(site.callers[0]) + " <- " + Describe(site.callers[1]) + " <- " +
            Describe(site.callers[2]));
        ++listed;
    }
    TraceRuntime("weapon watch: " + std::to_string(listed) + " sites, overflow=" + std::to_string(g_overflow.load()));
}
}

void WeaponWatchTick(std::uintptr_t weaponData) {
    static const std::wstring trigger = [] {
        wchar_t base[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        return length && length < MAX_PATH ? std::wstring(base) + L"\\JC3VR\\jc3vr_weapon_watch" : std::wstring();
    }();
    if (g_watching.load()) {
        if (GetTickCount64() < g_until) return;
        const int cleared = ApplyToThreads(0);
        g_watching = false;
        TraceRuntime("weapon watch: window closed, debug registers cleared on " + std::to_string(cleared) + " threads");
        Report();
        return;
    }
    if (trigger.empty() || GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(trigger.c_str());
    if (!weaponData) {
        TraceRuntime("weapon watch: no held gun found (fire first); not armed");
        return;
    }
    for (Site& site : g_sites) {
        site.rip = 0;
        site.kinds = 0;
        site.count = 0;
        site.thread = 0;
        for (auto& caller : site.callers) caller = 0;
    }
    g_overflow = 0;
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, WatchHandler);
    if (!g_handler) return;
    g_until = GetTickCount64() + kWindowMs;
    g_watching = true;
    const int applied = ApplyToThreads(weaponData);
    char text[160];
    std::snprintf(text, sizeof(text), "weapon watch: armed on CWeaponData 0x%llX (T1/T0 translation) for %llu ms on %d threads",
        static_cast<unsigned long long>(weaponData), static_cast<unsigned long long>(kWindowMs), applied);
    TraceRuntime(text);
}
}
