#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {
constexpr std::uintptr_t kCameraOwnerGlobalRva = 0x02ED0E20;

DWORD FindProcess() {
    PROCESSENTRY32W entry{sizeof(entry)};
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"JustCause3.exe") == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

std::uintptr_t FindImageBase(DWORD pid) {
    MODULEENTRY32W entry{sizeof(entry)};
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    std::uintptr_t base = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"JustCause3.exe") == 0) {
                base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return base;
}

template <typename T>
bool Read(HANDLE process, std::uintptr_t address, T& value) {
    SIZE_T count = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value, sizeof(value), &count) && count == sizeof(value);
}

void PrintMatrix(const char* label, const std::array<std::byte, 0x600>& bytes, std::size_t offset) {
    if (offset + 64 > bytes.size()) return;
    const auto* f = reinterpret_cast<const float*>(bytes.data() + offset);
    std::cout << label << "@0x" << std::hex << offset << std::dec;
    for (int row = 0; row < 4; ++row) {
        std::cout << " [";
        for (int col = 0; col < 4; ++col) {
            if (col) std::cout << ',';
            std::cout << std::fixed << std::setprecision(9) << f[row * 4 + col];
        }
        std::cout << ']';
    }
    std::cout << '\n';
}

void PrintCandidate(HANDLE process, const char* label, std::uintptr_t pointer) {
    std::cout << label << "=0x" << std::hex << pointer << std::dec << '\n';
    if (!pointer) return;
    std::array<std::byte, 0x600> bytes{};
    SIZE_T count = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(pointer), bytes.data(), bytes.size(), &count) || count != bytes.size()) {
        std::cout << "  read failed error=" << GetLastError() << " bytes=" << count << '\n';
        return;
    }
    for (const std::size_t offset : {0x14u, 0x54u, 0x94u, 0xD4u, 0x114u, 0x154u, 0x194u, 0x1D4u, 0x214u, 0x254u, 0x294u, 0x2D4u, 0x314u, 0x354u, 0x394u, 0x3D4u, 0x414u, 0x454u, 0x494u}) {
        PrintMatrix("  matrix", bytes, offset);
    }
    const auto* world = reinterpret_cast<const float*>(bytes.data() + 0x94);
    const auto* projection = reinterpret_cast<const float*>(bytes.data() + 0x154);
    const auto* viewProjection = reinterpret_cast<const float*>(bytes.data() + 0x194);
    double viewTranslation[3]{};
    for (int axis = 0; axis != 3; ++axis)
        for (int component = 0; component != 3; ++component)
            viewTranslation[axis] -= static_cast<double>(world[axis * 4 + component]) * world[12 + component];
    double maximumResidual = 0.0;
    for (int column = 0; column != 4; ++column) {
        double expected = projection[12 + column];
        for (int axis = 0; axis != 3; ++axis)
            expected += viewTranslation[axis] * projection[axis * 4 + column];
        maximumResidual = std::max(maximumResidual, std::abs(expected - viewProjection[12 + column]));
    }
    std::cout << "  vp_translation_residual_max=" << std::fixed << std::setprecision(9) << maximumResidual << '\n';
}
}

int wmain() {
    const DWORD pid = FindProcess();
    if (!pid) {
        std::cerr << "JustCause3.exe not found\n";
        return 1;
    }
    const std::uintptr_t base = FindImageBase(pid);
    if (!base) {
        std::cerr << "module snapshot failed error=" << GetLastError() << '\n';
        return 2;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        std::cerr << "OpenProcess failed error=" << GetLastError() << '\n';
        return 3;
    }
    std::uintptr_t owner = 0;
    if (!Read(process, base + kCameraOwnerGlobalRva, owner)) {
        std::cerr << "owner global read failed error=" << GetLastError() << '\n';
        CloseHandle(process);
        return 4;
    }
    std::uintptr_t paired = 0;
    std::uintptr_t projection = 0;
    if (owner) {
        Read(process, owner + 0x5C0, paired);
        Read(process, owner + 0x5C8, projection);
    }
    std::cout << "pid=" << pid << " base=0x" << std::hex << base << " owner=0x" << owner << std::dec << '\n';
    // Class identity: the first qword of a polymorphic object is its vtable;
    // its image-relative RVA matches tools/rtti_classes.py output.
    for (const auto& [label, object] : {std::pair{"owner", owner}, std::pair{"paired_camera", paired}, std::pair{"projection_camera", projection}}) {
        std::uintptr_t vtable = 0;
        if (object && Read(process, object, vtable) && vtable >= base && vtable < base + 0x03241000)
            std::cout << label << "_vtable_rva=0x" << std::hex << (vtable - base) << std::dec << '\n';
        else
            std::cout << label << "_vtable_rva=none\n";
    }
    PrintCandidate(process, "paired_camera", paired);
    PrintCandidate(process, "projection_camera", projection);
    // Layout map: every 4-byte offset in the first 0x600 bytes of each record
    // holding a rigid transform (world or inverse-view) or a unit-normal plane
    // quad block, so companion view/frustum fields can be located.
    for (const auto& [label, object] : {std::pair{"paired", paired}, std::pair{"projection", projection}, std::pair{"owner+0x10", owner + 0x10}}) {
        std::array<float, 0x600 / 4> f{};
        SIZE_T got = 0;
        if (!object || !ReadProcessMemory(process, reinterpret_cast<const void*>(object), f.data(), sizeof(f), &got) || got != sizeof(f)) continue;
        std::cout << "layout " << label << "=0x" << std::hex << object << std::dec << '\n';
        for (std::size_t i = 0; i + 16 <= f.size(); ++i) {
            const float* m = f.data() + i;
            auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
            auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
            const bool rigid = std::abs(len(m) - 1) < 0.01f && std::abs(len(m + 4) - 1) < 0.01f && std::abs(len(m + 8) - 1) < 0.01f &&
                std::abs(dot(m, m + 4)) < 0.01f && std::abs(dot(m + 4, m + 8)) < 0.01f &&
                std::abs(m[3]) < 1e-4f && std::abs(m[7]) < 1e-4f && std::abs(m[11]) < 1e-4f && std::abs(m[15] - 1) < 1e-4f;
            if (rigid)
                std::cout << "  +0x" << std::hex << i * 4 << std::dec << std::fixed << std::setprecision(4) << " rigid row2=["
                          << m[8] << ',' << m[9] << ',' << m[10] << "] t=[" << m[12] << ',' << m[13] << ',' << m[14] << "]\n";
            const bool planes = std::abs(len(m) - 1) < 0.001f && std::abs(len(m + 4) - 1) < 0.001f &&
                std::abs(len(m + 8) - 1) < 0.001f && std::abs(len(m + 12) - 1) < 0.001f && std::abs(m[3]) > 1.0f && !rigid;
            if (planes)
                std::cout << "  +0x" << std::hex << i * 4 << std::dec << std::fixed << std::setprecision(4) << " planes n0=["
                          << m[0] << ',' << m[1] << ',' << m[2] << "] d0=" << m[3] << '\n';
        }
    }
    CloseHandle(process);
    return owner && projection ? 0 : 5;
}
