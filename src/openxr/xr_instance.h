#pragma once
#include <string>

namespace jc3vr {
struct OpenXrLoaderProbe { bool available = false; std::string detail; };
OpenXrLoaderProbe ProbeOpenXrLoader();
}
