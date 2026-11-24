#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace jc3vr {
struct BuildFingerprint {
    std::uintmax_t fileSize = 0;
    std::uint32_t peTimestamp = 0;
    std::uint32_t imageSize = 0;
    std::string sha256;
    std::string ToString() const;
};
std::optional<BuildFingerprint> FingerprintExecutable(const std::filesystem::path& executable, std::string& error);
bool FileMatchesSha256(const std::filesystem::path& path, std::size_t size, const char* expected);
}
