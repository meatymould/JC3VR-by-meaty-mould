// Read-only inspector for the running JustCause3.exe (ReadProcessMemory only).
// Used to follow pointer chains during RE without rebuilding the mod.
//   live_peek dump <address> [bytes]      qwords, floats, RTTI class of pointees, strings
//   live_peek floats <address> <count>    raw floats, 4 per line
//   live_peek find <vtable-rva> [max]     objects whose first qword is base+rva
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
HANDLE g_process = nullptr;
std::uintptr_t g_base = 0, g_end = 0;

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

bool FindImage(DWORD pid) {
    MODULEENTRY32W entry{sizeof(entry)};
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"JustCause3.exe") == 0) {
                g_base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                g_end = g_base + entry.modBaseSize;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return g_base != 0;
}

bool ReadBytes(std::uintptr_t address, void* out, std::size_t size) {
    SIZE_T count = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<const void*>(address), out, size, &count) && count == size;
}

template <typename T> bool Read(std::uintptr_t address, T& value) { return ReadBytes(address, &value, sizeof(value)); }

std::string ClassOfVtable(std::uintptr_t vtable) {
    if (vtable < g_base || vtable >= g_end) return {};
    std::uintptr_t locator = 0;
    std::uint32_t fields[6]{};
    if (!Read(vtable - 8, locator) || locator < g_base || locator >= g_end || !ReadBytes(locator, fields, sizeof(fields)) || fields[0] != 1) return {};
    char name[96]{};
    if (!ReadBytes(g_base + fields[3] + 16, name, sizeof(name) - 1) || name[0] != '.') return {};
    return name;
}

std::string Describe(std::uintptr_t value) {
    if (value >= g_base && value < g_end) {
        const std::string cls = ClassOfVtable(value);
        std::ostringstream out;
        out << "image+0x" << std::hex << (value - g_base);
        if (!cls.empty()) out << " vtable " << cls;
        return out.str();
    }
    std::uintptr_t first = 0;
    if (value > 0x10000 && value < 0x7FFFFFFFFFFF && Read(value, first)) {
        const std::string cls = ClassOfVtable(first);
        if (!cls.empty()) return "-> " + cls;
        char text[40]{};
        if (ReadBytes(value, text, sizeof(text) - 1)) {
            int printable = 0;
            while (printable < 39 && text[printable] >= 32 && text[printable] < 127) ++printable;
            if (printable >= 4 && text[printable] == 0) return std::string("-> \"") + text + "\"";
        }
        return "-> (readable)";
    }
    return {};
}

void Dump(std::uintptr_t address, std::size_t bytes) {
    std::vector<std::uint8_t> data(bytes);
    if (!ReadBytes(address, data.data(), bytes)) { std::cout << "read failed\n"; return; }
    for (std::size_t offset = 0; offset + 8 <= bytes; offset += 8) {
        std::uint64_t q;
        float f[2];
        std::memcpy(&q, data.data() + offset, 8);
        std::memcpy(f, data.data() + offset, 8);
        std::cout << "+0x" << std::hex << std::setw(4) << std::setfill('0') << offset << "  " << std::setw(16) << q << std::dec << std::setfill(' ')
                  << "  " << std::setw(12) << std::setprecision(6) << f[0] << ' ' << std::setw(12) << f[1] << "  " << Describe(q) << '\n';
    }
}

// Visits every committed read/write region in 1 MB chunks.
template <typename Visitor> void ForEachChunk(Visitor visit) {
    MEMORY_BASIC_INFORMATION info{};
    std::uintptr_t address = 0x10000;
    std::vector<std::uint8_t> buffer(1 << 20);
    while (VirtualQueryEx(g_process, reinterpret_cast<const void*>(address), &info, sizeof(info))) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const DWORD protect = info.Protect & 0xFF;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE)) {
            for (std::size_t chunk = 0; chunk < info.RegionSize; chunk += buffer.size() - 4096) {
                const std::size_t size = std::min(buffer.size(), info.RegionSize - chunk);
                SIZE_T count = 0;
                if (!ReadProcessMemory(g_process, reinterpret_cast<const void*>(base + chunk), buffer.data(), size, &count)) break;
                if (!visit(base + chunk, buffer.data(), static_cast<std::size_t>(count))) return;
                if (size < buffer.size()) break;
            }
        }
        address = base + info.RegionSize;
        if (address < base) break;
    }
}

