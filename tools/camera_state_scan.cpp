// Read-only external scanner for JC3's upstream camera orientation state.
//
// Usage (JC3 in-world, JC3VR head rotation off so the camera records hold the
// engine orientation):
//   camera_state_scan init   <candidates.bin>   snapshot every memory copy
//   camera_state_scan filter <candidates.bin>   keep only copies that followed
//   camera_state_scan auto   <report.txt>       snapshot, then drive the camera
//       with exact relative mouse moves (SendInput, only while JC3 is the
//       foreground window) and filter after each settled move.
//
// The reference orientation is read from the validated owner+0x5C8 camera
// record (+0x94 rigid world transform). Candidates are: matrix rows (right,
// up, back, forward), quaternions (both signs, both row/column readings),
// and single yaw/pitch angles in radians and degrees under several axis
// conventions. Only ReadProcessMemory is used; the game is never written.
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr std::uintptr_t kCameraOwnerGlobalRva = 0x02ED0E20;
constexpr float kPi = 3.14159265358979f;

struct Candidate {
    std::uint64_t address;
    std::uint32_t kind;  // index into the reference-value table below
    std::uint32_t width; // number of floats compared
};

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

std::uintptr_t FindImageBase(DWORD pid, std::uintptr_t& imageSize) {
    MODULEENTRY32W entry{sizeof(entry)};
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    std::uintptr_t base = 0;
    if (snapshot != INVALID_HANDLE_VALUE && Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"JustCause3.exe") == 0) {
                base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                imageSize = entry.modBaseSize;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    return base;
}

template <typename T>
bool Read(HANDLE process, std::uintptr_t address, T& value) {
    SIZE_T count = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value, sizeof(value), &count) && count == sizeof(value);
}

// Reference values derived from the current engine camera basis.
struct Reference {
    std::vector<std::vector<float>> values; // per kind
    std::vector<std::string> names;
    std::uintptr_t cameraRecords[2]{};
};

void Add(Reference& reference, const std::string& name, std::vector<float> value) {
    reference.names.push_back(name);
    reference.values.push_back(std::move(value));
}

std::array<float, 4> QuaternionFromRows(const float* m) {
    // m is row-major 3x3 with rows = basis vectors (row-vector convention).
    const float trace = m[0] + m[4] + m[8];
    std::array<float, 4> q{}; // x y z w
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m[5] - m[7]) / s, (m[6] - m[2]) / s, (m[1] - m[3]) / s, 0.25f * s};
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q = {0.25f * s, (m[1] + m[3]) / s, (m[6] + m[2]) / s, (m[5] - m[7]) / s};
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q = {(m[1] + m[3]) / s, 0.25f * s, (m[5] + m[7]) / s, (m[6] - m[2]) / s};
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q = {(m[6] + m[2]) / s, (m[5] + m[7]) / s, 0.25f * s, (m[1] - m[3]) / s};
    }
    return q;
}

bool BuildReference(HANDLE process, std::uintptr_t base, Reference& reference) {
    std::uintptr_t owner = 0;
    if (!Read(process, base + kCameraOwnerGlobalRva, owner) || !owner) return false;
    Read(process, owner + 0x5C0, reference.cameraRecords[0]);
    Read(process, owner + 0x5C8, reference.cameraRecords[1]);
    std::array<float, 16> world{};
    if (!reference.cameraRecords[1] || !Read(process, reference.cameraRecords[1] + 0x94, world)) return false;
    const float rows[9]{world[0], world[1], world[2], world[4], world[5], world[6], world[8], world[9], world[10]};
    for (const float value : rows) if (!std::isfinite(value)) return false;
    const float* right = rows;
    const float* up = rows + 3;
    const float* back = rows + 6;
    Add(reference, "row_right", {right[0], right[1], right[2]});
    Add(reference, "row_up", {up[0], up[1], up[2]});
    Add(reference, "row_back", {back[0], back[1], back[2]});
    Add(reference, "forward", {-back[0], -back[1], -back[2]});
    // Column reading (the transpose) for engines storing column vectors.
    Add(reference, "col0", {rows[0], rows[3], rows[6]});
    Add(reference, "col2", {rows[2], rows[5], rows[8]});
    const auto q = QuaternionFromRows(rows);
    Add(reference, "quat_xyzw", {q[0], q[1], q[2], q[3]});
    Add(reference, "quat_neg_xyzw", {-q[0], -q[1], -q[2], -q[3]});
    Add(reference, "quat_conj_xyzw", {-q[0], -q[1], -q[2], q[3]});
    Add(reference, "quat_negconj_xyzw", {q[0], q[1], q[2], -q[3]});
    Add(reference, "quat_wxyz", {q[3], q[0], q[1], q[2]});
    const float fx = -back[0], fy = -back[1], fz = -back[2];
    const float yaws[4]{std::atan2(fx, fz), std::atan2(-fx, -fz), std::atan2(fx, -fz), std::atan2(-fx, fz)};
    const float pitch = std::asin(std::clamp(fy, -1.0f, 1.0f));
    for (int index = 0; index != 4; ++index) {
        Add(reference, "yaw_rad_" + std::to_string(index), {yaws[index]});
        Add(reference, "yaw_deg_" + std::to_string(index), {yaws[index] * 180.0f / kPi});
    }
    Add(reference, "pitch_rad", {pitch});
    Add(reference, "pitch_rad_neg", {-pitch});
    Add(reference, "pitch_deg", {pitch * 180.0f / kPi});
    Add(reference, "pitch_deg_neg", {-pitch * 180.0f / kPi});
    return true;
}

