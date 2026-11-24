#pragma once
#include <Windows.h>
#include <string>

namespace jc3vr {
// Loads the system loader when it is registered on the default DLL path, then
// falls back to a loader located beside the active OpenXR runtime registration.
// This does not alter the runtime selection or registry.
HMODULE LoadActiveOpenXrLoader(std::string& detail);
}
