#pragma once
#include <string_view>

namespace jc3vr {
void InitializeLogging();
void Log(std::string_view message);
// Process-safe direct trace used while validating bootstrap and live native
// rendering.  It deliberately bypasses the normal rotating log path.
void TraceRuntime(std::string_view message);
}
