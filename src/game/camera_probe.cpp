#include "game/camera_probe.h"
#include <cmath>

namespace jc3vr {
MatrixDiagnostics DiagnoseMatrix(const std::array<float, 16>& m) {
    const auto dot = [](float ax, float ay, float az, float bx, float by, float bz) { return ax * bx + ay * by + az * bz; };
    const float a = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    MatrixDiagnostics result{};
    result.determinant = a;
    for (int row = 0; row != 3; ++row) result.basisLengths[row] = std::sqrt(dot(m[row * 4], m[row * 4 + 1], m[row * 4 + 2], m[row * 4], m[row * 4 + 1], m[row * 4 + 2]));
    result.orthogonalityError = std::abs(dot(m[0], m[1], m[2], m[4], m[5], m[6])) + std::abs(dot(m[0], m[1], m[2], m[8], m[9], m[10])) + std::abs(dot(m[4], m[5], m[6], m[8], m[9], m[10]));
    result.resemblesProjection = std::abs(m[15]) < 0.001f && std::abs(m[11]) > 0.001f;
    return result;
}
}
