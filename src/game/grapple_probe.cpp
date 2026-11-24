#include "game/grapple_probe.h"
#include "core/logging.h"
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace jc3vr {
namespace {
struct ProbedVtable {
    const char* label;
    std::uintptr_t rva;
    const char* rttiName;  // type descriptor name expected via vtable[-1]
};
// Fingerprinted JustCause3.exe RTTI (tools/rtti_classes.py, 2026-09-29).
constexpr ProbedVtable kVtables[] = {
    {"hook", 0x023e8c98, ".?AVCGrapplingHook@@"},
    {"wire", 0x023e7938, ".?AVCGrapplingHookWire@@"},
    {"castfilter", 0x023e1b98, ".?AVCGrapplingHookWireEndCastFilter@CWireEnd@@"},
};
constexpr std::size_t kMaxSlots = 96;

// Per-slot data the stub writes: call count and the last four register args.
constexpr std::size_t kStackQwords = 24;
struct SlotRecord {
    std::uint64_t count;
    std::uint64_t rcx, rdx, r8, r9;
    std::uint64_t original;
    std::uint64_t stack[kStackQwords];  // [rsp + 8*i] at entry (i=0: return address)
};
struct ProbedSlot {
    std::size_t table = 0;
    std::size_t index = 0;
    SlotRecord* record = nullptr;
    std::uint64_t lastCount = 0;
};

bool g_enabled = false;
std::uintptr_t g_base = 0, g_textBegin = 0, g_textEnd = 0;
bool g_installed = false;
std::vector<ProbedSlot> g_slots;
std::uint64_t g_ticks = 0;
bool g_buttonWasHeld = false;
std::uint32_t g_buttonPresses = 0;

bool Readable(const void* address, std::size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return reinterpret_cast<std::uintptr_t>(address) + size <= end;
}

bool TextRange(std::uintptr_t base, std::uintptr_t& begin, std::uintptr_t& end) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i != nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5) == 0) {
            begin = base + section->VirtualAddress;
            end = begin + section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

std::string Floats(std::uint64_t pointer) {
    const auto* values = reinterpret_cast<const float*>(pointer);
    if (!pointer || !Readable(values, sizeof(float) * 4)) return "-";
    std::ostringstream out;
    out << '(' << values[0] << ',' << values[1] << ',' << values[2] << ',' << values[3] << ')';
    return out.str();
}
}

void ConfigureGrappleProbe(bool enabled) {
    g_enabled = enabled;
    if (enabled) TraceRuntime("grapple probe enabled (read-only call counting on grapple vtables)");
}

