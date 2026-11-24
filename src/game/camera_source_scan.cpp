#include "game/camera_source_scan.h"
#include "game/camera_capture.h"
#include "core/logging.h"
#include <Windows.h>
#include <psapi.h>
#include <atomic>
#include <unordered_map>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace jc3vr {
namespace {
bool g_enabled = false;
std::atomic<int> g_state = 0;  // 0 waiting, 1 scanning, 2 done
int g_attempts = 0;
float g_lastRows[9]{};
int g_stableFrames = 0;

struct Hit {
    std::uintptr_t address;
    bool layout4x4;  // rows 16 bytes apart (else 12)
};

// SEH-guarded bulk read; false if the page vanished mid-scan.
bool SafeCopy(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t g_imageBase = 0, g_imageEnd = 0;

struct PublishedHit {
    std::uintptr_t address;
    std::string className;   // "" when no vtable was found
    std::uintptr_t object;   // owning object base (0 if none)
    std::uintptr_t vtable;   // vtable at scan time (heap objects are re-checked)
};
std::mutex g_hitsMutex;
std::vector<PublishedHit> g_published;

// Class name for a vtable pointer inside the image (MSVC x64 RTTI), or "".
std::string ClassName(std::uintptr_t vtable) {
    if (vtable < g_imageBase + 0x1000 || vtable >= g_imageEnd) return {};
    std::uintptr_t locator = 0;
    if (!SafeCopy(&locator, reinterpret_cast<const void*>(vtable - 8), 8)) return {};
    if (locator < g_imageBase || locator >= g_imageEnd) return {};
    std::uint32_t fields[6]{};
    if (!SafeCopy(fields, reinterpret_cast<const void*>(locator), sizeof(fields)) || fields[0] != 1) return {};
    char name[96]{};
    if (!SafeCopy(name, reinterpret_cast<const void*>(g_imageBase + fields[3] + 16), sizeof(name) - 1)) return {};
    if (name[0] != '.' || name[1] != '?') return {};
    return std::string(name);
}

// Nearest preceding 8-byte-aligned vtable (within 0x1000 bytes).
std::string Attribute(std::uintptr_t address) {
    for (std::uintptr_t offset = 0; offset <= 0x1000; offset += 8) {
        const std::uintptr_t candidate = (address & ~std::uintptr_t(7)) - offset;
        std::uintptr_t value = 0;
        if (!SafeCopy(&value, reinterpret_cast<const void*>(candidate), 8)) break;
        const std::string name = ClassName(value);
        if (!name.empty()) {
            std::ostringstream out;
            out << name << "+0x" << std::hex << (address - candidate) << " (object 0x" << candidate << ")";
            return out.str();
        }
    }
    return "no vtable within 0x1000";
}

void Scan(std::vector<float> rows) {
    const auto started = GetTickCount64();
    std::vector<Hit> hits;
    // Exclude JC3VR's own module (it keeps copies of the engine basis).
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Scan), &self);
    MODULEINFO selfInfo{};
    std::uintptr_t selfBegin = 0, selfEnd = 0;
    if (self) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<std::uintptr_t>(self) + dos->e_lfanew);
        selfBegin = reinterpret_cast<std::uintptr_t>(self);
        selfEnd = selfBegin + nt->OptionalHeader.SizeOfImage;
    }
    (void)selfInfo;
    std::uint64_t scannedBytes = 0;
    std::vector<std::uint8_t> buffer(1 << 20);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* address = static_cast<std::uint8_t*>(system.lpMinimumApplicationAddress);
    MEMORY_BASIC_INFORMATION info{};
    while (address < system.lpMaximumApplicationAddress && VirtualQuery(address, &info, sizeof(info))) {
        auto* next = static_cast<std::uint8_t*>(info.BaseAddress) + info.RegionSize;
        const DWORD protect = info.Protect & 0xFF;
        const bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE);
        if (readable) {
            for (std::size_t chunk = 0; chunk < info.RegionSize; chunk += buffer.size() - 64) {
                const std::size_t size = std::min(buffer.size(), info.RegionSize - chunk);
                auto* source = static_cast<std::uint8_t*>(info.BaseAddress) + chunk;
                if (!SafeCopy(buffer.data(), source, size)) break;
                scannedBytes += size;
                for (std::size_t i = 0; i + 44 <= size; i += 4) {
                    if (std::memcmp(buffer.data() + i, rows.data(), 12) != 0) continue;
                    // Second row 16 bytes on (4x4 layout) or 12 (3x3 layout).
                    const bool layout4x4 = std::memcmp(buffer.data() + i + 16, rows.data() + 3, 12) == 0 &&
                                           std::memcmp(buffer.data() + i + 32, rows.data() + 6, 12) == 0;
                    const bool layout3x3 = std::memcmp(buffer.data() + i + 12, rows.data() + 3, 24) == 0;
                    const auto hitAddress = reinterpret_cast<std::uintptr_t>(source + i);
                    const auto rowsBegin = reinterpret_cast<std::uintptr_t>(rows.data());
                    const bool own = (hitAddress >= selfBegin && hitAddress < selfEnd) ||
                        (hitAddress >= rowsBegin && hitAddress < rowsBegin + 36) ||
                        (hitAddress >= reinterpret_cast<std::uintptr_t>(buffer.data()) && hitAddress < reinterpret_cast<std::uintptr_t>(buffer.data()) + buffer.size());
                    if ((layout4x4 || layout3x3) && !own && hits.size() < 200) hits.push_back({hitAddress, layout4x4});
                }
                if (size < buffer.size()) break;
            }
        }
        address = next;
    }
    std::ostringstream summary;
    summary << "camera source scan: rotation row0=(" << rows[0] << ',' << rows[1] << ',' << rows[2] << ") hits=" << hits.size()
            << " scanned_mb=" << scannedBytes / (1024 * 1024) << " ms=" << GetTickCount64() - started;
    TraceRuntime(summary.str());
    for (const auto& hit : hits) {
        std::ostringstream line;
        line << "camera source scan hit 0x" << std::hex << hit.address << std::dec << (hit.layout4x4 ? " 4x4" : " 3x3") << " -> " << Attribute(hit.address);
        // For 4x4 layouts, the translation row sits 48 bytes on.
        if (hit.layout4x4) {
            float translation[3]{};
            if (SafeCopy(translation, reinterpret_cast<const void*>(hit.address + 48), sizeof(translation)))
                line << " t=(" << translation[0] << ',' << translation[1] << ',' << translation[2] << ')';
        }
        TraceRuntime(line.str());
        if (hit.layout4x4) {
            PublishedHit published{hit.address, {}, 0, 0};
            for (std::uintptr_t offset = 0; offset <= 0x1000; offset += 8) {
                const std::uintptr_t candidate = (hit.address & ~std::uintptr_t(7)) - offset;
                std::uintptr_t value = 0;
                if (!SafeCopy(&value, reinterpret_cast<const void*>(candidate), 8)) break;
                const std::string name = ClassName(value);
                if (!name.empty()) { published = {hit.address, name, candidate, value}; break; }
            }
            std::lock_guard lock(g_hitsMutex);
            g_published.push_back(published);
        }
    }
    g_state.store(2);
}
}

