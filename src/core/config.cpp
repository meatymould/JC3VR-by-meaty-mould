#include "core/config.h"
#include <Windows.h>

namespace jc3vr {
namespace {
bool ReadBool(const std::filesystem::path& path, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(L"JC3VR", key, fallback ? 1 : 0, path.c_str()) != 0;
}
float ReadFloat(const std::filesystem::path& path, const wchar_t* key, float fallback) {
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"JC3VR", key, L"", value, 64, path.c_str());
    return value[0] ? static_cast<float>(wcstod(value, nullptr)) : fallback;
}
std::wstring ReadString(const std::filesystem::path& path, const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"JC3VR", key, fallback, value, 64, path.c_str());
    return value;
}
int ReadVirtualKey(const std::filesystem::path& path, const wchar_t* key, int fallback) {
    const auto value = ReadString(path, key, L"");
    if (value.empty()) return fallback;
    if (value == L"HOME") return VK_HOME;
    if (value == L"END") return VK_END;
    if (value.size() == 1) return VkKeyScanW(value[0]) & 0xff;
    if (value.size() >= 2 && (value[0] == L'F' || value[0] == L'f')) {
        const int number = _wtoi(value.c_str() + 1);
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    return fallback;
}
}
Config LoadConfig(const std::filesystem::path& path) {
    Config result;
    result.enabled = ReadBool(path, L"enabled", result.enabled);
    result.allowUnknownBuild = ReadBool(path, L"allow_unknown_build", false);
    result.debugLogging = ReadBool(path, L"debug_logging", result.debugLogging);
    result.creatorMirrorEye = ReadString(path, L"creator_mirror_eye", L"off");
    result.headTracking = ReadBool(path, L"head_tracking", result.headTracking);
    result.directHeadRotation = ReadString(path, L"head_look_mode", L"mouse") == L"direct_camera";
    result.positionalTracking = ReadBool(path, L"positional_tracking", result.positionalTracking);
    result.worldScale = ReadFloat(path, L"world_scale", result.worldScale);
    result.yawScale = ReadFloat(path, L"head_yaw_scale", result.yawScale);
    result.pitchScale = ReadFloat(path, L"head_pitch_scale", result.pitchScale);
    result.cameraShakeScale = ReadFloat(path, L"camera_shake_scale", result.cameraShakeScale);
    result.showBody = ReadBool(path, L"show_body", result.showBody);
    result.showHeadMesh = ReadBool(path, L"show_head_mesh", result.showHeadMesh);
    result.nearClip = ReadFloat(path, L"near_clip", result.nearClip);
    result.geo11SwapEyes = ReadBool(path, L"geo11_swap_eyes", result.geo11SwapEyes);
    result.geo11LogExtractionEveryFrame = ReadBool(path, L"geo11_log_extraction_every_frame", result.geo11LogExtractionEveryFrame);
    result.jc3WorldUnitsPerMeter = ReadFloat(path, L"jc3_world_units_per_meter", result.jc3WorldUnitsPerMeter);
    result.nativeCameraProbeEnabled = ReadBool(path, L"native_camera_probe_enabled", result.nativeCameraProbeEnabled);
    result.sixDof = ReadBool(path, L"six_dof", result.positionalTracking);
    result.sixDofScale = ReadFloat(path, L"six_dof_scale", result.worldScale);
    result.sixDofLimitMeters = ReadFloat(path, L"six_dof_limit_m", result.sixDofLimitMeters);
    result.sixDofVerticalLimitMeters = ReadFloat(path,L"six_dof_vertical_limit_m",1.8f);
    result.recenterKey = ReadVirtualKey(path, L"recenter_key", result.recenterKey);
    result.motionControllers = ReadBool(path, L"motion_controllers", result.motionControllers);
    result.handDebugPreview = ReadBool(path, L"hand_debug_preview", false);
    result.debugOverlay = ReadBool(path, L"debug_overlay", result.debugOverlay);
    result.shadowProbe = ReadBool(path, L"shadow_probe", result.shadowProbe);
    result.nativeEyeCapture = ReadBool(path, L"native_eye_capture", false);
    if (const auto source = ReadString(path, L"stereo_source", L"proper_sbs"); source == L"left_to_both") result.geo11StereoSource = Geo11StereoSource::LeftToBoth;
    else if (source == L"right_to_both") result.geo11StereoSource = Geo11StereoSource::RightToBoth;
    if (const auto camera = ReadString(path, L"camera_mode", L"current_working"); camera == L"first_person") result.cameraMode = CameraMode::FirstPerson;
    if (const auto stereo = ReadString(path, L"stereo_mode", L"mono_debug"); stereo == L"geo11") result.stereoMode = StereoMode::Geo11;
    else if (stereo == L"external_sbs") result.stereoMode = StereoMode::ExternalSbs;
    else if (stereo == L"native_full") result.stereoMode = StereoMode::NativeFull;
    else if (stereo == L"native_alternate") result.stereoMode = StereoMode::NativeAlternate;
    if (const auto nativeMode = ReadString(path, L"native_stereo_rig_mode", L"center"); nativeMode == L"left_to_both") result.nativeStereoRigMode = NativeStereoRigMode::LeftToBoth;
    else if (nativeMode == L"right_to_both") result.nativeStereoRigMode = NativeStereoRigMode::RightToBoth;
    else if (nativeMode == L"native_stereo") result.nativeStereoRigMode = NativeStereoRigMode::NativeStereo;
    else if (nativeMode == L"swap_eyes") result.nativeStereoRigMode = NativeStereoRigMode::SwapEyes;
    result.nativeProjectionOverride = ReadBool(path, L"native_projection_override", false);
    result.forceNativeProjectionBuffer = ReadBool(path, L"force_native_projection_buffer", false);
    result.syncNativeViewProjection = ReadBool(path, L"sync_native_view_projection", false);
    result.syncNativeProjectionToXr = ReadBool(path, L"sync_native_projection_to_xr", false);
    result.stereoCoherenceProbe = ReadBool(path, L"stereo_coherence_probe", false);
    result.syncPairedCameraAtUpload = ReadBool(path, L"sync_paired_camera_at_upload", false);
    result.cameraTimelineProbe = ReadBool(path, L"camera_timeline_probe", false);
    result.syncNativeCullingCamera = ReadBool(path, L"sync_native_culling_camera", false);
    result.cullingSyncParts = static_cast<int>(ReadFloat(path, L"culling_sync_parts", 7.0f));
    result.debugHeadYawDegrees = ReadFloat(path, L"debug_head_yaw_deg", 0.0f);
    result.debugHeadYawWobbleDegrees = ReadFloat(path, L"debug_head_yaw_wobble_deg", 0.0f);
    result.renderWidth = static_cast<int>(ReadFloat(path, L"render_width", 0.0f));
    result.vrGamepad = ReadBool(path, L"vr_gamepad", false);
    result.controllerHaptics = ReadBool(path,L"controller_haptics",true);
    result.hapticIntensity = ReadFloat(path,L"haptic_intensity",0.7f);
    result.handAim = ReadBool(path, L"hand_aim", false);
    result.engineFollowHead = ReadBool(path, L"engine_follow_head", false);
    result.pairedEyeSubmission = ReadBool(path, L"paired_eye_submission", false);
    result.aimFollowsRightHand = ReadBool(path, L"aim_follows_right_hand", false);
    result.controllerModel = ReadBool(path, L"controller_model", true);
    result.trackedHands = ReadBool(path, L"tracked_hands", false);
    result.aimLines = ReadBool(path, L"aim_lines", false);
    result.grappleProbe = ReadBool(path, L"grapple_probe", false);
    result.cameraSourceScan = ReadBool(path, L"camera_source_scan", false);
    result.cullOverrideTest = ReadBool(path, L"cull_override_test", false);
    result.cullOverrideAuto = ReadBool(path, L"cull_override_auto", false);
    result.lodProjectionScale = ReadFloat(path, L"lod_projection_scale", 1.0f);
    result.lodScaleTest = ReadBool(path, L"lod_scale_test", false);
    result.cullingMode = static_cast<int>(ReadFloat(path, L"culling_mode", 2.0f));
    result.cullingModeTest = ReadBool(path, L"culling_mode_test", false);
    result.fovSourceScan = ReadBool(path, L"fov_source_scan", false);
    result.skeletonProbe = ReadBool(path, L"skeleton_probe", false);
    result.vrBody = ReadBool(path, L"vr_body", false);
    result.vrBodyDebugPose = ReadBool(path, L"vr_body_debug_pose", false);
    result.vrBodyHideHead = ReadBool(path, L"vr_body_hide_head", false);
    result.vrBodyCarry = ReadBool(path, L"vr_body_carry", true);
    result.vrBodyHideAll = ReadBool(path, L"vr_body_hide_all", false);
    result.ricoNativeHands = ReadBool(path, L"rico_native_hands", false);
    result.ricoPocketHolster = ReadBool(path, L"rico_pocket_holster", false);
    result.nativeGrabProbe = ReadBool(path, L"native_grab_probe", false);
    result.nativeGrabReachMeters = ReadFloat(path, L"native_grab_reach_m", 0.15f);
    if (result.ricoNativeHands) {
        constexpr const wchar_t* sides[]{L"left", L"right"};
        constexpr const wchar_t* axes[]{L"x", L"y", L"z"};
        for (int side = 0; side != 2; ++side) {
            const std::wstring prefix = std::wstring(L"rico_hand_") + sides[side];
            for (int axis = 0; axis != 3; ++axis) {
                const float offset = ReadFloat(path, (prefix + L"_offset_" + axes[axis]).c_str(), 0);
                const float degrees = ReadFloat(path, (prefix + L"_degrees_" + axes[axis]).c_str(), 0);
                result.ricoHandTuning[side].offset[axis] = std::isfinite(offset) ? std::clamp(offset, -0.1f, 0.1f) : 0;
                result.ricoHandTuning[side].degrees[axis] = std::isfinite(degrees) ? std::clamp(degrees, -45.0f, 45.0f) : 0;
            }
        }
    }
    result.widenTerrainCull = ReadBool(path, L"widen_terrain_cull", true);
    result.relaxTerrainHullCulls = ReadBool(path, L"relax_terrain_hull_culls", true);
    result.widenSceneCull = ReadBool(path, L"widen_scene_cull", true);
    result.disableSceneOcclusion = ReadBool(path, L"disable_scene_occlusion", true);
    result.cullFovPadding = ReadFloat(path, L"cull_fov_padding", 0.4f);
    result.cullSizeFovDegrees = ReadFloat(path, L"cull_size_fov_deg", 65.0f);
    result.widenModelCull = ReadBool(path, L"widen_model_cull", true);
    result.cullFromHead = ReadBool(path, L"cull_from_head", true);
    result.offAxisReconstruction = ReadBool(path, L"offaxis_reconstruction", true);
    result.aimFromController = ReadBool(path, L"aim_from_controller", true);
    result.handsInGame = ReadBool(path, L"vr_hands_in_game", false);
    result.gunsToController = ReadBool(path, L"guns_to_controller", true);
    result.weaponControllerRoll = ReadBool(path, L"weapon_controller_roll", true);
    result.nativeAttachmentRenderRoot = ReadBool(path, L"native_attachment_render_root", true);
    result.shadowsEveryFrame = ReadBool(path, L"shadows_every_frame", true);
    result.shadowFitUnion = ReadBool(path, L"shadow_fit_union", true);
    result.shadowFitHeading = ReadBool(path, L"shadow_fit_heading", true);
    result.vrBodyHandAimFrame = ReadBool(path, L"vr_body_hand_aim_frame", true);
    result.hudAlign = ReadBool(path, L"hud_align", true);
    result.wristHud = ReadBool(path, L"wrist_hud", true);
    result.vrSuppressDamageEffects=ReadBool(path,L"vr_suppress_damage_effects",true);
    result.hudDistanceMeters = ReadFloat(path, L"hud_distance_m", 3.0f);
    result.hudScale = ReadFloat(path, L"hud_scale", 0.65f);
    result.menuDistanceMeters = ReadFloat(path, L"menu_distance_m", 4.0f);
    result.menuScale = ReadFloat(path, L"menu_scale", 0.55f);
    result.reprojectToFramePose = ReadBool(path, L"reproject_to_frame_pose", false);
    result.handAimGain = ReadFloat(path, L"hand_aim_gain", 0.5f);
    result.snapTurnDegrees = ReadFloat(path, L"snap_turn_deg", 30.0f);
    result.helicopterHeadSteering = ReadBool(path, L"helicopter_head_steering", true);
    result.helicopterPullHijack = ReadBool(path, L"helicopter_pull_hijack", false);
    result.helicopterHijackTrace = ReadBool(path, L"helicopter_hijack_trace", false);
    result.physicalHelicopterHijack = ReadBool(path, L"physical_helicopter_hijack", false);
    result.debugHandAim = ReadBool(path, L"debug_hand_aim", false);
    result.debugHandAimYawDegrees = ReadFloat(path, L"debug_hand_aim_yaw_deg", 0.0f);
    result.debugHandAimPitchDegrees = ReadFloat(path, L"debug_hand_aim_pitch_deg", 0.0f);
    result.renderHeight = static_cast<int>(ReadFloat(path, L"render_height", 0.0f));
    return result;
}

std::string ToString(CameraMode mode) { return mode == CameraMode::FirstPerson ? "first_person" : "current_working"; }
std::string ToString(StereoMode mode) {
    switch (mode) {
    case StereoMode::Geo11: return "geo11";
    case StereoMode::ExternalSbs: return "external_sbs";
    case StereoMode::NativeFull: return "native_full";
    case StereoMode::NativeAlternate: return "native_alternate";
    default: return "mono_debug";
    }
}
std::string ToString(Geo11StereoSource source) {
    switch (source) {
    case Geo11StereoSource::LeftToBoth: return "left_to_both";
    case Geo11StereoSource::RightToBoth: return "right_to_both";
    default: return "proper_sbs";
    }
}
std::string ToString(NativeStereoRigMode mode) {
    switch (mode) {
    case NativeStereoRigMode::LeftToBoth: return "left_to_both";
    case NativeStereoRigMode::RightToBoth: return "right_to_both";
    case NativeStereoRigMode::NativeStereo: return "native_stereo";
    case NativeStereoRigMode::SwapEyes: return "swap_eyes";
    default: return "center";
    }
}
}
