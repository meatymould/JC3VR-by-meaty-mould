#pragma once
#include <array>

namespace jc3vr {
struct MatrixDiagnostics { float determinant = 0; float basisLengths[3]{}; float orthogonalityError = 0; bool resemblesProjection = false; };
MatrixDiagnostics DiagnoseMatrix(const std::array<float, 16>& matrix);
}
