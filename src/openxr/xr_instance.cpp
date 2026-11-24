#include "openxr/xr_instance.h"
#include "openxr/xr_loader.h"

namespace jc3vr {
OpenXrLoaderProbe ProbeOpenXrLoader() {
    std::string location;
    HMODULE loader = LoadActiveOpenXrLoader(location);
    if (!loader) return {false, "not available: " + location};
    const bool hasInstanceEntry = GetProcAddress(loader, "xrCreateInstance") != nullptr;
    if (GetModuleHandleW(L"openxr_loader.dll") != loader) FreeLibrary(loader);
    return hasInstanceEntry ? OpenXrLoaderProbe{true, "loader found via " + location + "; xrCreateInstance export available"} : OpenXrLoaderProbe{false, "loader found via " + location + " but xrCreateInstance export is missing"};
}
}
