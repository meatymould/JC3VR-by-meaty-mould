#include "core/build_fingerprint.h"
#include <Windows.h>
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace jc3vr {
namespace {
std::string HashBytes(std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength = 0, resultLength = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<UCHAR> object(objectLength), digest(32);
    const bool ok = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream hex;
    for (const auto byte : digest) hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return hex.str();
}
}

bool FileMatchesSha256(const std::filesystem::path& path, std::size_t size, const char* expected) {
    if (!size || size > 8 * 1024 * 1024) return false;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (input.tellg() != static_cast<std::streamoff>(size)) return false;
    input.seekg(0);
    std::vector<unsigned char> bytes(size);
    return input.read(reinterpret_cast<char*>(bytes.data()), size) && HashBytes(bytes) == expected;
}

std::string BuildFingerprint::ToString() const {
    std::ostringstream stream;
    stream << "size=" << fileSize << " timestamp=0x" << std::hex << peTimestamp << " image_size=0x" << imageSize << " sha256=" << sha256;
    return stream.str();
}

std::optional<BuildFingerprint> FingerprintExecutable(const std::filesystem::path& executable, std::string& error) {
    std::ifstream input(executable, std::ios::binary);
    if (!input) { error = "unable to open executable"; return std::nullopt; }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < static_cast<std::streamoff>(sizeof(IMAGE_DOS_HEADER))) { error = "file is too small for a PE image"; return std::nullopt; }
    input.seekg(0);
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) { error = "failed reading executable"; return std::nullopt; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > bytes.size()) { error = "invalid PE headers"; return std::nullopt; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { error = "not a 64-bit PE executable"; return std::nullopt; }

    const auto sha256 = HashBytes(bytes);
    if (sha256.empty()) { error = "SHA-256 calculation failed"; return std::nullopt; }
    return BuildFingerprint{static_cast<std::uintmax_t>(size), nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, sha256};
}
}