// ---- Engine FOV source scan -----------------------------------------------
namespace {
bool g_fovScanEnabled = false;
std::atomic<float> g_engineFov = 0.0f;
std::atomic<int> g_fovScanState = 0;

void ScanFov(float value) {
    const auto started = GetTickCount64();
    std::vector<std::uintptr_t> hits;
    std::vector<std::uint8_t> buffer(1 << 20);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* address = static_cast<std::uint8_t*>(system.lpMinimumApplicationAddress);
    MEMORY_BASIC_INFORMATION info{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ScanFov), &self);
    std::uintptr_t selfBegin = reinterpret_cast<std::uintptr_t>(self), selfEnd = selfBegin;
    if (self) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        selfEnd = selfBegin + reinterpret_cast<const IMAGE_NT_HEADERS64*>(selfBegin + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    std::uint64_t total = 0;
    while (address < system.lpMaximumApplicationAddress && VirtualQuery(address, &info, sizeof(info))) {
        auto* next = static_cast<std::uint8_t*>(info.BaseAddress) + info.RegionSize;
        const DWORD protect = info.Protect & 0xFF;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE)) {
            for (std::size_t chunk = 0; chunk < info.RegionSize; chunk += buffer.size()) {
                const std::size_t size = std::min(buffer.size(), info.RegionSize - chunk);
                auto* source = static_cast<std::uint8_t*>(info.BaseAddress) + chunk;
                if (!SafeCopy(buffer.data(), source, size)) break;
                for (std::size_t i = 0; i + 4 <= size; i += 4) {
                    std::uint32_t word;
                    std::memcpy(&word, buffer.data() + i, 4);
                    if (word != bits) continue;
                    const auto hit = reinterpret_cast<std::uintptr_t>(source + i);
                    if (hit >= selfBegin && hit < selfEnd) continue;
                    if (hit >= reinterpret_cast<std::uintptr_t>(buffer.data()) && hit < reinterpret_cast<std::uintptr_t>(buffer.data()) + buffer.size()) continue;
                    ++total;
                    if (hits.size() < 400) hits.push_back(hit);
                }
            }
        }
        address = next;
    }
    TraceRuntime("engine FOV scan: value=" + std::to_string(value) + " rad total_hits=" + std::to_string(total) + " ms=" + std::to_string(GetTickCount64() - started));
    RegisterCameraFovHits(hits);
    int camera = 0, other = 0;
    for (const auto hit : hits) {
        const std::string owner = Attribute(hit);
        const bool interesting = owner.find("Camera") != std::string::npos || owner.find("camera") != std::string::npos;
        if (interesting ? camera++ < 60 : other++ < 25) {
            float neighbours[6]{};
            SafeCopy(neighbours, reinterpret_cast<const void*>(hit - 8), sizeof(neighbours));
            std::ostringstream line;
            line << "engine FOV scan hit 0x" << std::hex << hit << std::dec << " -> " << owner << " around=[" << neighbours[0] << ',' << neighbours[1]
                 << ",*" << neighbours[2] << ',' << neighbours[3] << ',' << neighbours[4] << ',' << neighbours[5] << ']';
            TraceRuntime(line.str());
        }
    }
    g_fovScanState.store(2);
}
}