// int16 parent-index arrays of exactly `count` bones: a[0] == -1 and
// 0 <= a[i] < i for all i > 0 (a valid tree in topological order).
void FindParents(std::size_t count, std::size_t max) {
    std::size_t found = 0;
    ForEachChunk([&](std::uintptr_t base, const std::uint8_t* data, std::size_t size) {
        for (std::size_t i = 0; i + count * 2 <= size; i += 2) {
            const auto* a = reinterpret_cast<const std::int16_t*>(data + i);
            if (a[0] != -1 || a[1] != 0) continue;
            bool tree = true;
            for (std::size_t k = 1; k < count && tree; ++k) tree = a[k] >= 0 && a[k] < static_cast<std::int16_t>(k);
            if (!tree) continue;
            // Real skeletons: many chains (a[k] == k-1) and many distinct parents.
            std::size_t chained = 0, distinct = 0;
            std::vector<bool> seen(count, false);
            for (std::size_t k = 1; k < count; ++k) {
                if (a[k] == static_cast<std::int16_t>(k - 1)) ++chained;
                if (!seen[a[k]]) { seen[a[k]] = true; ++distinct; }
            }
            if (chained < count / 3 || distinct < 25) continue;
            // Require the array to end there (next value not a valid parent).
            std::cout << "parents@0x" << std::hex << base + i << std::dec << ':';
            for (std::size_t k = 0; k < count; ++k) std::cout << ' ' << a[k];
            std::cout << '\n';
            if (++found >= max) return false;
            i += count * 2;
        }
        return true;
    });
}

// Qwords equal to `target` (who points here), up to `max`.
void FindRefs(std::uintptr_t target, std::uintptr_t window, std::size_t max) {
    std::size_t found = 0;
    ForEachChunk([&](std::uintptr_t base, const std::uint8_t* data, std::size_t size) {
        for (std::size_t i = 0; i + 8 <= size; i += 8) {
            std::uintptr_t q;
            std::memcpy(&q, data + i, 8);
            if (q < target || q > target + window) continue;
            std::cout << "ref@0x" << std::hex << base + i << " -> 0x" << q << std::dec << '\n';
            if (++found >= max) return false;
        }
        return true;
    });
}

// hkaSkeleton: +0x10 name, +0x18/+0x20 parents (int16), +0x28/+0x30 bones
// {name ptr, lockTranslation} x16, +0x38/+0x40 reference pose (hkQsTransform
// t,q,s x48, parent-local). Prints index, name, parent, local t, and the
// model-space rest position accumulated through the hierarchy.
void Skeleton(std::uintptr_t skeleton) {
    std::uintptr_t header[10]{};
    if (!ReadBytes(skeleton, header, sizeof(header))) { std::cout << "read failed\n"; return; }
    const std::uint32_t count = static_cast<std::uint32_t>(header[4] & 0xFFFFFFFF);
    std::vector<std::int16_t> parents(count);
    std::vector<std::uint8_t> bones(count * 16);
    std::vector<float> pose(count * 12);
    if (!ReadBytes(header[3], parents.data(), count * 2) || !ReadBytes(header[5], bones.data(), bones.size()) || !ReadBytes(header[7], pose.data(), pose.size() * 4)) {
        std::cout << "array read failed\n";
        return;
    }
    struct Xf { float t[3]; float q[4]; };
    std::vector<Xf> model(count);
    const auto rotate = [](const float* q, const float* v, float* out) {
        // q = (x, y, z, w); out = q v q*
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
        out[0] = v[0] + w * tx + (y * tz - z * ty);
        out[1] = v[1] + w * ty + (z * tx - x * tz);
        out[2] = v[2] + w * tz + (x * ty - y * tx);
    };
    const auto multiply = [](const float* a, const float* b, float* out) {  // a*b
        out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    };
    std::cout << "skeleton '" << [&] { char n[64]{}; ReadBytes(header[2] & ~std::uintptr_t(1), n, 63); return std::string(n); }() << "' bones=" << count << '\n';
    for (std::uint32_t i = 0; i < count; ++i) {
        const float* local = pose.data() + i * 12;
        Xf& m = model[i];
        const int parent = parents[i];
        if (parent < 0 || parent >= static_cast<int>(i)) {
            std::copy(local, local + 3, m.t);
            std::copy(local + 4, local + 8, m.q);
        } else {
            float rotated[3];
            rotate(model[parent].q, local, rotated);
            for (int c = 0; c < 3; ++c) m.t[c] = model[parent].t[c] + rotated[c];
            multiply(model[parent].q, local + 4, m.q);
        }
        std::uintptr_t namePointer = 0;
        std::memcpy(&namePointer, bones.data() + i * 16, 8);
        char name[64]{};
        ReadBytes(namePointer & ~std::uintptr_t(1), name, 63);
        std::cout << std::setw(3) << i << ' ' << std::left << std::setw(28) << name << std::right << " parent=" << std::setw(3) << parent
                  << std::fixed << std::setprecision(3) << " local=(" << local[0] << ',' << local[1] << ',' << local[2] << ") model=("
                  << m.t[0] << ',' << m.t[1] << ',' << m.t[2] << ")\n" << std::defaultfloat;
    }
}

