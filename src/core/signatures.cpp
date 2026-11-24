#include "core/signatures.h"
#include <algorithm>

namespace jc3vr {
bool ValidateGuardedAddress(const BuildFingerprint& actual, const GuardedAddress& candidate, std::span<const std::uint8_t> bytes, std::string& reason) {
    if (candidate.discoveredOnBuild != actual.sha256) { reason = "build fingerprint differs; refusing stale address"; return false; }
    if (bytes.size() < candidate.expectedBytes.size() || !std::equal(candidate.expectedBytes.begin(), candidate.expectedBytes.end(), bytes.begin())) { reason = "expected instruction bytes do not match"; return false; }
    return true;
}
}