void NoteEngineCameraFov(float radians) {
    if (radians > 0.3f && radians < 2.5f && g_engineFov.load() == 0.0f) g_engineFov.store(radians);
}

void ConfigureFovSourceScan(bool enabled) {
    g_fovScanEnabled = enabled;
    if (enabled) TraceRuntime("engine FOV source scan enabled (one-shot, read-only)");
}

// ---- Skeleton probe (read-only, one shot) -----------------------------------
// Locates Rico's skeleton data for the VR body: Havok hkaSkeleton objects
// (names, bones, parents), hkaAnimatedSkeleton instances, the first-person
// camera-bone proxy, world-space bone matrices near the camera, and
// hkQsTransform pose arrays. Class identity is by vtable (RTTI-verified).
namespace {
bool g_skeletonProbeEnabled = false;
std::atomic<int> g_skeletonProbeState = 0;

struct ProbeClass {
    const char* label;
    std::uintptr_t rva;
    const char* rtti;
    std::vector<std::uintptr_t> objects;
    std::uintptr_t vtable = 0;
};

std::string ReadCString(std::uintptr_t pointer, std::size_t max = 64) {
    pointer &= ~std::uintptr_t(1);  // hkStringPtr keeps a flag in bit 0
    char text[128]{};
    if (!pointer || !SafeCopy(text, reinterpret_cast<const void*>(pointer), std::min<std::size_t>(max, sizeof(text) - 1))) return {};
    std::string out;
    for (char c : text) {
        if (!c) break;
        if (c < 32 || c > 126) return {};
        out += c;
    }
    return out;
}

bool Orthonormal(const float* m) {
    const auto dot = [&](int a, int b) { return m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2]; };
    for (int r = 0; r != 3; ++r) if (std::abs(dot(r, r) - 1.0f) > 0.02f) return false;
    return std::abs(dot(0, 1)) < 0.02f && std::abs(dot(0, 2)) < 0.02f && std::abs(dot(1, 2)) < 0.02f && std::abs(m[15] - 1.0f) < 1e-3f;
}

