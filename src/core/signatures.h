#pragma once
#include "core/build_fingerprint.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jc3vr {
struct GuardedAddress {
    std::string feature;
    std::string discoveredOnBuild;
    std::uintptr_t rva = 0;
    std::vector<std::uint8_t> expectedBytes;
};
bool ValidateGuardedAddress(const BuildFingerprint& actual, const GuardedAddress& candidate, std::span<const std::uint8_t> bytes, std::string& reason);
}