// Rigid 4x4 row-major matrices (orthonormal 3x3, w column 0/0/0/1) inside
// [address, address+bytes), with their translation rows.
void Matrices(std::uintptr_t address, std::size_t bytes) {
    std::vector<float> f(bytes / 4);
    if (!ReadBytes(address, f.data(), f.size() * 4)) { std::cout << "read failed\n"; return; }
    for (std::size_t i = 0; i + 16 <= f.size(); i += 1) {
        const float* m = f.data() + i;
        auto dot = [&](int a, int b) { return m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2]; };
        bool ok = std::abs(m[3]) < 1e-4f && std::abs(m[7]) < 1e-4f && std::abs(m[11]) < 1e-4f && std::abs(m[15] - 1.0f) < 1e-4f;
        for (int r = 0; r < 3 && ok; ++r) ok = std::abs(dot(r, r) - 1.0f) < 0.01f;
        ok = ok && std::abs(dot(0, 1)) < 0.01f && std::abs(dot(0, 2)) < 0.01f && std::abs(dot(1, 2)) < 0.01f;
        if (!ok) continue;
        std::cout << "matrix +0x" << std::hex << i * 4 << std::dec << std::fixed << std::setprecision(3) << " rows=(" << m[0] << ',' << m[1] << ',' << m[2]
                  << ")(" << m[4] << ',' << m[5] << ',' << m[6] << ")(" << m[8] << ',' << m[9] << ',' << m[10] << ") t=(" << m[12] << ',' << m[13] << ',' << m[14] << ")\n"
                  << std::defaultfloat;
    }
}

// Skinning palettes {begin, end, ..., +0x48 count, +0x50 data} with `count`
// bones: begin == self+0x50 and end == begin + count*48.
void FindPalettes(std::size_t count, std::size_t max) {
    std::size_t found = 0;
    ForEachChunk([&](std::uintptr_t base, const std::uint8_t* data, std::size_t size) {
        for (std::size_t i = 0; i + 0x58 <= size; i += 8) {
            std::uint64_t begin, end, n;
            std::memcpy(&begin, data + i, 8);
            std::memcpy(&end, data + i + 8, 8);
            std::memcpy(&n, data + i + 0x48, 8);
            const std::uintptr_t self = base + i;
            if (n != count || begin != self + 0x50 || end != begin + count * 48) continue;
            float hips[12]{};
            ReadBytes(begin + 2 * 48, hips, sizeof(hips));
            std::cout << "palette@0x" << std::hex << self << std::dec << std::fixed << std::setprecision(3) << " hips t=(" << hips[3] << ',' << hips[7] << ',' << hips[11] << ")\n" << std::defaultfloat;
            if (++found >= max) return false;
        }
        return true;
    });
}

void Find(std::uintptr_t rva, std::size_t max) {    const std::uintptr_t needle = g_base + rva;
    std::cout << "searching for vtable " << ClassOfVtable(needle) << '\n';
    MEMORY_BASIC_INFORMATION info{};
    std::uintptr_t address = 0x10000;
    std::vector<std::uint8_t> buffer(1 << 20);
    std::size_t found = 0;
    while (found < max && VirtualQueryEx(g_process, reinterpret_cast<const void*>(address), &info, sizeof(info))) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const DWORD protect = info.Protect & 0xFF;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE)) {
            for (std::size_t chunk = 0; chunk < info.RegionSize && found < max; chunk += buffer.size()) {
                const std::size_t size = std::min(buffer.size(), info.RegionSize - chunk);
                SIZE_T count = 0;
                if (!ReadProcessMemory(g_process, reinterpret_cast<const void*>(base + chunk), buffer.data(), size, &count)) break;
                for (std::size_t i = 0; i + 8 <= count && found < max; i += 8) {
                    std::uintptr_t q;
                    std::memcpy(&q, buffer.data() + i, 8);
                    if (q == needle) { std::cout << "0x" << std::hex << base + chunk + i << std::dec << '\n'; ++found; }
                }
            }
        }
        address = base + info.RegionSize;
        if (address < base) break;
    }
}
}

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: live_peek dump <addr> [bytes] | floats <addr> <count> | find <rva> [max]\n"; return 2; }
    const DWORD pid = FindProcess();
    if (!pid) { std::cerr << "JustCause3.exe not running\n"; return 1; }
    g_process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_process || !FindImage(pid)) { std::cerr << "cannot open process\n"; return 1; }
    const std::string command = argv[1];
    const std::uintptr_t value = std::strtoull(argv[2], nullptr, 16);
    if (command == "dump") Dump(value, argc > 3 ? std::strtoull(argv[3], nullptr, 16) : 0x100);
    else if (command == "floats") {
        const std::size_t count = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 64;
        std::vector<float> f(count);
        if (!ReadBytes(value, f.data(), count * 4)) { std::cout << "read failed\n"; return 1; }
        for (std::size_t i = 0; i < count; ++i) std::cout << (i % 4 ? " " : (i ? "\n+0x" : "+0x")) << (i % 4 ? "" : [&] { std::ostringstream o; o << std::hex << i * 4 << "  "; return o.str(); }()) << f[i];
        std::cout << '\n';
    } else if (command == "find") Find(value, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 16);
    else if (command == "skeleton") Skeleton(value);
    else if (command == "matrices") Matrices(value, argc > 3 ? std::strtoull(argv[3], nullptr, 16) : 0x400);
    else if (command == "parents") FindParents(std::strtoull(argv[2], nullptr, 10), argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 8);
    else if (command == "palettes") FindPalettes(std::strtoull(argv[2], nullptr, 10), argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 16);
    else if (command == "refs") FindRefs(value, argc > 3 ? std::strtoull(argv[3], nullptr, 16) : 0, argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 16);
    CloseHandle(g_process);
    return 0;
}