void SkeletonProbe(std::array<float, 3> camera, std::array<float, 9> basis) {
    const auto started = GetTickCount64();
    std::vector<ProbeClass> classes{
        {"hkaSkeleton", 0x0258bd30, ".?AVhkaSkeleton@@"},
        {"hkaAnimatedSkeleton", 0x0283c0f8, ".?AVhkaAnimatedSkeleton@@"},
        {"CAnimatedModel", 0x0237e850, ".?AVCAnimatedModel@@"},
        {"CSkeletalAnimatedObject", 0x02461eb8, ".?AVCSkeletalAnimatedObject@@"},
        {"CMatrixProxy_CharacterCameraBone", 0x02428110, ".?AVCMatrixProxy_CharacterCameraBone@@"},
        {"CMatrixProxy_Skeletal", 0x02427a98, ".?AVCMatrixProxy_Skeletal@@"},
        {"CBoneAttachment", 0x023c7aa8, ".?AVCBoneAttachment@@"},
    };
    for (auto& probe : classes) {
        const std::uintptr_t vtable = g_imageBase + probe.rva;
        const std::string name = ClassName(vtable);
        probe.vtable = name == probe.rtti ? vtable : 0;
        if (!probe.vtable) TraceRuntime(std::string("skeleton probe: RTTI mismatch for ") + probe.label + " (" + name + "); skipped");
    }
    struct MatrixHit { std::uintptr_t address; float t[3]; };
    std::vector<MatrixHit> nearMatrices;
    struct QsRun { std::uintptr_t address; std::size_t count; };
    std::vector<QsRun> qsRuns;
    std::vector<std::uint8_t> buffer(1 << 20);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* address = static_cast<std::uint8_t*>(system.lpMinimumApplicationAddress);
    MEMORY_BASIC_INFORMATION info{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&SkeletonProbe), &self);
    const std::uintptr_t selfBegin = reinterpret_cast<std::uintptr_t>(self);
    const std::uintptr_t selfEnd = self ? selfBegin + reinterpret_cast<const IMAGE_NT_HEADERS64*>(selfBegin + reinterpret_cast<const IMAGE_DOS_HEADER*>(self)->e_lfanew)->OptionalHeader.SizeOfImage : selfBegin;
    std::uint64_t scanned = 0;
    while (address < system.lpMaximumApplicationAddress && VirtualQuery(address, &info, sizeof(info))) {
        auto* next = static_cast<std::uint8_t*>(info.BaseAddress) + info.RegionSize;
        const DWORD protect = info.Protect & 0xFF;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE) &&
            !(regionBase >= selfBegin && regionBase < selfEnd)) {
            std::size_t qsRunStart = 0, qsRunCount = 0;
            for (std::size_t chunk = 0; chunk < info.RegionSize; chunk += buffer.size()) {
                const std::size_t size = std::min(buffer.size(), info.RegionSize - chunk);
                auto* source = static_cast<std::uint8_t*>(info.BaseAddress) + chunk;
                if (!SafeCopy(buffer.data(), source, size)) break;
                scanned += size;
                const auto* bytes = buffer.data();
                for (std::size_t i = 0; i + 64 <= size; i += 8) {
                    std::uintptr_t qword;
                    std::memcpy(&qword, bytes + i, 8);
                    for (auto& probe : classes)
                        if (probe.vtable && qword == probe.vtable && probe.objects.size() < 256) probe.objects.push_back(reinterpret_cast<std::uintptr_t>(source + i));
                    if (i % 16) continue;
                    const auto* f = reinterpret_cast<const float*>(bytes + i);
                    // World matrix (row-major, translation row 3) near the camera.
                    const float dx = f[12] - camera[0], dy = f[13] - camera[1], dz = f[14] - camera[2];
                    if (dx * dx + dy * dy + dz * dz < 9.0f && Orthonormal(f) && nearMatrices.size() < 4000)
                        nearMatrices.push_back({reinterpret_cast<std::uintptr_t>(source + i), {dx, dy, dz}});
                }
                // hkQsTransform runs (t, q, s with s == 1 exactly), stride 48.
                for (std::size_t i = 0; i + 48 <= size; i += 16) {
                    std::uint32_t s[3];
                    std::memcpy(s, bytes + i + 32, 12);
                    if (s[0] != 0x3F800000u || s[1] != 0x3F800000u || s[2] != 0x3F800000u) continue;
                    const auto* f = reinterpret_cast<const float*>(bytes + i);
                    const float q = f[4] * f[4] + f[5] * f[5] + f[6] * f[6] + f[7] * f[7];
                    if (std::abs(q - 1.0f) > 0.01f || f[0] * f[0] + f[1] * f[1] + f[2] * f[2] > 9.0f) continue;
                    const std::size_t absolute = chunk + i;
                    if (qsRunCount && absolute == qsRunStart + qsRunCount * 48) { ++qsRunCount; }
                    else {
                        if (qsRunCount >= 40 && qsRuns.size() < 2000) qsRuns.push_back({regionBase + qsRunStart, qsRunCount});
                        qsRunStart = absolute;
                        qsRunCount = 1;
                    }
                    i += 32;  // next candidate starts 48 bytes on
                }
            }
            if (qsRunCount >= 40 && qsRuns.size() < 2000) qsRuns.push_back({regionBase + qsRunStart, qsRunCount});
        }
        address = next;
    }
    TraceRuntime("skeleton probe: scanned_mb=" + std::to_string(scanned >> 20) + " ms=" + std::to_string(GetTickCount64() - started) +
        " camera=(" + std::to_string(camera[0]) + "," + std::to_string(camera[1]) + "," + std::to_string(camera[2]) + ")");
    for (const auto& probe : classes)
        TraceRuntime(std::string("skeleton probe: ") + probe.label + " objects=" + std::to_string(probe.objects.size()));

    // Havok skeletons (hkReferencedObject 16 bytes, then m_name, m_parentIndices,
    // m_bones {name ptr, lockTranslation} x16 bytes, m_referencePose ...).
    std::vector<std::string> seenNames;
    std::unordered_map<std::uintptr_t, std::string> skeletonNames;
    for (const auto object : classes[0].objects) {
        std::uintptr_t header[16]{};
        if (!SafeCopy(header, reinterpret_cast<const void*>(object), sizeof(header))) continue;
        const std::string name = ReadCString(header[2]);
        std::uint32_t boneCount = 0;
        std::memcpy(&boneCount, reinterpret_cast<const std::uint8_t*>(header) + 0x30, 4);
        skeletonNames[object] = name;
        if (std::find(seenNames.begin(), seenNames.end(), name) != seenNames.end()) continue;
        seenNames.push_back(name);
        std::ostringstream raw;
        for (int k = 0; k != 10; ++k) raw << ' ' << std::hex << header[k];
        TraceRuntime("skeleton probe: hkaSkeleton 0x" + [&] { std::ostringstream o; o << std::hex << object; return o.str(); }() +
            " name='" + name + "' bones=" + std::to_string(boneCount) + " raw:" + raw.str());
        if (boneCount < 20 || boneCount > 600 || seenNames.size() > 8) continue;
        std::vector<std::int16_t> parents(boneCount, -1);
        SafeCopy(parents.data(), reinterpret_cast<const void*>(header[3]), boneCount * sizeof(std::int16_t));
        std::ostringstream line;
        for (std::uint32_t bone = 0; bone != boneCount; ++bone) {
            std::uintptr_t boneName = 0;
            SafeCopy(&boneName, reinterpret_cast<const void*>(header[5] + bone * 16), 8);
            line << ' ' << bone << ':' << ReadCString(boneName, 48) << '<' << parents[bone];
            if (bone % 24 == 23 || bone + 1 == boneCount) {
                TraceRuntime("skeleton probe: '" + name + "' bones" + line.str());
                line.str({});
            }
        }
    }
    // Animated skeleton instances -> which skeleton (hkArray controls at
    // +0x10, m_skeleton at +0x20).
    std::unordered_map<std::string, int> instancesPerSkeleton;
    for (const auto object : classes[1].objects) {
        std::uintptr_t skeleton = 0;
        SafeCopy(&skeleton, reinterpret_cast<const void*>(object + 0x20), 8);
        const auto found = skeletonNames.find(skeleton);
        ++instancesPerSkeleton[found != skeletonNames.end() ? found->second : "?"];
    }
    for (const auto& [name, count] : instancesPerSkeleton)
        TraceRuntime("skeleton probe: hkaAnimatedSkeleton instances for '" + name + "': " + std::to_string(count));
    // The first-person camera-bone proxies: raw dumps (character pointer,
    // bone index, offset are in here somewhere).
    for (std::size_t k = 0; k != std::min<std::size_t>(classes[4].objects.size(), 4); ++k) {
        std::uintptr_t raw[16]{};
        SafeCopy(raw, reinterpret_cast<const void*>(classes[4].objects[k]), sizeof(raw));
        std::ostringstream line;
        line << "skeleton probe: CMatrixProxy_CharacterCameraBone 0x" << std::hex << classes[4].objects[k] << ':';
        for (const auto value : raw) line << ' ' << value;
        TraceRuntime(line.str());
    }
    // World matrices near the camera, grouped into strided runs.
    std::sort(nearMatrices.begin(), nearMatrices.end(), [](const MatrixHit& a, const MatrixHit& b) { return a.address < b.address; });
    int reported = 0;
    for (std::size_t first = 0; first < nearMatrices.size() && reported < 12;) {
        std::size_t last = first;
        std::uintptr_t stride = 0;
        while (last + 1 < nearMatrices.size()) {
            const std::uintptr_t step = nearMatrices[last + 1].address - nearMatrices[last].address;
            if (step > 0x100 || (stride && step != stride)) break;
            stride = step;
            ++last;
        }
        const std::size_t count = last - first + 1;
        if (count >= 8) {
            ++reported;
            std::ostringstream line;
            line << "skeleton probe: near-camera matrix run 0x" << std::hex << nearMatrices[first].address << std::dec << " stride=" << stride
                 << " count=" << count << " owner=" << Attribute(nearMatrices[first].address) << " rel_to_camera(right,up,fwd):";
            for (std::size_t k = first; k <= last && k < first + 64; ++k) {
                const float* t = nearMatrices[k].t;
                line << " [" << static_cast<int>(100 * (t[0] * basis[0] + t[1] * basis[1] + t[2] * basis[2])) << ','
                     << static_cast<int>(100 * (t[0] * basis[3] + t[1] * basis[4] + t[2] * basis[5])) << ','
                     << static_cast<int>(-100 * (t[0] * basis[6] + t[1] * basis[7] + t[2] * basis[8])) << ']';
            }
            TraceRuntime(line.str());
        }
        first = last + 1;
    }
    TraceRuntime("skeleton probe: near-camera matrices=" + std::to_string(nearMatrices.size()) + " qs_runs(>=40)=" + std::to_string(qsRuns.size()));
    std::sort(qsRuns.begin(), qsRuns.end(), [](const QsRun& a, const QsRun& b) { return a.count > b.count; });
    for (std::size_t k = 0; k != std::min<std::size_t>(qsRuns.size(), 12); ++k) {
        std::ostringstream line;
        line << "skeleton probe: hkQsTransform run 0x" << std::hex << qsRuns[k].address << std::dec << " count=" << qsRuns[k].count << " owner=" << Attribute(qsRuns[k].address);
        TraceRuntime(line.str());
    }
    g_skeletonProbeState.store(2);
}
}