float Tolerance(const std::string& name) {
    // Loose enough for small first-person camera animation between reads.
    if (name.rfind("yaw_deg", 0) == 0 || name.rfind("pitch_deg", 0) == 0) return 0.06f;
    if (name.rfind("yaw", 0) == 0 || name.rfind("pitch", 0) == 0) return 0.001f;
    return 0.0015f;
}

bool Matches(const float* memory, const std::vector<float>& expected, float tolerance) {
    for (std::size_t index = 0; index != expected.size(); ++index)
        if (!(std::abs(memory[index] - expected[index]) <= tolerance)) return false;
    return true;
}

// Single-angle kinds are only meaningful away from zero, where random data
// would match trivially.
bool Usable(const std::vector<float>& value) {
    if (value.size() != 1) return true;
    return std::abs(value[0]) > 0.05f;
}

void PrintCandidate(const Candidate& candidate, const Reference& reference, std::uintptr_t base, std::uintptr_t imageSize) {
    std::printf("  0x%016llx %-18s", static_cast<unsigned long long>(candidate.address), reference.names[candidate.kind].c_str());
    if (candidate.address >= base && candidate.address < base + imageSize)
        std::printf(" JustCause3.exe+0x%llx", static_cast<unsigned long long>(candidate.address - base));
    for (int record = 0; record != 2; ++record) {
        const auto camera = reference.cameraRecords[record];
        if (camera && candidate.address >= camera && candidate.address < camera + 0x600)
            std::printf(" (in cam%s+0x%llx)", record ? "5c8" : "5c0", static_cast<unsigned long long>(candidate.address - camera));
    }
    std::printf("\n");
}

// Everything below reads game memory only. The one side effect of "auto" is
// ordinary relative mouse input sent while JC3 is the foreground window, the
// same input path the original head-look bridge used.
struct Session {
    HANDLE process = nullptr;
    DWORD pid = 0;
    std::uintptr_t base = 0;
    std::uintptr_t imageSize = 0;
};

bool OpenSession(Session& session) {
    session.pid = FindProcess();
    if (!session.pid) { std::cerr << "JustCause3.exe not found\n"; return false; }
    session.base = FindImageBase(session.pid, session.imageSize);
    session.process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, session.pid);
    if (!session.base || !session.process) { std::cerr << "cannot open JC3 for reading\n"; return false; }
    return true;
}

std::vector<Candidate> ScanAll(const Session& session, Reference& reference) {
    std::vector<Candidate> candidates;
    std::vector<std::byte> chunk(4 << 20);
    std::uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION info{};
    for (std::uintptr_t address = 0x10000; VirtualQueryEx(session.process, reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info);
         address = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
        const DWORD protection = info.Protect & 0xff;
        const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (info.State != MEM_COMMIT || !writable || (info.Protect & PAGE_GUARD)) continue;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        for (std::uintptr_t offset = 0; offset < info.RegionSize; offset += chunk.size()) {
            const std::size_t size = static_cast<std::size_t>(std::min<std::uintptr_t>(chunk.size(), info.RegionSize - offset));
            SIZE_T got = 0;
            if (!ReadProcessMemory(session.process, reinterpret_cast<const void*>(regionBase + offset), chunk.data(), size, &got) || got < 16) continue;
            // Refresh the reference per chunk so small camera animation during
            // a multi-second scan does not hide the copies being looked for.
            Reference fresh;
            if (BuildReference(session.process, session.base, fresh)) reference = fresh;
            scanned += got;
            const auto* floats = reinterpret_cast<const float*>(chunk.data());
            const std::size_t count = got / sizeof(float);
            for (std::size_t index = 0; index + 4 <= count; ++index) {
                const float first = floats[index];
                if (!std::isfinite(first)) continue;
                for (std::uint32_t kind = 0; kind != reference.values.size(); ++kind) {
                    const auto& expected = reference.values[kind];
                    if (!Usable(expected)) continue;
                    const float tolerance = Tolerance(reference.names[kind]);
                    if (std::abs(first - expected[0]) > tolerance) continue;
                    if (!Matches(floats + index, expected, tolerance)) continue;
                    candidates.push_back({regionBase + offset + index * sizeof(float), kind, static_cast<std::uint32_t>(expected.size())});
                }
            }
        }
    }
    std::printf("scanned %.1f MB -> %zu candidates\n", scanned / 1048576.0, candidates.size());
    return candidates;
}

