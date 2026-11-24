#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace jc3vr {
// Keep complete getter signatures, including the state bit and RET. Fail
// closed on oversized requests or unreadable memory, without truncation.
template<class Reader>
bool MatchNativeSignature(std::uintptr_t address, const std::uint8_t* expected,
                          std::size_t size, Reader read) {
    std::uint8_t actual[64]{};
    return address && expected && size && size<=sizeof(actual) &&
        read(actual,reinterpret_cast<const void*>(address),size) &&
        std::memcmp(actual,expected,size)==0;
}
}