void ConfigureSkeletonProbe(bool enabled) {
    g_skeletonProbeEnabled = enabled;
    if (enabled) TraceRuntime("skeleton probe enabled (one-shot, read-only)");
}

void SkeletonProbeTick() {
    if (!g_skeletonProbeEnabled || g_skeletonProbeState.load() != 0) return;
    static int frames = 0;
    if (!IsNativeSceneRendering() || !HasNativeGameplayCamera()) { frames = 0; return; }
    if (++frames < 900) return;
    float position[3]{}, rows[9]{};
    if (!GetEngineCameraPosition(position) || !GetEngineCameraBasis(rows)) return;
    if (!g_imageBase) {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        g_imageBase = base;
        g_imageEnd = base + reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    }
    g_skeletonProbeState.store(1);
    TraceRuntime("skeleton probe: scanning process memory on a background thread");
    std::array<float, 3> camera{position[0], position[1], position[2]};
    std::array<float, 9> basis{};
    std::copy(rows, rows + 9, basis.begin());
    std::thread(SkeletonProbe, camera, basis).detach();
}

void ConfigureCameraSourceScan(bool enabled) {
    g_enabled = enabled;
    if (enabled) TraceRuntime("camera source scan enabled (one-shot, read-only)");
}

void CameraSourceScanTick() {
    if ((g_fovScanEnabled || GetCullingMode() >= 3) && g_fovScanState.load() == 0) {
        static int frames = 0;
        if (IsNativeSceneRendering() && HasNativeGameplayCamera() && g_engineFov.load() > 0.0f && ++frames >= 900) {
            if (!g_imageBase) {
                const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                g_imageBase = base;
                g_imageEnd = base + reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
            }
            g_fovScanState.store(1);
            TraceRuntime("engine FOV scan: searching process memory for " + std::to_string(g_engineFov.load()) + " rad on a background thread");
            std::thread(ScanFov, g_engineFov.load()).detach();
        }
    }
    if (!g_enabled || g_state.load() != 0 || g_attempts >= 3) return;
    // Real gameplay only: live eye rendering for ~10 s and the gameplay
    // camera present (the front-end camera is an identity record).
    static int sceneFrames = 0;
    if (!IsNativeSceneRendering() || !HasNativeGameplayCamera()) { g_stableFrames = 0; sceneFrames = 0; return; }
    if (++sceneFrames < 900) return;
    float rows[9]{};
    if (!GetEngineCameraBasis(rows)) return;
    // Reject axis-aligned (identity-like) rotations: they match countless
    // unrelated matrices (live 16:57 run: identity -> 200 useless hits).
    // (Only the right axis is checked: a level camera's up row is ~(0,1,0).)
    // Only an exact identity-like record is rejected: a real camera can face
    // almost along a world axis (live 17:14 run: right axis 0.999 was
    // wrongly rejected every frame).
    const bool trivial = std::abs(rows[0]) > 0.99999f && std::abs(rows[4]) > 0.99999f;
    if (trivial) { g_stableFrames = 0; return; }
    if (std::memcmp(rows, g_lastRows, sizeof(rows)) == 0) ++g_stableFrames;
    else { g_stableFrames = 0; std::memcpy(g_lastRows, rows, sizeof(rows)); }
    if (g_stableFrames < 90) return;
    ++g_attempts;
    g_stableFrames = 0;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_imageBase = base;
    g_imageEnd = base + nt->OptionalHeader.SizeOfImage;
    g_state.store(1);
    TraceRuntime("camera source scan: engine rotation stable for 90 frames; scanning process memory on a background thread");
    std::thread(Scan, std::vector<float>(rows, rows + 9)).detach();
}

