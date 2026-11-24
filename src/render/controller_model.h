#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace jc3vr {
// One vertex of a render-model triangle list, in the model's own space.
struct ControllerModelVertex {
    float position[3];
    float normal[3];
    float uv[2];  // D3D convention (v flipped from OBJ)
};

// A SteamVR render-model component's local frame ("component_local").
struct RenderModelComponent {
    bool found = false;
    float origin[3]{};
    float rotateXyzDegrees[3]{};
};

// The Meta Quest 3 (Touch Plus) controller as shipped with the local
// SteamVR install (resources/rendermodels/oculus_quest_plus_controller_*).
// Loaded at runtime from the user's install; nothing is redistributed.
struct ControllerModelAsset {
    std::vector<ControllerModelVertex> vertices;
    std::vector<std::uint8_t> rgba;
    unsigned width = 0, height = 0;
    RenderModelComponent grip;  // "openxr_grip": the OpenXR grip pose in model space
    RenderModelComponent aim;   // "openxr_aim"
};

// Triangulated (fan) OBJ faces with position/uv/normal; false if none.
bool ParseObjTriangles(const std::string& text, std::vector<ControllerModelVertex>& out);
RenderModelComponent ParseRenderModelComponent(const std::string& json, const std::string& name);
std::wstring FindSteamVrRoot();
bool LoadQuestPlusControllerModel(bool leftHand, ControllerModelAsset& out, std::string& error);
bool LoadRgbaPng(const std::wstring& path, std::vector<std::uint8_t>& rgba, unsigned& width, unsigned& height);
// Local-only cache produced by prepare_c4_asset.py, never a procedural proxy.
bool LoadInstalledChargeAsset(const std::wstring& directory, ControllerModelAsset& out, std::string& error);
}