bool InstallGrappleProbe() {
    if (!g_enabled || g_installed) return g_installed;
    g_installed = true;  // one attempt
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t textBegin = 0, textEnd = 0;
    if (!base || !TextRange(base, textBegin, textEnd)) return false;
    g_base = base;
    g_textBegin = textBegin;
    g_textEnd = textEnd;
    // Stub: mov [rip+a],rcx; mov [rip+b],rdx; mov [rip+c],r8; mov [rip+d],r9;
    //       lock inc qword [rip+e]; jmp qword [rip+f]   (flags only; the
    //       callee does not rely on flags at entry).
    constexpr std::size_t kStubSize = 7 * 4 + kStackQwords * (8 + 7) + 8 + 6;
    const std::size_t totalSlots = std::size(kVtables) * kMaxSlots;
    auto* block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, totalSlots * (kStubSize + sizeof(SlotRecord) + 16), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!block) return false;
    std::uint8_t* stubs = block;
    auto* records = reinterpret_cast<SlotRecord*>(block + ((totalSlots * kStubSize + 15) & ~std::size_t(15)));
    std::size_t used = 0;
    for (std::size_t table = 0; table != std::size(kVtables); ++table) {
        auto** vtable = reinterpret_cast<std::uintptr_t**>(base + kVtables[table].rva);
        // RTTI check: vtable[-1] -> complete object locator -> type descriptor name.
        const auto* locatorPointer = reinterpret_cast<const std::uintptr_t*>(vtable) - 1;
        bool named = false;
        if (Readable(locatorPointer, 8)) {
            const auto* locator = reinterpret_cast<const std::uint32_t*>(*locatorPointer);
            if (Readable(locator, 24)) {
                const char* name = reinterpret_cast<const char*>(base + locator[3] + 16);
                named = Readable(name, 64) && std::strcmp(name, kVtables[table].rttiName) == 0;
            }
        }
        if (!named) {
            TraceRuntime(std::string("grapple probe: RTTI mismatch for ") + kVtables[table].label + "; not probed");
            continue;
        }
        std::size_t count = 0;
        while (count < kMaxSlots) {
            const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(vtable[count]);
            if (target < textBegin || target >= textEnd) break;
            ++count;
        }
        DWORD oldProtect = 0;
        if (!count || !VirtualProtect(vtable, count * sizeof(void*), PAGE_READWRITE, &oldProtect)) continue;
        for (std::size_t slot = 0; slot != count; ++slot, ++used) {
            SlotRecord* record = records + used;
            *record = {};
            record->original = reinterpret_cast<std::uint64_t>(vtable[slot]);
            std::uint8_t* stub = stubs + used * kStubSize;
            std::uint8_t* p = stub;
            const auto emitStore = [&](std::uint8_t rex, std::uint8_t modrm, void* field) {
                *p++ = rex; *p++ = 0x89; *p++ = modrm;
                const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::uint8_t*>(field) - (p + 4));
                std::memcpy(p, &displacement, 4); p += 4;
            };
            emitStore(0x48, 0x0D, &record->rcx);  // mov [rip+d], rcx
            emitStore(0x48, 0x15, &record->rdx);  // mov [rip+d], rdx
            emitStore(0x4C, 0x05, &record->r8);   // mov [rip+d], r8
            emitStore(0x4C, 0x0D, &record->r9);   // mov [rip+d], r9
            // Stack snapshot via rax (volatile, unused for arguments on x64
            // Windows): mov rax,[rsp+8*i]; mov [rip+d],rax.
            for (std::size_t q = 0; q != kStackQwords; ++q) {
                *p++ = 0x48; *p++ = 0x8B; *p++ = 0x84; *p++ = 0x24;  // mov rax,[rsp+disp32]
                const auto offset = static_cast<std::int32_t>(q * 8);
                std::memcpy(p, &offset, 4); p += 4;
                emitStore(0x48, 0x05, &record->stack[q]);                                                 // mov [rip+d], rax
            }
            *p++ = 0xF0; *p++ = 0x48; *p++ = 0xFF; *p++ = 0x05;  // lock inc qword [rip+d]
            {
                const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::uint8_t*>(&record->count) - (p + 4));
                std::memcpy(p, &displacement, 4); p += 4;
            }
            *p++ = 0xFF; *p++ = 0x25;  // jmp qword [rip+d]
            {
                const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::uint8_t*>(&record->original) - (p + 4));
                std::memcpy(p, &displacement, 4); p += 4;
            }
            g_slots.push_back({table, slot, record, 0});
        }
        FlushInstructionCache(GetCurrentProcess(), stubs, totalSlots * kStubSize);
        for (std::size_t slot = 0; slot != count; ++slot)
            vtable[slot] = reinterpret_cast<std::uintptr_t*>(stubs + (used - count + slot) * kStubSize);
        VirtualProtect(vtable, count * sizeof(void*), oldProtect, &oldProtect);
        TraceRuntime(std::string("grapple probe: ") + kVtables[table].label + " vtable RVA 0x" +
            [&] { char b[16]; std::snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(kVtables[table].rva)); return std::string(b); }() +
            " slots=" + std::to_string(count) + " redirected through counting stubs");
    }
    return !g_slots.empty();
}

void GrappleProbeTick(bool grappleButtonHeld) {
    if (!g_enabled) return;
    if (!g_installed) InstallGrappleProbe();
    if (grappleButtonHeld && !g_buttonWasHeld) ++g_buttonPresses;
    g_buttonWasHeld = grappleButtonHeld;
    if (g_slots.empty() || ++g_ticks % 90 != 0) return;
    std::ostringstream line, rare;
    line << "grapple probe t=" << g_ticks / 90 << " grapple_presses=" << g_buttonPresses << " held=" << (grappleButtonHeld ? 1 : 0) << " deltas:";
    bool any = false;
    int rareLines = 0;
    for (auto& slot : g_slots) {
        const std::uint64_t count = slot.record->count;
        const std::uint64_t delta = count - slot.lastCount;
        slot.lastCount = count;
        if (!delta) continue;
        any = true;
        line << ' ' << kVtables[slot.table].label << '[' << slot.index << "]+" << delta;
        // Rarely called slots: dump their last arguments (candidate fire /
        // target functions carry a direction or point).
        if (delta <= 6 && rareLines < 20) {
            ++rareLines;
            rare << "\n  grapple probe args " << kVtables[slot.table].label << '[' << slot.index << "] rcx=0x" << std::hex << slot.record->rcx
                 << " rdx=0x" << slot.record->rdx << " r8=0x" << slot.record->r8 << " r9=0x" << slot.record->r9 << std::dec
                 << " *rdx=" << Floats(slot.record->rdx) << " *r8=" << Floats(slot.record->r8) << " *r9=" << Floats(slot.record->r9) << " code_on_stack=[";
            for (std::size_t q = 0; q != kStackQwords; ++q) {
                const std::uint64_t value = slot.record->stack[q];
                if (value >= g_textBegin && value < g_textEnd) rare << ' ' << q << ":0x" << std::hex << (value - g_base) << std::dec;
            }
            rare << " ]";
        }
    }
    if (any) TraceRuntime(line.str() + rare.str());
}
}