std::vector<Candidate> Filter(const Session& session, const std::vector<Candidate>& previous, const Reference& reference) {
    std::vector<Candidate> kept;
    for (const auto& item : previous) {
        std::array<float, 4> memory{};
        SIZE_T got = 0;
        if (!ReadProcessMemory(session.process, reinterpret_cast<const void*>(item.address), memory.data(), item.width * sizeof(float), &got) ||
            got != item.width * sizeof(float)) continue;
        if (Matches(memory.data(), reference.values[item.kind], Tolerance(reference.names[item.kind]))) kept.push_back(item);
    }
    return kept;
}

void PrintSummary(const std::vector<Candidate>& candidates, const Reference& reference, const Session& session, std::size_t listLimit) {
    std::vector<std::size_t> perKind(reference.values.size());
    for (const auto& item : candidates) ++perKind[item.kind];
    for (std::size_t kind = 0; kind != perKind.size(); ++kind)
        if (perKind[kind]) std::printf("  %-18s %zu\n", reference.names[kind].c_str(), perKind[kind]);
    for (std::size_t index = 0; index != std::min(candidates.size(), listLimit); ++index)
        PrintCandidate(candidates[index], reference, session.base, session.imageSize);
}

// Brings JC3's visible top-level window forward. A synthetic Alt tap satisfies
// Windows' foreground-lock rule for a background process.
void FocusJc3(DWORD pid) {
    struct Search { DWORD pid; HWND window; } search{pid, nullptr};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* state = reinterpret_cast<Search*>(parameter);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == state->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) { state->window = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (!search.window) return;
    INPUT alt[2]{};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(search.window);
}

bool IsJc3Foreground(DWORD pid) {
    DWORD foregroundPid = 0;
    if (const HWND window = GetForegroundWindow()) GetWindowThreadProcessId(window, &foregroundPid);
    return foregroundPid == pid;
}

float AngleBetweenDegrees(const std::vector<float>& a, const std::vector<float>& b) {
    const float dot = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0f, 1.0f);
    return std::acos(dot) * 180.0f / kPi;
}

// Waits until two reads 250 ms apart agree, i.e. camera smoothing finished.
bool WaitForSettledCamera(const Session& session, Reference& reference) {
    Reference previous;
    if (!BuildReference(session.process, session.base, previous)) return false;
    for (int attempt = 0; attempt != 40; ++attempt) {
        Sleep(250);
        Reference current;
        if (!BuildReference(session.process, session.base, current)) return false;
        // First-person idle sway moves the camera slightly even at rest.
        if (AngleBetweenDegrees(previous.values[3], current.values[3]) < 0.3f) { reference = current; return true; }
        previous = current;
    }
    return false;
}

bool SendMouseMove(DWORD pid, int dx, int dy) {
    constexpr int kSteps = 20;
    for (int step = 0; step != kSteps; ++step) {
        if (!IsJc3Foreground(pid)) return false;
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = dx / kSteps;
        input.mi.dy = dy / kSteps;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(input));
        Sleep(15);
    }
    return true;
}

