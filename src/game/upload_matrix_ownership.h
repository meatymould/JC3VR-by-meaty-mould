#pragma once
#include <array>
#include <cstddef>
#include <cmath>

namespace jc3vr {
// A 4 KB upload has at most 64 non-overlapping rigid 4x4 matrices.
// Once a rider is placed/hidden, a subsequent 3x4 skinning sweep must not
// reinterpret any of its bytes as a bone (including shifted windows).
class UploadMatrixOwnership {
public:
    void Protect(std::size_t offset, std::size_t bytes = 64) {
        if (count_ < spans_.size()) spans_[count_++] = {offset, bytes};
    }
    bool Overlaps(std::size_t offset, std::size_t bytes = 48) const {
        for (std::size_t i = 0; i < count_; ++i) {
            const auto [start, length] = spans_[i];
            if (offset >= start ? offset - start < length : start - offset < bytes) return true;
        }
        return false;
    }
private:
    struct Span { std::size_t offset, size; };
    std::array<Span, 86> spans_{}; // 4096 / 48 unique matched matrices, including riders.
    std::size_t count_ = 0;
};

inline bool FiniteMatrix(const float* matrix, std::size_t floats) {
    for (std::size_t i = 0; i < floats; ++i)
        if (!std::isfinite(matrix[i])) return false;
    return true;
}
}
