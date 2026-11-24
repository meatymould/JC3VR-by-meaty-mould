#pragma once
#include <cstdint>
namespace jc3vr {
inline bool IsResourceCrash(std::uint32_t code,std::uintptr_t image,std::uintptr_t ip) {
    return code==0xC0000005u && image && ip>=image && ip-image==0xA53C0;
}
void InstallResourceCrashTrace();
}