// ---- Visibility-camera A/B (F5) -------------------------------------------
namespace {
std::mutex g_overrideMutex;
bool g_overrideEnabled = false;
int g_group = 0;
float g_headForward[3]{};
bool g_haveHeadForward = false;
std::atomic<bool> g_handSteering = false;
std::uint64_t g_overrideWrites = 0, g_overrideSkips = 0;
// Automatic A/B: phases of 600 Presents (~6.7 s): 180 to settle, 420 measured.
// 0 = head (baseline), 1 = aim +60 deg, 2..7 = aim +60 deg with group 1..6,
// 8 = head again. Draw calls per frame are the visibility proxy.
bool g_autoTest = false;
int g_autoPhase = -1;
std::uint64_t g_autoPhaseFrame = 0;
std::uint64_t g_autoDrawStart = 0;
std::atomic<float> g_autoYaw = 0.0f;
constexpr const char* kGroupNames[] = {"off", "static 0x2ED7704 trio", "static 0x2F34190/0x2F38190", "CGIPass+0x8c8",
                                       "CAISystemJC3+0x1a0", "unattributed heap copies", "CSoundSystem listener"};

std::vector<std::uintptr_t> GroupTargets(int group) {
    std::vector<std::uintptr_t> targets;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (group == 1) for (int k = 0; k != 3; ++k) targets.push_back(base + 0x2ED7704 + 0x40 * k);
    if (group == 2) { targets.push_back(base + 0x2F34190); targets.push_back(base + 0x2F38190); }
    if (group >= 3) {
        std::lock_guard lock(g_hitsMutex);
        for (const auto& hit : g_published) {
            const bool inImage = hit.address >= g_imageBase && hit.address < g_imageEnd;
            const bool match =
                (group == 3 && hit.className.find("CGIPass") != std::string::npos) ||
                (group == 4 && hit.className.find("CAISystemJC3") != std::string::npos) ||
                (group == 5 && hit.className.empty() && !inImage) ||
                (group == 6 && hit.className.find("CSoundSystem") != std::string::npos);
            if (!match) continue;
            // Heap objects: still the same object (vtable unchanged)?
            std::uintptr_t vtable = 0;
            if (hit.object && (!SafeCopy(&vtable, reinterpret_cast<const void*>(hit.object), 8) || vtable != hit.vtable)) continue;
            targets.push_back(hit.address);
        }
    }
    return targets;
}

// Engine-convention rows (right, up, back) for a camera looking along the
// head forward; handedness taken from the live engine basis.
bool HeadRows(float rows[9]) {
    float engine[9]{};
    if (!g_haveHeadForward || !GetEngineCameraBasis(engine)) return false;
    float f[3]{g_headForward[0], g_headForward[1], g_headForward[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 0.5f) return false;
    for (float& v : f) v /= fl;
    float r[3]{-f[2], 0.0f, f[0]};  // horizontal, perpendicular to f
    const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    if (rl < 0.05f) return false;
    r[0] /= rl;
    r[2] /= rl;
    // Match the engine's handedness: its right row vs the same construction
    // from its own forward.
    const float ef[3]{-engine[6], -engine[7], -engine[8]};
    const float engineCross[3]{-ef[2], 0.0f, ef[0]};
    if (engineCross[0] * engine[0] + engineCross[2] * engine[2] < 0.0f) { r[0] = -r[0]; r[2] = -r[2]; }
    const float b[3]{-f[0], -f[1], -f[2]};
    float u[3]{b[1] * r[2] - b[2] * r[1], b[2] * r[0] - b[0] * r[2], b[0] * r[1] - b[1] * r[0]};
    if (u[1] * engine[4] < 0.0f) for (float& v : u) v = -v;
    const float out[9]{r[0], r[1], r[2], u[0], u[1], u[2], b[0], b[1], b[2]};
    std::memcpy(rows, out, sizeof(out));
    return true;
}

bool LooksLikeRotationRows(const float* matrix) {
    for (int row = 0; row != 3; ++row) {
        const float* v = matrix + row * 4;
        const float length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (!(length > 0.96f && length < 1.04f)) return false;
    }
    return true;
}

void ApplyGroup() {
    if (!g_overrideEnabled || g_group == 0 || !g_handSteering.load()) return;
    float rows[9]{};
    if (!HeadRows(rows)) return;
    for (const std::uintptr_t address : GroupTargets(g_group)) {
        float matrix[12]{};
        if (!SafeCopy(matrix, reinterpret_cast<const void*>(address), sizeof(matrix)) || !LooksLikeRotationRows(matrix)) { ++g_overrideSkips; continue; }
        for (int row = 0; row != 3; ++row)
            for (int c = 0; c != 3; ++c) matrix[row * 4 + c] = rows[row * 3 + c];
        DWORD oldProtect = 0;
        const bool inImage = address >= g_imageBase && address < g_imageEnd;
        if (inImage && !VirtualProtect(reinterpret_cast<void*>(address), sizeof(matrix), PAGE_READWRITE, &oldProtect)) { ++g_overrideSkips; continue; }
        const bool written = SafeCopy(reinterpret_cast<void*>(address), matrix, sizeof(matrix));
        if (inImage) VirtualProtect(reinterpret_cast<void*>(address), sizeof(matrix), oldProtect, &oldProtect);
        if (written) ++g_overrideWrites; else ++g_overrideSkips;
    }
}
}

float CullAutoTestAimYaw() { return g_autoYaw.load(); }

void ConfigureCullOverrideTest(bool enabled, bool automatic) {
    g_autoTest = enabled && automatic;
    g_overrideEnabled = enabled;
    if (enabled) TraceRuntime("visibility-camera A/B enabled: F5 cycles candidate groups (active only while a hand steers the aim)");
}

void SetCullHeadForward(const float forward[3], bool handSteering) {
    std::lock_guard lock(g_overrideMutex);
    std::memcpy(g_headForward, forward, sizeof(g_headForward));
    g_haveHeadForward = true;
    g_handSteering.store(handSteering);
}

void CullOverridePresent() {
    if (!g_overrideEnabled) return;
    if (!g_imageBase) {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_imageBase = base;
        g_imageEnd = base + nt->OptionalHeader.SizeOfImage;
    }
    static bool f5WasDown = false;
    const bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    std::lock_guard lock(g_overrideMutex);
    if (g_autoTest && g_autoPhase < 9) {
        // Start once the source scan has published its hits, in gameplay.
        if (g_autoPhase < 0) {
            if (g_state.load() != 2 || !IsNativeSceneRendering()) return;
            g_autoPhase = 0;
            g_autoPhaseFrame = 0;
            TraceRuntime("visibility-camera auto A/B: starting (9 phases x ~6.7 s; keep the head still)");
        }
        const int phase = g_autoPhase;
        g_group = (phase >= 2 && phase <= 7) ? phase - 1 : 0;
        g_autoYaw.store((phase >= 1 && phase <= 7) ? 1.047f : 0.0f);
        ++g_autoPhaseFrame;
        if (g_autoPhaseFrame == 180) g_autoDrawStart = GetObservedDrawCount();
        if (g_autoPhaseFrame == 600) {
            const double perFrame = static_cast<double>(GetObservedDrawCount() - g_autoDrawStart) / 420.0;
            TraceRuntime("visibility-camera auto A/B phase " + std::to_string(phase) + " aim=" + (g_autoYaw.load() != 0.0f ? "+60deg" : "head") +
                " group=" + std::to_string(g_group) + " (" + kGroupNames[g_group] + ") targets=" + std::to_string(GroupTargets(g_group).size()) +
                " draws/frame=" + std::to_string(perFrame) + " writes=" + std::to_string(g_overrideWrites) + " skips=" + std::to_string(g_overrideSkips));
            ++g_autoPhase;
            g_autoPhaseFrame = 0;
            if (g_autoPhase == 9) {
                g_group = 0;
                g_autoYaw.store(0.0f);
                TraceRuntime("visibility-camera auto A/B: finished");
            }
        }
        ApplyGroup();
        return;
    }
    if (f5Down && !f5WasDown) {
        g_group = (g_group + 1) % static_cast<int>(std::size(kGroupNames));
        TraceRuntime("F5: visibility-camera A/B group " + std::to_string(g_group) + " = " + kGroupNames[g_group] +
            " targets=" + std::to_string(GroupTargets(g_group).size()));
    }
    f5WasDown = f5Down;
    ApplyGroup();
    static std::uint64_t frames = 0;
    if (++frames % 900 == 0 && g_group)
        TraceRuntime("visibility-camera A/B: group " + std::to_string(g_group) + " writes=" + std::to_string(g_overrideWrites) +
            " skips=" + std::to_string(g_overrideSkips) + " steering=" + (g_handSteering.load() ? "1" : "0"));
}

void CullOverrideAtUpload() {
    if (!g_overrideEnabled) return;
    std::lock_guard lock(g_overrideMutex);
    ApplyGroup();
}
}