int RunAuto(const Session& session, const wchar_t* reportPath) {
    std::printf("waiting up to 60 s for JC3 to be the foreground window...\n");
    for (int wait = 0; wait != 240 && !IsJc3Foreground(session.pid); ++wait) {
        if (wait % 8 == 0) FocusJc3(session.pid);
        Sleep(250);
    }
    if (!IsJc3Foreground(session.pid)) { std::cerr << "JC3 never became foreground; no input sent\n"; return 10; }
    Reference reference;
    if (!WaitForSettledCamera(session, reference)) { std::cerr << "camera not settled/readable\n"; return 11; }
    auto candidates = ScanAll(session, reference);
    Reference settled;
    if (!WaitForSettledCamera(session, settled)) return 11;
    candidates = Filter(session, candidates, settled);
    std::printf("post-scan stability filter -> %zu\n", candidates.size());
    // Mouse counts per move: yaw right, pitch up, yaw left, then mixed moves.
    const int moves[][2]{{600, 0}, {0, -250}, {-900, 0}, {300, 350}, {450, -200}, {-300, 100}};
    int round = 0;
    for (const auto& move : moves) {
        ++round;
        Reference before;
        if (!BuildReference(session.process, session.base, before)) return 12;
        if (!SendMouseMove(session.pid, move[0], move[1])) { std::cerr << "JC3 lost foreground; stopping\n"; break; }
        Reference after;
        if (!WaitForSettledCamera(session, after)) { std::cerr << "camera did not settle after move\n"; break; }
        const float turned = AngleBetweenDegrees(before.values[3], after.values[3]);
        std::printf("round %d mouse=(%d,%d) camera turned %.1f deg yaw0=%.1f pitch=%.1f", round, move[0], move[1], turned,
            after.values[12][0], after.values[21][0]);
        if (turned < 5.0f) { std::printf(" -> too small, mouse input may not reach the camera; stopping\n"); break; }
        candidates = Filter(session, candidates, after);
        std::printf(" -> %zu remain\n", candidates.size());
        if (candidates.empty()) break;
    }
    Reference final;
    BuildReference(session.process, session.base, final);
    std::printf("=== surviving camera-state candidates ===\n");
    PrintSummary(candidates, final, session, 300);
    FILE* report = nullptr;
    if (_wfopen_s(&report, reportPath, L"w") == 0 && report) {
        for (const auto& item : candidates) {
            std::fprintf(report, "0x%016llx %s", static_cast<unsigned long long>(item.address), final.names[item.kind].c_str());
            if (item.address >= session.base && item.address < session.base + session.imageSize)
                std::fprintf(report, " JustCause3.exe+0x%llx", static_cast<unsigned long long>(item.address - session.base));
            std::fprintf(report, "\n");
        }
        std::fclose(report);
    }
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc == 3 ? argv[1] : L"";
    if (mode != L"init" && mode != L"filter" && mode != L"auto" && mode != L"press") {
        std::wcerr << L"usage: camera_state_scan init|filter|auto <file> | press enter|escape|space\n";
        return 1;
    }
    Session session;
    if (!OpenSession(session)) return 2;
    int result = 0;
    if (mode == L"press") {
        // Menu navigation for unattended runs: one scancode key press (the
        // form DirectInput keyboards observe) sent only while JC3 is focused.
        const std::wstring key = argv[2];
        const WORD scan = key == L"enter" ? 0x1C : key == L"escape" ? 0x01 : key == L"space" ? 0x39 : key == L"f2" ? 0x3C : key == L"f3" ? 0x3D : 0;
        for (int wait = 0; wait != 40 && !IsJc3Foreground(session.pid); ++wait) {
            if (wait % 8 == 0) FocusJc3(session.pid);
            Sleep(100);
        }
        if (!scan || !IsJc3Foreground(session.pid)) { std::cerr << "not sent (unknown key or JC3 not foreground)\n"; result = 13; }
        else {
            INPUT press[2]{};
            press[0].type = press[1].type = INPUT_KEYBOARD;
            press[0].ki.wScan = press[1].ki.wScan = scan;
            press[0].ki.dwFlags = KEYEVENTF_SCANCODE;
            press[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &press[0], sizeof(INPUT));
            Sleep(80);
            SendInput(1, &press[1], sizeof(INPUT));
            std::printf("pressed %ls\n", key.c_str());
        }
    } else if (mode == L"auto") {
        result = RunAuto(session, argv[2]);
    } else {
        Reference reference;
        if (!BuildReference(session.process, session.base, reference)) { std::cerr << "camera record unavailable\n"; CloseHandle(session.process); return 4; }
        std::vector<Candidate> candidates;
        if (mode == L"init") {
            candidates = ScanAll(session, reference);
        } else {
            std::ifstream input(argv[2], std::ios::binary);
            Candidate candidate{};
            std::vector<Candidate> previous;
            while (input.read(reinterpret_cast<char*>(&candidate), sizeof(candidate))) previous.push_back(candidate);
            candidates = Filter(session, previous, reference);
            std::printf("filter: %zu -> %zu\n", previous.size(), candidates.size());
        }
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(candidates.data()), static_cast<std::streamsize>(candidates.size() * sizeof(Candidate)));
        PrintSummary(candidates, reference, session, mode == L"init" && candidates.size() > 200 ? 0 : 200);
    }
    CloseHandle(session.process);
    return result;
}
