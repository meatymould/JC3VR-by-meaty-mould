// Rico's VR body on his own skeleton (render-side; no game logic touched).
//
// Map (engine facts in docs/ENGINE_REFERENCE.md section 5):
//   discovery:        Discover (background vtable scan for Rico's CCharacter and
//                     the 177-bone hkaSkeleton), ReadSkeleton, Palette, IsRico
//   solve:            VrBodyApplyAtUpload (at the Globals map and each 4 KB /
//                     528 B bone upload map): two-bone arm IK towards the
//                     controller targets, hand orientation from the aim pose,
//                     head collapse or hide-all, per-segment carry corrections
//   upload fix-ups:   VrBodyRewriteUpload (the game fills bone uploads from its
//                     own copy of a fresh pose every 2nd frame: entries are
//                     matched by value and replaced), VrBodyCarryUpload +
//                     CarryOrHideMatrix (objects riding Rico's bones: arm riders
//                     follow the controllers, others hide with the body)
//   Present tick:     VrBodyPresentTick (discovery scheduling, validation,
//                     trigger files, periodic stats)
//   probes:           attach probe, parity probe, upload diagnostic
#include "game/vr_body_rig.h"
#include "game/vehicle_camera.h"
#include "game/render_character_root.h"
#include "game/placed_transform_history.h"
#include "game/upload_matrix_ownership.h"
#include "game/gauntlet_upload.h"
#include "game/vr_body.h"
#include "game/weapon_pose.h"
#include "game/wrist_pose.h"
#include "game/movement_anchor.h"
#include "game/camera_capture.h"
#include "core/build_fingerprint.h"
#include "core/logging.h"
#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace jc3vr {
namespace {
constexpr std::uintptr_t kCharacterVtableRva = 0x023d16d8;
constexpr std::uintptr_t kAnimatedModelVtableRva = 0x0237e850;
constexpr std::uintptr_t kAnimationControlVtableRva = 0x023773d0;
constexpr std::uintptr_t kSkeletonVtableRva = 0x0258bd30;
constexpr std::size_t kSkeletonPathOffset = 0x4E0;
constexpr std::size_t kWorldMatrixOffset = 0x27E8;
constexpr std::size_t kAnimatedModelOffset = 0x1778;
constexpr std::size_t kBoneCount = 177;
// Arm chains: {upper arm, forearm, hand} (shoulder joint = upper arm head).
constexpr int kLeftArm[3]{23, 24, 25};
constexpr int kRightArm[3]{63, 64, 65};
// Hand frame from the animated fingers: {index1, middle1, pinky1}.
constexpr int kLeftFingers[3]{30, 33, 41};
constexpr int kRightFingers[3]{70, 73, 81};
// Rico's wrist (hand bone origin) sits ~8 cm behind the palm centre, which
// is where the OpenXR grip pose is.
constexpr float kWristToPalm = 0.08f;

bool SafeCopy(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Render-side conversions must use the SAME root that generated the scene.
// Absence/rejection retains the original raw path; never estimate motion.
bool ReadBodyRenderWorld(std::uintptr_t character,float world[16]) {
    return GetCharacterRenderWorld(character,world) ||
        SafeCopy(world,reinterpret_cast<const void*>(character+kWorldMatrixOffset),64);
}

template <typename T> bool Read(std::uintptr_t address, T& value) { return SafeCopy(&value, reinterpret_cast<const void*>(address), sizeof(T)); }

struct Affine {  // column-vector: p' = R p + t, R rows r[0..2]
    float r[3][3];
    float t[3];
};

Affine FromPalette(const float* m) {
    Affine a{};
    for (int row = 0; row != 3; ++row) {
        for (int c = 0; c != 3; ++c) a.r[row][c] = m[row * 4 + c];
        a.t[row] = m[row * 4 + 3];
    }
    return a;
}

void ToPalette(const Affine& a, float* m) {
    for (int row = 0; row != 3; ++row) {
        for (int c = 0; c != 3; ++c) m[row * 4 + c] = a.r[row][c];
        m[row * 4 + 3] = a.t[row];
    }
}

BodyVec3 Apply(const Affine& a, const BodyVec3& p) {
    return {a.r[0][0] * p.x + a.r[0][1] * p.y + a.r[0][2] * p.z + a.t[0],
            a.r[1][0] * p.x + a.r[1][1] * p.y + a.r[1][2] * p.z + a.t[1],
            a.r[2][0] * p.x + a.r[2][1] * p.y + a.r[2][2] * p.z + a.t[2]};
}

Affine Compose(const Affine& outer, const Affine& inner) {  // outer after inner
    Affine out{};
    for (int i = 0; i != 3; ++i) {
        for (int j = 0; j != 3; ++j) out.r[i][j] = outer.r[i][0] * inner.r[0][j] + outer.r[i][1] * inner.r[1][j] + outer.r[i][2] * inner.r[2][j];
        out.t[i] = outer.r[i][0] * inner.t[0] + outer.r[i][1] * inner.t[1] + outer.r[i][2] * inner.t[2] + outer.t[i];
    }
    return out;
}

// Rotation taking unit direction `from` to `to` about `pivot` (shortest arc).
Affine RotationBetween(BodyVec3 from, BodyVec3 to, const BodyVec3& pivot) {
    const auto normalize = [](BodyVec3 v) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 1e-6f ? BodyVec3{v.x / l, v.y / l, v.z / l} : v;
    };
    from = normalize(from);
    to = normalize(to);
    BodyVec3 axis{from.y * to.z - from.z * to.y, from.z * to.x - from.x * to.z, from.x * to.y - from.y * to.x};
    const float s = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    const float c = std::clamp(from.x * to.x + from.y * to.y + from.z * to.z, -1.0f, 1.0f);
    Affine a{};
    if (s < 1e-6f) {
        for (int i = 0; i != 3; ++i) a.r[i][i] = 1.0f;
    } else {
        axis = {axis.x / s, axis.y / s, axis.z / s};
        const float k = 1.0f - c;
        const float x = axis.x, y = axis.y, z = axis.z;
        const float m[3][3]{{c + x * x * k, x * y * k - z * s, x * z * k + y * s},
                            {y * x * k + z * s, c + y * y * k, y * z * k - x * s},
                            {z * x * k - y * s, z * y * k + x * s, c + z * z * k}};
        std::memcpy(a.r, m, sizeof(m));
    }
    const BodyVec3 rotatedPivot = Apply(a, pivot);
    a.t[0] = pivot.x - rotatedPivot.x;
    a.t[1] = pivot.y - rotatedPivot.y;
    a.t[2] = pivot.z - rotatedPivot.z;
    return a;
}

struct Rig {
    std::uintptr_t character = 0;
    std::uintptr_t palette = 0;          // CSkinningPaletteData
    std::vector<BodyVec3> rest;          // rest-pose model positions
    std::vector<std::array<float, 4>> restQ;  // rest-pose model rotations (x, y, z, w)
    std::vector<int> parents;
    std::vector<std::vector<int>> subtree;  // bone -> itself + descendants
    std::vector<int> armSegment;            // bone -> side*3 + {0 upper, 1 fore, 2 hand}, or -1
};

bool g_enabled = false;
bool g_debugPose = false;
// Collapse the head (Head bone subtree) to a point at the neck so a full
// body model can be used in first person without the head in the camera.
bool g_hideHead = false;
// Hide the whole body (all palette entries collapsed); IK still drives carry.
bool g_hideAll = false;
std::atomic<bool> g_ricoNativeHands = false;
std::once_flag g_ricoHandsStartup;
std::array<RicoHandTuning, 2> g_ricoHandTuning{};
std::atomic<ULONGLONG> g_ricoHandPrepared[2]{};
std::array<bool, kBoneCount> g_ricoVisibleBones{}; // guarded by g_rigMutex
constexpr int kHeadBone = 8;
constexpr int kNeckBone = 7;
std::atomic<int> g_scanState = 0;  // 0 idle, 1 scanning, 2 ready, 3 failed
std::mutex g_rigMutex;
Rig g_rig;
std::uintptr_t g_imageBase = 0;
std::mutex g_targetMutex;
bool g_targetValid[2]{};
ULONGLONG g_targetTick[2]{};
float g_targetWorld[2][3]{};
float g_targetRoot[2][3]{};
float g_targetCamera[2][3]{};
bool g_targetCameraValid[2]{}, g_targetFirstPersonAnchor[2]{};
std::uintptr_t g_targetCharacter[2]{};
float g_targetFingers[2][3]{};  // world: grip -Z (index-finger pointing)
float g_targetDown[2][3]{};     // world: grip -Y (index -> pinky, down the handle)
RicoFingerInput g_targetFingersInput[2]{};
// Last written arm entries (to detect a palette the game did not refresh).
std::vector<std::array<float, 12>> g_written;
  std::vector<std::array<float, 12>> g_original;
  // Render preparation may retain an earlier animation pose for the other
  // eye. Keep a bounded, local-character-only history for hide-all matching;
  // never broaden the spatial/rotation tolerances or match NPC identities.
  std::array<std::vector<std::array<float, 12>>, 4> g_hidePoseHistory;
  std::size_t g_hidePoseCursor = 0;
  std::uintptr_t g_hidePoseCharacter = 0;
std::uint64_t g_applies = 0, g_invalid = 0, g_reapplies = 0;
// Objects attached to Rico's arms (the gun, the grapple gauntlet) are
// separate draws positioned from the game's animated bones, so the palette
// IK alone leaves them behind. This frame's world-space corrections the arm
// IK applied (column-vector, p' = C p) per arm segment, and the animated
// (game) world positions of the arm bones 22..101 they are matched against.
// Guarded by g_rigMutex.
constexpr int kArmBoneFirst = 22, kArmBoneEnd = 102;
struct ArmCarry {
    bool valid[2]{};
    Affine segment[2][3]{};  // [side][upper, fore, hand]
    BodyVec3 bones[kArmBoneEnd - kArmBoneFirst]{};  // animated world positions
    Affine world[177]{};  // every bone's animated world transform (column-vector)
    bool bonesValid = false;
};
ArmCarry g_carry;
std::atomic<std::uintptr_t> g_carryCharacter = 0;
// Where attached objects' world matrices live (row-major 4x4, absolute
// world). Live attach probe 2026-09-30, Rico firing the Bavarium rifle:
// 208-byte constant buffer +0x0 and +0x40 held a matrix exactly on
// RightHandAttach (bone 66) (+0x40 presumably last frame's, for motion
// vectors); a 128-byte buffer +0x0 held the same; a 4096-byte buffer +0x0
// sat exactly on LeftForeArmRoll (bone 45) (grapple gauntlet); another
// 208-byte object sat 6.6 cm from LeftHandAttach2 (bone 44).
struct CarrySignature {
    std::size_t size;
    std::size_t offsets[4];
    std::size_t count;
};
// 4096 +0x0 (bone 45) was dropped: with it carried, the grapple gauntlet
// vanished in the headset (user 2026-09-30); it may be a palette, not a world.
constexpr CarrySignature kCarrySignatures[]{{208, {0x0, 0x40}, 2}, {128, {0x0}, 1}};
// An attached object's origin lies within this distance of an arm bone.
constexpr float kCarryRadius = 0.15f;
bool g_carryEnabled = false;
std::atomic<bool> g_carryActive = false;  // runtime A/B toggle (trigger file)
std::atomic<bool> g_debugAlternate = false;  // debug pose variant (trigger file)
std::uint64_t g_carried = 0, g_carryMisses = 0, g_hiddenRiders = 0;

std::uintptr_t Palette(std::uintptr_t character) {
    std::uintptr_t vtable = 0, control = 0, palette = 0;
    const std::uintptr_t model = character + kAnimatedModelOffset;
    if (!Read(model, vtable) || vtable != g_imageBase + kAnimatedModelVtableRva) return 0;
    if (!Read(model + 0x178, control) || !Read(control, vtable) || vtable != g_imageBase + kAnimationControlVtableRva) return 0;
    if (!Read(control + 0xA8, palette) || !palette) return 0;
    std::uint64_t count = 0;
    std::uintptr_t begin = 0, end = 0;
    if (!Read(palette + 0x48, count) || count != kBoneCount || !Read(palette, begin) || !Read(palette + 8, end) ||
        begin != palette + 0x50 || end - begin != kBoneCount * 48) return 0;
    return palette;
}

bool IsRico(std::uintptr_t character) {
    std::uintptr_t vtable = 0, path = 0;
    if (!Read(character, vtable) || vtable != g_imageBase + kCharacterVtableRva || !Read(character + kSkeletonPathOffset, path)) return false;
    char text[64]{};
    if (!SafeCopy(text, reinterpret_cast<const void*>(path), sizeof(text) - 1)) return false;
    return std::strstr(text, "jc_skeletons/characters/rico") != nullptr;
}

// hkaSkeleton "Reference" with 177 bones, LeftHand at 25, RightHand at 65.
bool ReadSkeleton(std::uintptr_t skeleton, Rig& rig) {
    std::uintptr_t header[10]{};
    if (!SafeCopy(header, reinterpret_cast<const void*>(skeleton), sizeof(header))) return false;
    if (static_cast<std::uint32_t>(header[4]) != kBoneCount || static_cast<std::uint32_t>(header[6]) != kBoneCount || static_cast<std::uint32_t>(header[8]) != kBoneCount) return false;
    const auto boneName = [&](int bone) {
        std::uintptr_t name = 0;
        char text[32]{};
        if (!Read(header[5] + bone * 16, name) || !SafeCopy(text, reinterpret_cast<const void*>(name & ~std::uintptr_t(1)), sizeof(text) - 1)) return std::string();
        return std::string(text);
    };
    if (boneName(25) != "LeftHand" || boneName(65) != "RightHand" || boneName(24) != "LeftForeArm" || boneName(64) != "RightForeArm") return false;
    std::vector<std::int16_t> parents(kBoneCount);
    std::vector<float> pose(kBoneCount * 12);
    if (!SafeCopy(parents.data(), reinterpret_cast<const void*>(header[3]), kBoneCount * 2) ||
        !SafeCopy(pose.data(), reinterpret_cast<const void*>(header[7]), pose.size() * 4)) return false;
    struct Xf { BodyVec3 t; float q[4]; };
    std::vector<Xf> model(kBoneCount);
    rig.rest.assign(kBoneCount, {});
    rig.restQ.assign(kBoneCount, {});
    rig.parents.assign(kBoneCount, -1);
    for (std::size_t i = 0; i != kBoneCount; ++i) {
        const float* local = pose.data() + i * 12;
        const int parent = parents[i];
        rig.parents[i] = parent;
        Xf& m = model[i];
        if (parent < 0 || parent >= static_cast<int>(i)) {
            m.t = {local[0], local[1], local[2]};
            std::copy(local + 4, local + 8, m.q);
        } else {
            const float* q = model[parent].q;
            const float v[3]{local[0], local[1], local[2]};
            const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
            const BodyVec3 rotated{v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz), v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
            m.t = {model[parent].t.x + rotated.x, model[parent].t.y + rotated.y, model[parent].t.z + rotated.z};
            const float* b = local + 4;
            m.q[0] = q[3] * b[0] + q[0] * b[3] + q[1] * b[2] - q[2] * b[1];
            m.q[1] = q[3] * b[1] - q[0] * b[2] + q[1] * b[3] + q[2] * b[0];
            m.q[2] = q[3] * b[2] + q[0] * b[1] - q[1] * b[0] + q[2] * b[3];
            m.q[3] = q[3] * b[3] - q[0] * b[0] - q[1] * b[1] - q[2] * b[2];
        }
        rig.rest[i] = m.t;
        rig.restQ[i] = {m.q[0], m.q[1], m.q[2], m.q[3]};
    }
    rig.subtree.assign(kBoneCount, {});
    for (int bone = static_cast<int>(kBoneCount) - 1; bone >= 0; --bone) {
        rig.subtree[bone].insert(rig.subtree[bone].begin(), bone);
        const int parent = rig.parents[bone];
        if (parent >= 0) rig.subtree[parent].insert(rig.subtree[parent].end(), rig.subtree[bone].begin(), rig.subtree[bone].end());
    }
    // Innermost arm segment first wins (hand inside forearm inside upper arm).
    rig.armSegment.assign(kBoneCount, -1);
    for (int side = 0; side != 2; ++side) {
        const int* chain = side == 0 ? kLeftArm : kRightArm;
        for (int segment = 0; segment != 3; ++segment)
            for (const int bone : rig.subtree[chain[segment]]) rig.armSegment[bone] = side * 3 + segment;
    }
    return true;
}

// Background discovery: Rico's CCharacter and the 177-bone skeleton.
void Discover() {
    const auto started = GetTickCount64();
    const std::uintptr_t characterVtable = g_imageBase + kCharacterVtableRva;
    const std::uintptr_t skeletonVtable = g_imageBase + kSkeletonVtableRva;
    std::vector<std::uintptr_t> characters, skeletons;
    std::vector<std::uint8_t> buffer(1 << 20);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* address = static_cast<std::uint8_t*>(system.lpMinimumApplicationAddress);
    MEMORY_BASIC_INFORMATION info{};
    while (address < system.lpMaximumApplicationAddress && VirtualQuery(address, &info, sizeof(info))) {
        auto* next = static_cast<std::uint8_t*>(info.BaseAddress) + info.RegionSize;
        const DWORD protect = info.Protect & 0xFF;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE)) {
            for (std::size_t chunk = 0; chunk < info.RegionSize; chunk += buffer.size()) {
                const std::size_t size = std::min(buffer.size(), info.RegionSize - chunk);
                auto* source = static_cast<std::uint8_t*>(info.BaseAddress) + chunk;
                if (!SafeCopy(buffer.data(), source, size)) break;
                for (std::size_t i = 0; i + 8 <= size; i += 8) {
                    std::uintptr_t q;
                    std::memcpy(&q, buffer.data() + i, 8);
                    if (q == characterVtable && characters.size() < 512) characters.push_back(reinterpret_cast<std::uintptr_t>(source + i));
                    else if (q == skeletonVtable && skeletons.size() < 512) skeletons.push_back(reinterpret_cast<std::uintptr_t>(source + i));
                }
            }
        }
        address = next;
    }
    Rig rig;
    for (const auto candidate : characters) {
        if (IsRico(candidate) && Palette(candidate)) { rig.character = candidate; break; }
    }
    bool skeleton = false;
    for (const auto candidate : skeletons) if ((skeleton = ReadSkeleton(candidate, rig))) break;
    const bool ok = rig.character && skeleton;
    if (ok) rig.palette = Palette(rig.character);
    std::ostringstream line;
    line << "VR body: discovery " << (ok ? "ready" : "failed") << " characters=" << characters.size() << " skeletons=" << skeletons.size()
         << " rico=0x" << std::hex << rig.character << " palette=0x" << rig.palette << std::dec << " ms=" << GetTickCount64() - started;
    if (skeleton) line << " rest LeftHand=(" << rig.rest[25].x << "," << rig.rest[25].y << "," << rig.rest[25].z << ") Head y=" << rig.rest[8].y;
    TraceRuntime(line.str());
    {
        std::lock_guard lock(g_rigMutex);
        g_rig = std::move(rig);
        g_written.clear();
        g_ricoVisibleBones.fill(false);
        g_original.clear();
        for (auto& pose : g_hidePoseHistory) pose.clear();
        g_hidePoseCursor = 0;
        g_hidePoseCharacter = 0;
    }
    g_scanState.store(ok ? 2 : 3);
}

// World direction -> Rico model space (rotation only).
bool WorldDirectionToModel(std::uintptr_t character, const float world[3], BodyVec3& model) {
    float m[16]{};
    if (!ReadBodyRenderWorld(character,m)) return false;
    model = {world[0] * m[0] + world[1] * m[1] + world[2] * m[2], world[0] * m[4] + world[1] * m[5] + world[2] * m[6], world[0] * m[8] + world[1] * m[9] + world[2] * m[10]};
    return true;
}

// Rotation mapping the frame built from (a1, a2) onto the one from (b1, b2)
// (first axes aligned exactly, second axes as closely as possible), about pivot.
Affine AlignFrames(BodyVec3 a1, BodyVec3 a2, BodyVec3 b1, BodyVec3 b2, const BodyVec3& pivot) {
    const auto normalize = [](BodyVec3 v) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 1e-6f ? BodyVec3{v.x / l, v.y / l, v.z / l} : BodyVec3{0.0f, 0.0f, 0.0f};
    };
    const auto dot = [](BodyVec3 a, BodyVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto cross = [](BodyVec3 a, BodyVec3 b) { return BodyVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    const auto frame = [&](BodyVec3 x, BodyVec3 y, float out[3][3]) {  // out[axis][component]
        const BodyVec3 e0 = normalize(x);
        const float d = dot(y, e0);
        const BodyVec3 e1 = normalize({y.x - e0.x * d, y.y - e0.y * d, y.z - e0.z * d});
        const BodyVec3 e2 = cross(e0, e1);
        const BodyVec3 axes[3]{e0, e1, e2};
        for (int k = 0; k != 3; ++k) { out[k][0] = axes[k].x; out[k][1] = axes[k].y; out[k][2] = axes[k].z; }
    };
    float a[3][3], b[3][3];
    frame(a1, a2, a);
    frame(b1, b2, b);
    Affine r{};
    // R = sum_k b_k a_k^T maps each a_k onto b_k.
    for (int i = 0; i != 3; ++i)
        for (int j = 0; j != 3; ++j) r.r[i][j] = b[0][i] * a[0][j] + b[1][i] * a[1][j] + b[2][i] * a[2][j];
    const BodyVec3 rotated = Apply(r, pivot);
    r.t[0] = pivot.x - rotated.x;
    r.t[1] = pivot.y - rotated.y;
    r.t[2] = pivot.z - rotated.z;
    return r;
}

// World point -> Rico model space through the row-major world transform
// (p_world = p_model * R + t  =>  p_model = (p_world - t) * R^T).
bool WorldToModel(std::uintptr_t character, const float world[3], BodyVec3& model) {
    float m[16]{};
    if (!ReadBodyRenderWorld(character,m) || std::abs(m[15] - 1.0f) > 1e-3f) return false;
    const float d[3]{world[0] - m[12], world[1] - m[13], world[2] - m[14]};
    model = {d[0] * m[0] + d[1] * m[1] + d[2] * m[2], d[0] * m[4] + d[1] * m[5] + d[2] * m[6], d[0] * m[8] + d[1] * m[9] + d[2] * m[10]};
    return true;
}
}

// ---- Upload diagnostic: which GPU upload carries Rico's palette, when, and
// whether it holds the game's hand matrix or JC3VR's IK result.
namespace {
std::atomic<bool> g_appliedThisFrame = false;
std::atomic<int> g_diagFramesLeft = 0;
std::atomic<int> g_diagLines = 0;
}

// ---- Attachment probe (on demand, read-only, bounded): which buffer uploads
// carry a rigid matrix aligned with one of Rico's animated bones, i.e. a
// weapon or gadget held in / attached to a hand. Armed by creating
// %LOCALAPPDATA%\JC3VR\jc3vr_attach_probe (consumed at the next Present).
namespace {
std::atomic<int> g_attachProbeFramesLeft = 0;
std::atomic<int> g_attachProbeLines = 0;
struct BoneWorld {
    float r[3][3];  // world rotation, column-vector
    BodyVec3 p;     // world position
};
std::mutex g_attachProbeMutex;
std::vector<BoneWorld> g_attachBones;
std::uint64_t g_attachBonesKey = ~0ull;
std::vector<std::uint64_t> g_attachSeen;

Affine QuatToAffine(const std::array<float, 4>& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Affine a{};
    a.r[0][0] = 1 - 2 * (y * y + z * z); a.r[0][1] = 2 * (x * y - z * w);     a.r[0][2] = 2 * (x * z + y * w);
    a.r[1][0] = 2 * (x * y + z * w);     a.r[1][1] = 1 - 2 * (x * x + z * z); a.r[1][2] = 2 * (y * z - x * w);
    a.r[2][0] = 2 * (x * z - y * w);     a.r[2][1] = 2 * (y * z + x * w);     a.r[2][2] = 1 - 2 * (x * x + y * y);
    return a;
}

// World transforms of all animated bones (game pose, before JC3VR's IK).
bool AttachBones(std::vector<BoneWorld>& out, float character[16]) {
    std::lock_guard lock(g_rigMutex);
    if (g_original.size() != kBoneCount || g_rig.restQ.size() != kBoneCount) return false;
    if (!SafeCopy(character, reinterpret_cast<const void*>(g_rig.character + kWorldMatrixOffset), 64)) return false;
    std::lock_guard cacheLock(g_attachProbeMutex);
    if (g_attachBonesKey != g_applies || g_attachBones.size() != kBoneCount) {
        g_attachBones.assign(kBoneCount, {});
        for (std::size_t i = 0; i != kBoneCount; ++i) {
            const Affine palette = FromPalette(g_original[i].data());
            const Affine model = Compose(palette, QuatToAffine(g_rig.restQ[i]));
            const BodyVec3 position = Apply(palette, g_rig.rest[i]);
            const float mp[3]{position.x, position.y, position.z};
            BoneWorld& bone = g_attachBones[i];
            // Row-major world: p_world = p_model * R + t (R rows at 0/4/8).
            for (int a = 0; a != 3; ++a) {
                for (int b = 0; b != 3; ++b) bone.r[a][b] = character[a] * model.r[0][b] + character[4 + a] * model.r[1][b] + character[8 + a] * model.r[2][b];
            }
            bone.p = {mp[0] * character[0] + mp[1] * character[4] + mp[2] * character[8] + character[12],
                      mp[0] * character[1] + mp[1] * character[5] + mp[2] * character[9] + character[13],
                      mp[0] * character[2] + mp[1] * character[6] + mp[2] * character[10] + character[14]};
        }
        g_attachBonesKey = g_applies;
    }
    out = g_attachBones;
    return true;
}

void AttachProbe(const void* data, std::size_t size, UINT bindFlags, const char* how) {
    if (size < 48 || size > 65536 || g_attachProbeLines.load() >= 80) return;
    std::vector<BoneWorld> bones;
    float character[16]{};
    if (!AttachBones(bones, character)) return;
    float camera[3]{};
    const bool haveCamera = GetEngineCameraPosition(camera);
    std::vector<float> f(size / 4);
    std::memcpy(f.data(), data, f.size() * 4);
    const std::size_t n = f.size();
    (void)haveCamera;
    // Live 2026-09-30 (first probe): object world matrices are uploaded as
    // absolute row-major 4x4 (rows 0/4/8, translation at 12..14, w column
    // 0,0,0,1), e.g. Rico's own world matrix verbatim in 4 KB / 208 B / 512 B
    // constant buffers. Report every such matrix near a hand/arm bone, with
    // its pose in that bone's frame (what the gun is relative to the hand).
    const float* rico = character;
    for (std::size_t o = 0; o + 16 <= n; o += 4) {
        const float* m = f.data() + o;
        if (std::abs(m[3]) > 1e-4f || std::abs(m[7]) > 1e-4f || std::abs(m[11]) > 1e-4f || std::abs(m[15] - 1.0f) > 1e-4f) continue;
        const auto dot = [&](int a, int b) { return m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2]; };
        bool rigid = true;
        for (int r = 0; r != 3 && rigid; ++r) rigid = std::abs(dot(r, r) - 1.0f) < 0.02f;
        rigid = rigid && std::abs(dot(0, 1)) < 0.02f && std::abs(dot(0, 2)) < 0.02f && std::abs(dot(1, 2)) < 0.02f;
        if (!rigid) continue;
        // Skip Rico's own world matrix.
        bool own = true;
        for (int k = 0; k != 16 && own; ++k) own = std::abs(m[k] - rico[k]) < 1e-3f;
        if (own) continue;
        const BodyVec3 t{m[12], m[13], m[14]};
        int nearest = -1;
        float nearestDistance = 0.6f;
        for (std::size_t b = 22; b != 102; ++b) {  // arms, hands, fingers, attach points
            const BodyVec3& p = bones[b].p;
            const float d = std::sqrt((t.x - p.x) * (t.x - p.x) + (t.y - p.y) * (t.y - p.y) + (t.z - p.z) * (t.z - p.z));
            if (d < nearestDistance) { nearestDistance = d; nearest = static_cast<int>(b); }
        }
        if (nearest < 0) continue;
        const std::uint64_t key = (static_cast<std::uint64_t>(size) << 32) | (static_cast<std::uint64_t>(o) << 9) | static_cast<std::uint64_t>(nearest);
        {
            std::lock_guard lock(g_attachProbeMutex);
            if (std::find(g_attachSeen.begin(), g_attachSeen.end(), key) != g_attachSeen.end()) continue;
            g_attachSeen.push_back(key);
        }
        // Pose in the frame of the hand bones (column-vector world rotation W,
        // object rotation Rc[i][j] = m[j*4+i]): local = W^T (t - p), W^T Rc.
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(3);
        line << "VR body attach probe: " << how << " size=" << size << " bind=0x" << std::hex << bindFlags << " off=0x" << o * 4 << std::dec
             << " t=(" << t.x << "," << t.y << "," << t.z << ") nearest=" << nearest << " d=" << nearestDistance;
        for (const int b : {65, 66, 84, 25, 26, 44}) {
            const BoneWorld& bone = bones[b];
            const float d[3]{t.x - bone.p.x, t.y - bone.p.y, t.z - bone.p.z};
            float local[3]{}, rotation[3][3]{};
            for (int i = 0; i != 3; ++i) {
                local[i] = bone.r[0][i] * d[0] + bone.r[1][i] * d[1] + bone.r[2][i] * d[2];
                for (int j = 0; j != 3; ++j) rotation[i][j] = bone.r[0][i] * m[j * 4 + 0] + bone.r[1][i] * m[j * 4 + 1] + bone.r[2][i] * m[j * 4 + 2];
            }
            line << " | b" << b << " local=(" << local[0] << "," << local[1] << "," << local[2] << ") R=[" << rotation[0][0] << "," << rotation[0][1] << "," << rotation[0][2] << ";"
                 << rotation[1][0] << "," << rotation[1][1] << "," << rotation[1][2] << ";" << rotation[2][0] << "," << rotation[2][1] << "," << rotation[2][2] << "]";
        }
        TraceRuntime(line.str());
        if (++g_attachProbeLines >= 80) return;
    }
}
}

// ---- Parity probe (on demand, trigger file jc3vr_parity_probe, ~60 frames):
// per Present, how many 4 KB / 528 B constant-buffer uploads carry JC3VR's
// written right-hand entry vs the game's original one. Live 2026-09-30: an
// untouched Rico is drawn on alternate frames although only one 177-bone
// palette exists in memory.
namespace {
std::atomic<int> g_parityFramesLeft = 0;
std::atomic<std::uint32_t> g_parityOurs = 0, g_parityOriginal = 0, g_parityUploads = 0;

void ParityProbe(const void* data, std::size_t size) {
    if (size != 4096 && size != 528) return;
    std::array<float, 12> ours{}, original{};
    {
        std::lock_guard lock(g_rigMutex);
        if (g_written.size() != kBoneCount || g_original.size() != kBoneCount) return;
        ours = g_written[kRightArm[2]];
        original = g_original[kRightArm[2]];
    }
    std::vector<std::uint8_t> copy(size);
    std::memcpy(copy.data(), data, size);
    bool hasOurs = false, hasOriginal = false;
    for (std::size_t offset = 0; offset + 48 <= size; offset += 16) {
        if (!hasOurs && std::memcmp(copy.data() + offset, ours.data(), 48) == 0) hasOurs = true;
        if (!hasOriginal && std::memcmp(copy.data() + offset, original.data(), 48) == 0) hasOriginal = true;
    }
    ++g_parityUploads;
    if (hasOurs) ++g_parityOurs;
    if (hasOriginal) ++g_parityOriginal;
}
}

void VrBodyObserveUpload(ID3D11Resource* resource, const void* data, std::size_t size, const char* how) {
    const bool diag = g_enabled && g_diagFramesLeft.load() > 0 && g_diagLines.load() < 24;
    const bool attach = g_enabled && g_attachProbeFramesLeft.load() > 0;
    const bool parity = g_enabled && g_parityFramesLeft.load() > 0;
    if ((!diag && !attach && !parity) || !data || g_scanState.load() != 2) return;
    if (parity && resource) {
        D3D11_RESOURCE_DIMENSION dimension{};
        resource->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC desc{};
            static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
            if (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ParityProbe(data, desc.ByteWidth);
        }
    }
    if (!diag && !attach) return;
    UINT bindFlags = 0;
    if (resource) {
        ID3D11Buffer* buffer = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) && buffer) {
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            if (size == 0) size = desc.ByteWidth;
            bindFlags = desc.BindFlags;
            buffer->Release();
        }
    }
    if (attach) AttachProbe(data, size, bindFlags, how);
    if (!diag) return;
    std::array<float, 12> hips{}, handOriginal{}, handWritten{};
    {
        std::lock_guard lock(g_rigMutex);
        if (g_original.size() != kBoneCount || g_written.size() != kBoneCount) return;
        hips = g_original[2];
        handOriginal = g_original[kLeftArm[2]];
        handWritten = g_written[kLeftArm[2]];
    }
    if (size < 48 || size > (4u << 20)) return;  // mapped memory is slow to read
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    // Rows as stored (m00 m01 m02) or transposed (m00 m10 m20).
    const float rowPattern[3]{hips[0], hips[1], hips[2]};
    const float columnPattern[3]{hips[0], hips[4], hips[8]};
    for (std::size_t offset = 0; offset + 12 <= size; offset += 4) {
        const bool row = std::memcmp(bytes + offset, rowPattern, 12) == 0;
        const bool column = !row && std::memcmp(bytes + offset, columnPattern, 12) == 0;
        if (!row && !column) continue;
        std::string handState = "n/a";
        // Assume palette stride 48 from the Hips slot (bone 2).
        const std::size_t handSlot = offset + static_cast<std::size_t>(kLeftArm[2] - 2) * 48;
        if (row && handSlot + 48 <= size) {
            handState = std::memcmp(bytes + handSlot, handWritten.data(), 48) == 0 ? "JC3VR-IK" :
                        std::memcmp(bytes + handSlot, handOriginal.data(), 48) == 0 ? "game" : "other";
        }
        std::ostringstream line;
        line << "VR body upload diag: " << how << " size=" << size << " hips@+0x" << std::hex << offset << std::dec << (row ? " rows" : " transposed")
             << " left-hand slot=" << handState << " applied_this_frame=" << (g_appliedThisFrame.load() ? 1 : 0);
        TraceRuntime(line.str());
        ++g_diagLines;
        return;
    }
}

namespace {
std::atomic<bool> g_handAimFrame = true;
}

void ConfigureVrBodyHandFrame(bool aimFrame) {
    g_handAimFrame.store(aimFrame);
    TraceRuntime(std::string("VR body: hands oriented from the controller ") + (aimFrame ? "aim" : "grip") + " pose");
}

bool VrBodyHandsUseAimFrame() { return g_handAimFrame.load(); }

// ---- Upload rewrite. Live parity probe 2026-09-30 (VR, 29 uploads/frame):
// odd frames 20 uploads carried JC3VR's hand entry, 7 the game's; even frames
// only 7 JC3VR, 7 game and 15 NEITHER, while the palette object was re-solved
// every 2nd frame. On the frames the game refreshes Rico's animation it fills
// his bone uploads from its own copy of the fresh pose, so the palette write
// never reaches those draws (the untouched "animation Rico" in one eye).
// At Unmap every 48-byte entry equal to a current game palette entry of a
// bone JC3VR owns is replaced by JC3VR's value; only uploads with at least
// three distinctive Rico matches are touched.
namespace {
// Grid over the entries' translations (column 3, model space); cell 5 cm.
constexpr float kRewriteCell = 0.05f, kRewriteTranslation = 0.03f, kRewriteRotation = 0.05f;
std::unordered_multimap<std::int64_t, int> g_rewriteGrid;
std::uint64_t g_rewriteKey = ~0ull;
std::uint64_t g_rewritten = 0, g_rewriteUploads = 0;
std::vector<bool> g_ownedBones;

std::int64_t CellKey(int x, int y, int z) {
    return (static_cast<std::int64_t>(x & 0x1FFFFF) << 42) | (static_cast<std::int64_t>(y & 0x1FFFFF) << 21) | static_cast<std::int64_t>(z & 0x1FFFFF);
}
}

static bool CarryOrHideMatrix(float* m, const float* rico, bool requireRotation, float radius, bool* owned = nullptr);

// The last held object the carry moved on each hand: the game's own matrix
// and where we drew it. Read by vr_weapons (the weapon probe, bullet origin).
struct LastCarried {
    float original[16]{};
    float carried[16]{};
    int bone = -1;
    ULONGLONG tick = 0;
};
LastCarried g_lastCarried[2];  // guarded by g_rigMutex
// True while vr_weapons moves the game's own guns to the drawn hands: the
// carry must then leave the gun mounts alone, or a gun it mistakes for the
// animated one gets the hand correction twice (live 2026-10-01: guns ~0.5 m
// off and rotated whenever the real hand came near the animated one).
std::atomic<bool> g_gunsPlacedInGame{false};
struct PlacedWeapon {
    std::uintptr_t object = 0;
    float matrix[16]{};
    float grip[3]{};
    int hand=-1;
    ULONGLONG tick = 0;
};
std::mutex g_placedWeaponMutex;
PlacedWeapon g_placedWeapons[32]{};
PlacedWeapon g_lateWeapons[16]{};
unsigned g_lateWeaponNext=0;
std::atomic<std::uintptr_t> g_scopeProbeWeapon=0;
std::mutex g_scopeBufferMutex;
std::unordered_map<ID3D11Resource*,ULONGLONG> g_scopeBuffers;
void TagScopeBuffer(ID3D11Resource* resource,const float matrix[16]) {
    const auto sniper=g_scopeProbeWeapon.load();if(!sniper) return;
    bool found=false;
    {
        std::lock_guard lock(g_placedWeaponMutex);
        for(const auto& placed:g_placedWeapons) {
            std::uintptr_t parent=0;
            if(placed.object && GetTickCount64()-placed.tick<200 && MatchesPlacedWeapon(matrix,placed.matrix) &&
               SafeCopy(&parent,reinterpret_cast<void*>(placed.object+0xC0),sizeof(parent)) && parent==sniper) {found=true;break;}
        }
    }
    if(found) {std::lock_guard lock(g_scopeBufferMutex);g_scopeBuffers[resource]=GetTickCount64();}
}
PlacedTransformHistory g_placedGrapple;
std::atomic<bool> g_trackedGauntletReady{false};
std::atomic<std::uint64_t> g_placedWeaponBypasses{0};
// Caller holds g_rigMutex; this helper must not lock the rig recursively.
static bool BuildLeftWristMountLocked(int bone, const float grip[3], const float fingers[3], const float down[3], float world[16]);

static bool IsAlreadyPlacedWeapon(float matrix[16], bool& changed) {
    const auto now = GetTickCount64();
    std::lock_guard lock(g_placedWeaponMutex);
    if (g_placedGrapple.Contains(matrix, now)) {
        float grip[3], fingers[3], down[3], refreshed[16];
        if (GetVrBodyRenderHandTarget(0, grip, fingers, down) &&
            BuildLeftWristMountLocked(45, grip, fingers, down, refreshed)) {
            changed = std::memcmp(matrix, refreshed, sizeof(refreshed)) != 0;
            std::memcpy(matrix, refreshed, sizeof(refreshed));
            g_placedGrapple.Publish(refreshed, now);
            static std::atomic<bool> traced{false};
            if (!traced.exchange(true)) TraceRuntime("gauntlet tracking: owned native upload refreshed from current render wrist; no velocity prediction");
        }
        return true;
    }
    if (!g_gunsPlacedInGame.load()) return false;
    // Corrected uploads retain their grip metadata too. A later tracking
    // sample can refresh them, without interpreting them as animated bones.
    for (int pass=0;pass<2;++pass) {
      const auto* entries=pass==0?g_lateWeapons:g_placedWeapons;
      const int count=pass==0?16:32;
      for(int index=0;index<count;++index) {
        const auto& weapon=entries[index];
        if (weapon.object && now - weapon.tick < 200 && MatchesPlacedWeapon(matrix, weapon.matrix)) {
            ++g_placedWeaponBypasses;
            float grip[3],fingers[3],down[3],retargeted[16];
            if(weapon.hand>=0 && GetVrBodyRenderHandTarget(weapon.hand,grip,fingers,down) &&
               RetargetPlacedWeapon(weapon.matrix,weapon.grip,grip,fingers,down,retargeted)) {
                std::memcpy(matrix,retargeted,sizeof(retargeted));
                auto& late=g_lateWeapons[g_lateWeaponNext++%16];
                late=weapon;late.tick=now;
                std::memcpy(late.matrix,matrix,sizeof(late.matrix));std::copy(grip,grip+3,late.grip);
                changed=true;
                static std::atomic<bool> traced{false};
                if(!traced.exchange(true)) TraceRuntime("weapon tracking: owned CPU placement late-latched at native upload; double carry remains blocked");
            }
            return true;
        }
      }
    }
    return false;
}

// The hand-attach bones (the gun mounts) in Rico's model space, as the game
// posed them and as JC3VR draws them (the same hand motion the carry applies
// to the gun). Model space so the sim side can use them with Rico's own
// next-step world matrix, at any speed. Published at each IK apply.
constexpr int kAttachBones[4]{26, 44, 66, 84};  // left, left2, right, right2
struct AttachPublish {
    float game[4][16]{};     // row-major 4x4, model space
    float carried[4][16]{};
    bool valid[4]{};
    ULONGLONG tick = 0;
};
std::mutex g_attachMutex;
AttachPublish g_attach;

// Column-vector affine -> the engine's row-major 4x4 (rows = axes, row 3 = translation).
static void AffineToRowMajor(const Affine& a, float m[16]) {
    for (int i = 0; i != 3; ++i) {
        for (int j = 0; j != 3; ++j) m[j * 4 + i] = a.r[i][j];
        m[i * 4 + 3] = 0.0f;
        m[12 + i] = a.t[i];
    }
    m[15] = 1.0f;
}

void VrBodyRewriteUpload(ID3D11Resource* resource, void* data) {
    if (!g_enabled || !data || !resource || g_scanState.load() != 2) return;
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || (desc.ByteWidth != 4096 && desc.ByteWidth != 528)) return;
    std::vector<float> copy(desc.ByteWidth / 4);
    std::memcpy(copy.data(), data, copy.size() * 4);  // write-combined: one read
    std::lock_guard lock(g_rigMutex);
    if (g_original.size() != kBoneCount || g_written.size() != kBoneCount || g_ownedBones.size() != kBoneCount) return;
    // Gauntlet draws also use 4 KB uploads. They are separate rigid objects,
    // not Rico's body palette: protecting just their first 64 bytes still
    // leaves every other rigid-looking constant open to the blanket hide.
    // Establish LOCAL ownership from a successfully placed pose or the exact
    // animated bone45 transform, never merely from buffer size/address.
    bool placedGauntlet = false;
    {
        std::lock_guard placedLock(g_placedWeaponMutex);
        placedGauntlet = g_placedGrapple.Contains(copy.data(), GetTickCount64());
    }
    float forearm[16]{};
    const bool haveForearm = g_carryActive.load() && g_carry.bonesValid;
    if (haveForearm) AffineToRowMajor(g_carry.world[45], forearm);
    if (IsLocalGauntletUpload(desc.ByteWidth, copy.data(), placedGauntlet, haveForearm ? forearm : nullptr)) {
        // Preserve existing rigid-placement/optional replacement-hide behavior,
        // but do not interpret the attachment's remaining shader constants as
        // body skinning entries. No tolerance/freshness/IPD changes.
        float rico[16]{};
        const auto character = g_carryCharacter.load();
        if (character && ReadBodyRenderWorld(character,rico) &&
            CarryOrHideMatrix(copy.data(), rico, true, 0.05f))
            std::memcpy(data, copy.data(), 64);
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 6)
            TraceRuntime("native gauntlet upload: body palette/hide bypass eye=" + std::to_string(GetActiveNativeEye()) +
                " ownership=" + (placedGauntlet ? std::string("placed") : std::string("local-bone45")));
        return;
    }
    if (g_rewriteKey != g_applies) {
        g_rewriteGrid.clear();
        for (int sample = 0; sample != 5; ++sample) {
        const auto& pose = sample == 0 ? g_original : g_hidePoseHistory[sample - 1];
        if (sample && (!g_hideAll || pose.size() != kBoneCount)) continue;
        for (int bone = 0; bone != static_cast<int>(kBoneCount); ++bone) {
            if (!g_ownedBones[bone]) continue;
            const auto& e = pose[bone];
            // Identity-like entries (bone 0, unanimated helpers) are shared
            // with every other object's uploads: never match them.
            if (std::abs(e[0] - 1.0f) < 1e-3f && std::abs(e[5] - 1.0f) < 1e-3f && std::abs(e[10] - 1.0f) < 1e-3f &&
                std::abs(e[3]) < 1e-3f && std::abs(e[7]) < 1e-3f && std::abs(e[11]) < 1e-3f) continue;
            g_rewriteGrid.emplace(CellKey(static_cast<int>(std::floor(e[3] / kRewriteCell)), static_cast<int>(std::floor(e[7] / kRewriteCell)),
                                          static_cast<int>(std::floor(e[11] / kRewriteCell))), sample * static_cast<int>(kBoneCount) + bone);
        }
        }
        g_rewriteKey = g_applies;
    }
    // Riders uploaded as absolute world matrices (live: the grapple gauntlet
    // at +0x0 of a 4 KB buffer, exactly on LeftForeArmRoll).
    UploadMatrixOwnership riderOwnership;
    if (g_carryActive.load()) {
        const std::uintptr_t character = g_carryCharacter.load();
        float rico[16]{};
        if (character && ReadBodyRenderWorld(character,rico)) {
            for (std::size_t i = 0; i + 16 <= copy.size(); i += 4) {
                float* m = copy.data() + i;
                if (std::abs(m[15] - 1.0f) > 1e-4f) continue;
                bool owned = false;
                const bool changed = CarryOrHideMatrix(m, rico, true, 0.05f, &owned);
                if (changed || owned) {
                    riderOwnership.Protect(i * 4);
                    if (changed) std::memcpy(static_cast<std::uint8_t*>(data) + i * 4, m, 64);
                    i += 12;
                }
            }
        }
    }
    if (g_rewriteGrid.empty()) return;
    std::vector<std::pair<std::size_t, int>> hits;
    for (std::size_t i = 0; i + 12 <= copy.size(); i += 4) {
        if (riderOwnership.Overlaps(i * 4)) continue;
        const float* m = copy.data() + i;
        if (!FiniteMatrix(m, 12)) continue;
        // Rigid 3x4 rows (skinning entries are rotations here).
        const float n0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2], n1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6];
        if (std::abs(n0 - 1.0f) > 0.02f || std::abs(n1 - 1.0f) > 0.02f) continue;
        const int cx = static_cast<int>(std::floor(m[3] / kRewriteCell)), cy = static_cast<int>(std::floor(m[7] / kRewriteCell)), cz = static_cast<int>(std::floor(m[11] / kRewriteCell));
        int best = -1;
        float bestError = 1e9f;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto range = g_rewriteGrid.equal_range(CellKey(cx + dx, cy + dy, cz + dz));
                    for (auto it = range.first; it != range.second; ++it) {
                        const int sample = it->second / static_cast<int>(kBoneCount);
                        const int bone = it->second % static_cast<int>(kBoneCount);
                        const auto& e = sample == 0 ? g_original[bone] : g_hidePoseHistory[sample - 1][bone];
                        const float tx = m[3] - e[3], ty = m[7] - e[7], tz = m[11] - e[11];
                        if (tx * tx + ty * ty + tz * tz > kRewriteTranslation * kRewriteTranslation) continue;
                        float rotation = 0.0f;
                        for (int k : {0, 1, 2, 4, 5, 6, 8, 9, 10}) rotation = std::max(rotation, std::abs(m[k] - e[k]));
                        if (rotation > kRewriteRotation) continue;
                        const float error = tx * tx + ty * ty + tz * tz + rotation * rotation;
                        if (error < bestError) { bestError = error; best = bone; }
                    }
                }
        if (best >= 0) { hits.emplace_back(i * 4, best); i += 8; }
    }
    if (hits.size() < 3) return;
    auto* bytes = static_cast<std::uint8_t*>(data);
    for (const auto& [offset, bone] : hits) {
        std::memcpy(bytes + offset, g_written[bone].data(), 48);
        if (g_ricoVisibleBones[bone]) riderOwnership.Protect(offset, 48);
    }
    if (g_hideAll) {
        // This upload is Rico's: hide every bone entry in it, matched or not
        // (live 2026-09-30: unmatched entries left a crumpled pile of gear).
        const auto& collapsed = g_written[2];
        for (std::size_t i = 0; i + 12 <= copy.size(); i += 4) {
            if (riderOwnership.Overlaps(i * 4)) continue;
            const float* m = copy.data() + i;
            if (!FiniteMatrix(m, 12)) continue;
            const float n0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2], n1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6], n2 = m[8] * m[8] + m[9] * m[9] + m[10] * m[10];
            if (std::abs(n0 - 1.0f) > 0.05f || std::abs(n1 - 1.0f) > 0.05f || std::abs(n2 - 1.0f) > 0.05f) continue;
            if (std::abs(m[3]) > 3.0f || std::abs(m[7]) > 3.0f || std::abs(m[11]) > 3.0f) continue;
            std::memcpy(bytes + i * 4, collapsed.data(), 48);
            i += 8;
        }
    }
    g_rewritten += hits.size();
    ++g_rewriteUploads;
}
void ConfigureVrBodyHideAll(bool hideAll) {
    g_hideAll = hideAll;
    TraceRuntime(std::string("VR body: whole body hidden (controllers + carried gun): ") + (hideAll ? "yes" : "no"));
}

void ConfigureRicoNativeHands(bool enabled, const std::filesystem::path& directory,
                             const std::array<RicoHandTuning, 2>& tuning) {
    if (!enabled) return;
    std::call_once(g_ricoHandsStartup, [&] {
        constexpr std::size_t sizes[]{2011645, 1096023, 221490};
        constexpr const char* hashes[]{
            "0826eea198001f43915cff867e4845ff112e61158982516a1b68c37a53b771f4",
            "95e274ecd06ee73e85869da1e238036db069f7b3dbc937305c606716058b9caa",
            "65172aa2097fc23319aae3b2e41ffcb7912451333ece83e1f576f911bb5463f1"};
        const auto assets = directory / L"dropzone/models/jc_characters/main_characters/rico";
        for (int lod = 0; lod != 3; ++lod) {
            const auto name = L"rico_body_lod" + std::to_wstring(lod + 1) + L".rbm";
            if (!FileMatchesSha256(assets / name, sizes[lod], hashes[lod])) {
                TraceRuntime("Rico hands: missing or mismatched local LOD" + std::to_string(lod + 1));
                return;
            }
        }
        g_ricoHandTuning = tuning;
        g_ricoNativeHands.store(true);
        TraceRuntime("Rico hands: all three local hand-only LOD hashes matched; gameplay rendering pending");
    });
}

bool RicoNativeHandsEnabled() { return g_ricoNativeHands.load(); }

bool RicoNativeHandPrepared(int side) {
    if (!RicoNativeHandsEnabled() || side < 0 || side > 1 || !VrBodyActive()) return false;
    const auto tick = g_ricoHandPrepared[side].load();
    return tick && GetTickCount64() - tick <= 150;
}

void ConfigureVrBodyCarry(bool enabled) {
    g_carryEnabled = enabled;
    g_carryActive.store(enabled);
    TraceRuntime(std::string("VR body: held objects follow the IK hands: ") + (enabled ? "on" : "off"));
}

// An absolute rigid object matrix (row-major 4x4) riding one of Rico's bones:
// on an arm, it follows the controller-driven arm (the gun, the grapple
// gauntlet; with the body hidden, forearm riders follow the hand); anywhere
// else, with the body hidden, it is collapsed (hair, eyes, gear: live
// 2026-09-30 they floated in one eye). Returns true if m was changed.
// Caller holds g_rigMutex.
static bool CarryOrHideMatrix(float* m, const float* rico, bool requireRotation, float radius, bool* owned) {
    if (owned) *owned = false;
    if (!FiniteMatrix(m, 16) || !FiniteMatrix(rico, 16)) return false;
    if (g_trackedGauntletReady.load()) {
        std::lock_guard placedLock(g_placedWeaponMutex);
        if (g_placedGrapple.Contains(m, GetTickCount64())) {
            if (owned) *owned = true;
            // Hide ONLY a successfully placed local gauntlet, not the gun,
            // NPC equipment or cable. Late XR hand mesh replaces its visual.
            for (int axis = 0; axis != 3; ++axis)
                for (int component = 0; component != 3; ++component) m[axis*4+component] *= 0.0001f;
            return true;
        }
    }
    bool lateChanged=false;
    if (IsAlreadyPlacedWeapon(m,lateChanged)) {
        if (owned) *owned = true;
        return lateChanged;
    }
    if (std::abs(m[3]) > 1e-4f || std::abs(m[7]) > 1e-4f || std::abs(m[11]) > 1e-4f || std::abs(m[15] - 1.0f) > 1e-4f) return false;
    const float n0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2], n1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6], n2 = m[8] * m[8] + m[9] * m[9] + m[10] * m[10];
    if (std::abs(n0 - 1.0f) > 0.02f || std::abs(n1 - 1.0f) > 0.02f || std::abs(n2 - 1.0f) > 0.02f) return false;
    // Cheap reject: only objects within reach of Rico; never Rico himself.
    const float rx = m[12] - rico[12], ry = m[13] - rico[13], rz = m[14] - rico[14];
    const float fromRico = rx * rx + ry * ry + rz * rz;
    if (fromRico > 2.5f * 2.5f || fromRico < 1e-6f) return false;
    if (!g_carry.bonesValid || g_rig.armSegment.size() != kBoneCount) return false;
    int nearest = -1;
    float best = radius * radius;
    for (int bone = 0; bone != static_cast<int>(kBoneCount); ++bone) {
        const Affine& w = g_carry.world[bone];
        const float dx = m[12] - w.t[0], dy = m[13] - w.t[1], dz = m[14] - w.t[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d >= best) continue;
        if (requireRotation) {
            float trace = 0.0f;
            for (int i = 0; i != 3; ++i)
                for (int j = 0; j != 3; ++j) trace += w.r[i][j] * m[j * 4 + i];
            if ((trace - 1.0f) * 0.5f < 0.98f) continue;
        }
        best = d;
        nearest = bone;
    }
    if (nearest < 0) { ++g_carryMisses; return false; }
    int segment = g_rig.armSegment[nearest];
    if (g_gunsPlacedInGame.load() && (nearest == 26 || nearest == 44 || nearest == 66 || nearest == 84)) return false;
    if (segment >= 0 && g_carry.valid[segment / 3]) {
        if (g_hideAll && segment % 3 == 1) segment += 1;  // forearm riders follow the hand
        const Affine& c = g_carry.segment[segment / 3][segment % 3];
        float out[16];
        std::memcpy(out, m, sizeof(out));
        for (int i = 0; i != 3; ++i) {
            for (int j = 0; j != 3; ++j) out[j * 4 + i] = c.r[i][0] * m[j * 4 + 0] + c.r[i][1] * m[j * 4 + 1] + c.r[i][2] * m[j * 4 + 2];
            out[12 + i] = c.r[i][0] * m[12] + c.r[i][1] * m[13] + c.r[i][2] * m[14] + c.t[i];
        }
        // Only objects on the hand-attach bones (the guns: 26/44 left, 66/84 right).
        if (segment % 3 == 2 && (nearest == 26 || nearest == 44 || nearest == 66 || nearest == 84)) {
            LastCarried& last = g_lastCarried[segment / 3];
            std::memcpy(last.original, m, sizeof(last.original));
            std::memcpy(last.carried, out, sizeof(last.carried));
            last.bone = nearest;
            last.tick = GetTickCount64();
        }
        std::memcpy(m, out, sizeof(out));
        ++g_carried;
        return true;
    }
    if (!g_hideAll) return false;
    {
        // Gear rides its bone rigidly (bone-local mesh): same rotation.
        const Affine& w = g_carry.world[nearest];
        float trace = 0.0f;
        for (int i = 0; i != 3; ++i)
            for (int j = 0; j != 3; ++j) trace += w.r[i][j] * m[j * 4 + i];
        if ((trace - 1.0f) * 0.5f < 0.98f) return false;
    }
    for (int k : {0, 1, 2, 4, 5, 6, 8, 9, 10}) m[k] = 0.0f;
    ++g_hiddenRiders;
    return true;
}

void VrBodyCarryUpload(ID3D11Resource* resource, void* data) {
    if(g_scopeProbeWeapon.load()) {std::lock_guard lock(g_scopeBufferMutex);g_scopeBuffers.erase(resource);}
    if (!g_enabled || !g_carryActive.load() || !data || !resource || g_scanState.load() != 2) return;
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
    const std::size_t* offsets = nullptr;
    std::size_t offsetCount = 0;
    for (const auto& signature : kCarrySignatures) {
        if (signature.size == desc.ByteWidth) { offsets = signature.offsets; offsetCount = signature.count; break; }
    }
    if (!offsets) return;
    const std::uintptr_t character = g_carryCharacter.load();
    float rico[16]{};
    if (!character || !ReadBodyRenderWorld(character,rico)) return;
    auto* bytes = static_cast<std::uint8_t*>(data);
    for (std::size_t index = 0; index != offsetCount; ++index) {
        const std::size_t offset = offsets[index];
        if (offset + 64 > desc.ByteWidth) continue;
        float m[16];
        std::memcpy(m, bytes + offset, sizeof(m));  // write-combined: one read
        TagScopeBuffer(resource,m);
        bool changed = false;
        {
            std::lock_guard lock(g_rigMutex);
            // Held objects sit up to ~7 cm from their bone at an offset
            // rotation (live: 6.6 cm from LeftHandAttach2); gear elsewhere
            // must also match the bone's rotation.
            changed = CarryOrHideMatrix(m, rico, false, kCarryRadius);
        }
        if (changed) std::memcpy(bytes + offset, m, sizeof(m));
    }
}
void ConfigureVrBody(bool enabled, bool debugPose, bool hideHead) {
    g_enabled = enabled;
    g_debugPose = debugPose;
    g_hideHead = hideHead;
    TraceRuntime(std::string("VR body (arms follow controllers, render-side IK): ") + (enabled ? "on" : "off") + (debugPose ? " [debug pose: both hands raised]" : "") + (hideHead ? " [head hidden at the neck]" : ""));
}
void BeginScopeSourceProbe(std::uintptr_t sniper) {g_scopeProbeWeapon=sniper;}
bool ScopeSourceProbeActive() {return g_scopeProbeWeapon.load()!=0;}
bool IsScopeSourceBuffer(ID3D11Resource* resource) {
    std::lock_guard lock(g_scopeBufferMutex);const auto it=g_scopeBuffers.find(resource);
    return it!=g_scopeBuffers.end() && GetTickCount64()-it->second<100;
}
void EndScopeSourceProbe() {g_scopeProbeWeapon=0;std::lock_guard lock(g_scopeBufferMutex);g_scopeBuffers.clear();}

void VrBodyPresentTick() {
    if (!g_enabled) return;
    static bool rootF3Was=false;
    const bool rootF3=(GetAsyncKeyState(VK_F3)&0x8000)!=0;
    if(rootF3 && !rootF3Was)ReportRenderCharacterRoot();
    rootF3Was=rootF3;
    if (!g_imageBase) g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    static std::uint64_t presents = 0;
    ++presents;
    const int state = g_scanState.load();
    // Discover as soon as the gameplay camera has been live for ~1 s (live
    // 22:47: a 30 s retry left the arms unbound for ~25 s after entering the
    // world), then retry every ~10 s while not found or invalid.
    static std::uint64_t retryAt = 900;
    static int gameplayPresents = 0;
    gameplayPresents = HasNativeGameplayCamera() ? gameplayPresents + 1 : 0;
    if ((state == 0 || state == 3) && (presents >= retryAt || gameplayPresents == 90)) {
        retryAt = presents + 900;
        g_scanState.store(1);
        std::thread(Discover).detach();
    }
    if (state == 2) {
        std::lock_guard lock(g_rigMutex);
        if (!IsRico(g_rig.character) || Palette(g_rig.character) != g_rig.palette) {
            TraceRuntime("VR body: Rico/palette no longer valid; rediscovering");
            g_rig = {};
            g_scanState.store(3);
            retryAt = presents + 90;
        }
    }
    if (presents % 2700 == 0)
        TraceRuntime("VR body: applies=" + std::to_string(g_applies) + " reapplied_after_game_overwrite=" + std::to_string(g_reapplies) +
            " invalid=" + std::to_string(g_invalid) + " state=" + std::to_string(state) + " carried=" + std::to_string(g_carried) +
            " carry_candidates_not_at_hand=" + std::to_string(g_carryMisses) + " carry=" + (g_carryActive.load() ? "on" : "off") + " upload_rewrites=" + std::to_string(g_rewriteUploads) + "/" + std::to_string(g_rewritten) + " hidden_riders=" + std::to_string(g_hiddenRiders) +
            " placed_weapon_bypasses=" + std::to_string(g_placedWeaponBypasses.load()));
    // Upload diagnostic for ~10 s once the rig is ready.
    static bool diagArmed = false;
    if (state == 2 && !diagArmed && g_debugPose) { diagArmed = true; g_diagFramesLeft.store(600); }
    if (g_diagFramesLeft.load() > 0) g_diagFramesLeft.fetch_sub(1);
    // Parity probe: one line per Present (the counts cover the frame just
    // presented), then reset.
    if (g_parityFramesLeft.load() > 0) {
        TraceRuntime("VR body parity: present=" + std::to_string(presents) + " parity=" + std::to_string(presents & 1) +
            " rico_uploads(4K/528)=" + std::to_string(g_parityUploads.exchange(0)) + " with_JC3VR_hand=" + std::to_string(g_parityOurs.exchange(0)) +
            " with_game_hand=" + std::to_string(g_parityOriginal.exchange(0)) + " reapplies_total=" + std::to_string(g_reapplies));
        g_parityFramesLeft.fetch_sub(1);
    }
    // On-demand attachment probe (trigger file), ~20 frames per request.
    if (g_attachProbeFramesLeft.load() > 0) g_attachProbeFramesLeft.fetch_sub(1);
    if (state == 2 && presents % 30 == 0) {
        static std::wstring trigger = [] {
            wchar_t base[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
            return length && length < MAX_PATH ? std::wstring(base) + L"\\JC3VR\\jc3vr_attach_probe" : std::wstring();
        }();
        // A/B for unattended 2D checks: %LOCALAPPDATA%\JC3VR\jc3vr_carry_toggle.
        static std::wstring toggle = trigger.empty() ? std::wstring() : trigger.substr(0, trigger.size() - std::wcslen(L"jc3vr_attach_probe")) + L"jc3vr_carry_toggle";
        if (g_carryEnabled && !toggle.empty() && GetFileAttributesW(toggle.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(toggle.c_str());
            g_carryActive.store(!g_carryActive.load());
            TraceRuntime(std::string("VR body: carry toggled ") + (g_carryActive.load() ? "on" : "off") + " carried=" + std::to_string(g_carried));
        }
        static std::wstring parityTrigger = toggle.empty() ? std::wstring() : toggle.substr(0, toggle.size() - std::wcslen(L"jc3vr_carry_toggle")) + L"jc3vr_parity_probe";
        if (!parityTrigger.empty() && GetFileAttributesW(parityTrigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(parityTrigger.c_str());
            g_parityOurs.store(0);
            g_parityOriginal.store(0);
            g_parityUploads.store(0);
            g_parityFramesLeft.store(60);
            TraceRuntime("VR body parity probe: armed for 60 presents");
        }
        static std::wstring alternate =toggle.empty() ? std::wstring() : toggle.substr(0, toggle.size() - std::wcslen(L"jc3vr_carry_toggle")) + L"jc3vr_debug_pose_alt";
        if (g_debugPose && !alternate.empty() && GetFileAttributesW(alternate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(alternate.c_str());
            g_debugAlternate.store(!g_debugAlternate.load());
            TraceRuntime(std::string("VR body: debug pose right hand ") + (g_debugAlternate.load() ? "low/outward" : "raised"));
        }
        if (!trigger.empty() && GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(trigger.c_str());
            {
                std::lock_guard lock(g_attachProbeMutex);
                g_attachSeen.clear();
            }
            g_attachProbeLines.store(0);
            g_attachProbeFramesLeft.store(20);
            float world[16]{};
            float camera[3]{};
            const bool haveCamera = GetEngineCameraPosition(camera);
            {
                std::lock_guard lock(g_rigMutex);
                SafeCopy(world, reinterpret_cast<const void*>(g_rig.character + kWorldMatrixOffset), sizeof(world));
            }
            std::ostringstream line;
            line << "VR body attach probe: armed rico=(" << world[12] << "," << world[13] << "," << world[14] << ") camera=";
            if (haveCamera) line << "(" << camera[0] << "," << camera[1] << "," << camera[2] << ")"; else line << "n/a";
            TraceRuntime(line.str());
        }
    }
    g_appliedThisFrame.store(false);
}

void SetVrBodyHandTarget(int hand, bool valid, const float grip[3], const float fingers[3], const float down[3],
                         const float* cameraOrigin, bool firstPersonAnchor, RicoFingerInput fingersInput) {
    if (hand < 0 || hand > 1) return;
    std::lock_guard lock(g_targetMutex);
    g_targetValid[hand] = valid;
    g_targetTick[hand] = GetTickCount64();
    g_targetCameraValid[hand] = false;
    if (!valid) {
        if (RicoNativeHandsEnabled()) g_ricoHandPrepared[hand] = 0;
        return;
    }
    if (RicoNativeHandsEnabled()) g_targetFingersInput[hand] = fingersInput;
    if (cameraOrigin && FiniteMatrix(cameraOrigin, 3)) {
        std::copy(cameraOrigin, cameraOrigin + 3, g_targetCamera[hand]);
        g_targetCameraValid[hand] = true;
        g_targetFirstPersonAnchor[hand] = firstPersonAnchor;
    }
    // Keep the tracking sample relative to the actor movement at publication.
    // Both physics placement and render-side IK rebase it at consumption.
    const auto character=g_carryCharacter.load();
    float root[16]{};
    g_targetCharacter[hand]=0;
    if (character && GetCharacterRenderPosition(character,root)) {
        std::copy(root,root+3,g_targetRoot[hand]);
        g_targetCharacter[hand]=character;
    }
    std::copy(grip, grip + 3, g_targetWorld[hand]);
    std::copy(fingers, fingers + 3, g_targetFingers[hand]);
    std::copy(down, down + 3, g_targetDown[hand]);
}

bool GetVrBodyLeftWristMount(int bone, float world[16]) {
    if ((bone != 25 && bone != 45) || !g_enabled || !g_carryActive.load() || !VrBodyActive()) return false;
    float grip[3], fingers[3], down[3];
    if (!GetVrBodyHandTarget(0,grip,fingers,down)) return false;
    std::lock_guard lock(g_rigMutex);
    return BuildLeftWristMountLocked(bone, grip, fingers, down, world);
}

static bool BuildLeftWristMountLocked(int bone, const float grip[3], const float fingers[3], const float down[3], float world[16]) {
    if (bone != 25 && bone != 45) return false;
    if (g_rig.rest.size() != kBoneCount || g_rig.restQ.size() != kBoneCount) return false;
    const BodyVec3 wrist = g_rig.rest[25];
    const float restWrist[3]{wrist.x, wrist.y, wrist.z};
    const auto middle = g_rig.rest[33], index = g_rig.rest[30], pinky = g_rig.rest[41];
    const float restForward[3]{middle.x-wrist.x, middle.y-wrist.y, middle.z-wrist.z};
    const float restDown[3]{pinky.x-index.x, pinky.y-index.y, pinky.z-index.z};
    Affine rest = QuatToAffine(g_rig.restQ[bone]);
    rest.t[0] = g_rig.rest[bone].x; rest.t[1] = g_rig.rest[bone].y; rest.t[2] = g_rig.rest[bone].z;
    float restBone[16]; AffineToRowMajor(rest, restBone);
    return BuildWristAttachment(restWrist, restForward, restDown, restBone, grip, fingers, down, world);
}

void SetVrBodyGunsPlacedInGame(bool placed) { g_gunsPlacedInGame.store(placed); }

void RegisterVrBodyPlacedWeapon(std::uintptr_t weapon, const float matrix[16], int hand) {
    if (!weapon) return;
    float grip[3]{},fingers[3],down[3];
    if(hand>=0 && !GetVrBodyHandTarget(hand,grip,fingers,down)) hand=-1;
    std::lock_guard lock(g_placedWeaponMutex);
    PlacedWeapon* destination = &g_placedWeapons[0];
    for (auto& entry : g_placedWeapons) {
        if (entry.object == weapon && MatchesPlacedWeapon(entry.matrix,matrix)) { destination = &entry; break; }
        if (entry.tick < destination->tick) destination = &entry;
    }
    destination->object = weapon;
    destination->hand=hand;std::copy(grip,grip+3,destination->grip);
    std::memcpy(destination->matrix, matrix, sizeof(destination->matrix));
    destination->tick = GetTickCount64();
}

void RegisterVrBodyPlacedGrapple(const float matrix[16]) {
    std::lock_guard lock(g_placedWeaponMutex);
    g_placedGrapple.Publish(matrix, GetTickCount64());
}
void SetVrBodyTrackedGauntletReady(bool ready) { g_trackedGauntletReady.store(ready); }

bool GetVrBodyAttachMounts(float game[4][16], float carried[4][16], bool valid[4], unsigned long long& tick) {
    std::lock_guard lock(g_attachMutex);
    std::memcpy(game, g_attach.game, sizeof(g_attach.game));
    std::memcpy(carried, g_attach.carried, sizeof(g_attach.carried));
    std::memcpy(valid, g_attach.valid, sizeof(g_attach.valid));
    tick = g_attach.tick;
    return tick != 0;
}

bool GetVrBodyLastCarried(int hand, float original[16], float carried[16], int& bone, unsigned long long& tick) {
    if (hand < 0 || hand > 1) return false;
    std::lock_guard lock(g_rigMutex);
    const LastCarried& last = g_lastCarried[hand];
    if (!last.tick) return false;
    std::memcpy(original, last.original, sizeof(last.original));
    std::memcpy(carried, last.carried, sizeof(last.carried));
    bone = last.bone;
    tick = last.tick;
    return true;
}

static bool GetHandTarget(int hand, float grip[3], float fingers[3], float down[3],bool render,
                          RicoFingerInput* fingerInput = nullptr) {
    if (hand < 0 || hand > 1) return false;
    std::lock_guard lock(g_targetMutex);
    if (!g_targetValid[hand] || GetTickCount64()-g_targetTick[hand]>200) return false;
    std::copy(g_targetWorld[hand], g_targetWorld[hand] + 3, grip);
    if (g_targetCharacter[hand] && g_targetCharacter[hand] != g_carryCharacter.load()) return false;
    if (render && g_targetCameraValid[hand]) {
        // The target was formed from THIS camera origin, not a separately
        // sampled actor root. Keep its offset relative to the rendered camera
        // even when simulation roots/interpolation advance at different times.
        float camera[3]{};
        if (g_targetFirstPersonAnchor[hand]) {
            const auto anchor = GetFirstPersonCameraAnchor(true);
            if (!anchor.valid) return false;
            std::copy(anchor.position, anchor.position + 3, camera);
        } else if (!GetEngineCameraPosition(camera)) return false;
        if (!RebaseTrackedPoint(g_targetWorld[hand], g_targetCamera[hand], camera, grip)) return false;
        static std::atomic<bool> traced{false};
        if (!traced.exchange(true)) TraceRuntime("hand tracking: render target rebased from its exact sampled camera origin; actor correction not applied twice");
    } else
    if (g_targetCharacter[hand]) {
        if (g_targetCharacter[hand]!=g_carryCharacter.load()) return false;
        float root[16]{};
        if(render) {
            if(!GetCharacterRenderPosition(g_targetCharacter[hand],root+12)) return false;
        } else if(!SafeCopy(root,reinterpret_cast<const void*>(g_targetCharacter[hand]+0x2828),sizeof(root)) || std::abs(root[15]-1)>0.001f) return false;
        if(!RebaseTrackedPoint(g_targetWorld[hand],g_targetRoot[hand],root+12,grip)) return false;
        static std::atomic<bool> movementTraced{false};
        float distance=0; for(int i=0;i<3;++i) {const float d=root[12+i]-g_targetRoot[hand][i];distance+=d*d;}
        if(distance>0.001f*0.001f && !movementTraced.exchange(true))
            TraceRuntime("movement anchor: tracked hand rebased using measured Rico T1 displacement; no velocity prediction");
    }
    std::copy(g_targetFingers[hand], g_targetFingers[hand] + 3, fingers);
    std::copy(g_targetDown[hand], g_targetDown[hand] + 3, down);
    if (fingerInput) *fingerInput = g_targetFingersInput[hand];
    return true;
}

bool VrBodyActive() { return g_enabled && g_scanState.load() == 2; }
bool GetVrBodyHandTarget(int hand,float grip[3],float fingers[3],float down[3]) {return GetHandTarget(hand,grip,fingers,down,false);}
bool GetVrBodyRenderHandTarget(int hand,float grip[3],float fingers[3],float down[3]) {return GetHandTarget(hand,grip,fingers,down,true);}

bool VrBodyHeadHideEnabled() { return g_enabled && (g_hideHead || g_hideAll); }

std::size_t GetVrBodyHeadBones(std::uint32_t* bones, std::size_t capacity) {
    if (!bones || !VrBodyActive() || !VrBodyHeadHideEnabled()) return 0;
    std::lock_guard lock(g_rigMutex);
    if (g_rig.subtree.size() != kBoneCount || g_rig.subtree[kHeadBone].size() > capacity) return 0;
    std::size_t count = 0;
    for (const int bone : g_rig.subtree[kHeadBone]) bones[count++] = static_cast<std::uint32_t>(bone);
    return count;
}

std::uintptr_t VrBodyCharacter() { return VrBodyActive() ? g_rig.character : 0; }

namespace {
constexpr int kRicoVisualHandBones = 19; // wrist through last finger, excludes Attach2
using RicoHandPalette = std::array<std::array<float, 12>, kRicoVisualHandBones>;

Affine RotateAbout(BodyVec3 axis, float radians, BodyVec3 pivot) {
    const float sinHalf = std::sin(radians * 0.5f);
    auto rotation = QuatToAffine({axis.x * sinHalf, axis.y * sinHalf, axis.z * sinHalf, std::cos(radians * 0.5f)});
    const auto turned = Apply(rotation, pivot);
    rotation.t[0] = pivot.x - turned.x;
    rotation.t[1] = pivot.y - turned.y;
    rotation.t[2] = pivot.z - turned.z;
    return rotation;
}

bool BuildRicoVisualHand(int side, int base, const Affine& trackedWrist, BodyVec3 wrist,
                         BodyVec3 fingers, BodyVec3 down, const RicoFingerInput& input,
                         const std::vector<std::array<float, 12>>& current, RicoHandPalette& result) {
    const auto firstBone = g_rig.armSegment.begin() + base;
    // Attach (base+1) has no retained skin weights and may be parented
    // separately. Validate the wrist and the actual finger chains.
    if (*firstBone != side * 3 + 2 ||
        !std::all_of(firstBone + 2, firstBone + kRicoVisualHandBones, [side](int segment) {
        return segment == side * 3 + 2;
    })) return false;

    const float along[]{fingers.x, fingers.y, fingers.z};
    const float below[]{down.x, down.y, down.z};
    float frame[16]{};
    if (!AlignWeaponToController(along, below, frame)) return false;
    const BodyVec3 axes[]{{frame[0], frame[1], frame[2]}, {frame[4], frame[5], frame[6]},
                          {frame[8], frame[9], frame[10]}};
    const auto& tuning = g_ricoHandTuning[side];
    constexpr float radians = 0.01745329252f;
    Affine adjust = Compose(RotateAbout(axes[2], tuning.degrees[2] * radians, wrist),
        Compose(RotateAbout(axes[1], tuning.degrees[1] * radians, wrist),
                RotateAbout(axes[0], tuning.degrees[0] * radians, wrist)));
    for (int axis = 0; axis != 3; ++axis)
        for (int c = 0; c != 3; ++c)
            adjust.t[c] += frame[axis * 4 + c] * tuning.offset[axis];
    const Affine tracked = Compose(adjust, trackedWrist);
    const auto flexAxis = Apply(adjust, axes[1]);
    const BodyVec3 hinge{flexAxis.x - adjust.t[0], flexAxis.y - adjust.t[1], flexAxis.z - adjust.t[2]};
    std::array<Affine, kRicoVisualHandBones> posed{};
    for (int i = 0; i != kRicoVisualHandBones; ++i) {
        const int bone = base + i;
        posed[i] = Compose(tracked, FromPalette(current[bone].data()));
        if (input.unarmed && i >= 2) {
            const int parent = g_rig.parents[bone] - base;
            if (parent < 0 || parent >= i) return false;
            posed[i] = posed[parent];
            const auto pivot = Apply(posed[i], g_rig.rest[bone]);
            const float curl = (side ? 1.0f : -1.0f) * RicoFingerFlex(kLeftArm[2] + i, input);
            posed[i] = Compose(RotateAbout(hinge, curl, pivot), posed[i]);
        }
        ToPalette(posed[i], result[i].data());
        if (!FiniteMatrix(result[i].data(), 12)) return false;
    }
    return true;
}
}

void VrBodyApplyAtUpload() {
    if (!g_enabled || g_scanState.load() != 2) return;
    std::lock_guard lock(g_rigMutex);
    if (!g_rig.palette || g_rig.rest.size() != kBoneCount) return;
    auto* data = reinterpret_cast<float*>(g_rig.palette + 0x50);
    // Fast path (runs at every write-Map): this frame's result is still in
    // the palette, so the draw about to be prepared will upload it.
    if (g_written.size() == kBoneCount && g_appliedThisFrame.load()) {
        // Sentinels: the game rewrites the whole palette at once, so the two
        // hands and the head (if hidden) tell whether our result survived.
        // (Checking every arm bone on every Map cost ~90% of the frame rate.)
        std::array<float, 12> live{};
        bool intact = true;
        for (const int bone : {kLeftArm[2], kRightArm[2], kHeadBone}) {
            if (bone == kHeadBone && !g_hideHead) continue;
            if (!SafeCopy(live.data(), data + bone * 12, 48) || std::memcmp(live.data(), g_written[bone].data(), 48) != 0) { intact = false; break; }
        }
        if (intact) return;
        ++g_reapplies;
    }
    std::vector<std::array<float, 12>> current(kBoneCount);
    if (!SafeCopy(current.data(), data, kBoneCount * 48)) { ++g_invalid; return; }
    // Palette entries JC3VR owns: the arm (and head) subtrees, or every bone
    // when the whole body is hidden.
    std::vector<int> owned;
    if (g_hideAll) for (int bone = 0; bone != static_cast<int>(kBoneCount); ++bone) owned.push_back(bone);
    else
        for (const int root : {kLeftArm[0], kRightArm[0], kHeadBone})
            if (root != kHeadBone || g_hideHead) owned.insert(owned.end(), g_rig.subtree[root].begin(), g_rig.subtree[root].end());
    // If the game has not refreshed the arms since our last write, restore
    // its original values first so the IK never compounds.
    if (g_written.size() == kBoneCount && g_original.size() == kBoneCount) {
        bool unchanged = true;
        for (const int bone : owned)
            if (std::memcmp(current[bone].data(), g_written[bone].data(), 48) != 0) unchanged = false;
        // Globals is uploaded dozens of times per frame; once this frame's
        // result is in place and the game has not refreshed it, skip.
        if (unchanged && g_appliedThisFrame.load()) return;
        if (unchanged)
            for (const int bone : owned) current[bone] = g_original[bone];
    }
    if (g_hidePoseCharacter != g_rig.character) {
        for (auto& pose : g_hidePoseHistory) pose.clear();
        g_hidePoseCursor = 0;
        g_hidePoseCharacter = g_rig.character;
    }
    if (g_hideAll && g_original.size() == kBoneCount && current != g_original) {
        g_hidePoseHistory[g_hidePoseCursor] = g_original;
        g_hidePoseCursor = (g_hidePoseCursor + 1) % g_hidePoseHistory.size();
    }
    g_original = current;
    std::vector<std::array<float, 12>> updated = current;
    bool targetValid[2]{};
    float targetWorld[2][3]{}, targetFingers[2][3]{}, targetDown[2][3]{};
    const bool nativeHands = RicoNativeHandsEnabled();
    RicoFingerInput fingerInput[2]{};
    for (int side=0;side<2;++side)
        targetValid[side]=GetHandTarget(side,targetWorld[side],targetFingers[side],targetDown[side],true,
                                       nativeHands ? &fingerInput[side] : nullptr);
    // Model axes: +X right, +Y up, -Z forward (rest pose).
    const BodyVec3 right{1.0f, 0.0f, 0.0f}, forward{0.0f, 0.0f, -1.0f};
    bool wrote = false;
    // Model <-> world (column-vector) from the row-major world transform.
    float world[16]{};
    const bool haveWorld = ReadBodyRenderWorld(g_rig.character,world) && std::abs(world[15] - 1.0f) < 1e-3f;
    Affine toWorld{}, toModel{};
    for (int a = 0; a != 3; ++a) {
        for (int k = 0; k != 3; ++k) {
            toWorld.r[a][k] = world[k * 4 + a];
            toModel.r[k][a] = world[k * 4 + a];
        }
        toWorld.t[a] = world[12 + a];
    }
    for (int k = 0; k != 3; ++k) toModel.t[k] = -(toModel.r[k][0] * world[12] + toModel.r[k][1] * world[13] + toModel.r[k][2] * world[14]);
    g_carry.valid[0] = g_carry.valid[1] = false;
    g_carry.bonesValid = haveWorld;
    if (haveWorld) {
        for (int bone = kArmBoneFirst; bone != kArmBoneEnd; ++bone)
            g_carry.bones[bone - kArmBoneFirst] = Apply(toWorld, Apply(FromPalette(current[bone].data()), g_rig.rest[bone]));
        for (int bone = 0; bone != static_cast<int>(kBoneCount); ++bone) {
            Affine rest = QuatToAffine(g_rig.restQ[bone]);
            rest.t[0] = g_rig.rest[bone].x;
            rest.t[1] = g_rig.rest[bone].y;
            rest.t[2] = g_rig.rest[bone].z;
            g_carry.world[bone] = Compose(toWorld, Compose(FromPalette(current[bone].data()), rest));
        }
        g_carryCharacter.store(g_rig.character);
    }
    Affine handMotionModel[2]{};
    bool handMotionValid[2]{};
    RicoHandPalette visibleHands[2];
    bool visibleHand[2]{};
    for (int side = 0; side != 2; ++side) {
        const int* chain = side == 0 ? kLeftArm : kRightArm;
        const int* fingers = side == 0 ? kLeftFingers : kRightFingers;
        BodyVec3 palm{}, fingerDir{0.0f, 0.0f, -1.0f}, downDir{0.0f, -1.0f, 0.0f};
        bool orient = false;
        // Debug pose: both hands raised; the alternate (trigger file) drops the
        // right hand low and outward, away from where the game's own aim
        // animation holds the gun, so a carried gun is distinguishable in 2D.
        if (g_debugPose) palm = side == 0 ? BodyVec3{-0.18f, 1.50f, -0.40f} : g_debugAlternate.load() ? BodyVec3{0.34f, 1.18f, -0.42f} : BodyVec3{0.18f, 1.50f, -0.40f};
        else {
            if (!targetValid[side] || !WorldToModel(g_rig.character, targetWorld[side], palm)) continue;
            orient = WorldDirectionToModel(g_rig.character, targetFingers[side], fingerDir) &&
                     WorldDirectionToModel(g_rig.character, targetDown[side], downDir);
        }
        // The wrist sits behind the palm along the finger direction.
        const BodyVec3 target = orient ? BodyVec3{palm.x - fingerDir.x * kWristToPalm, palm.y - fingerDir.y * kWristToPalm, palm.z - fingerDir.z * kWristToPalm} : palm;
        const Affine upperM = FromPalette(current[chain[0]].data());
        const Affine foreM = FromPalette(current[chain[1]].data());
        const Affine handM = FromPalette(current[chain[2]].data());
        const BodyVec3 shoulder = Apply(upperM, g_rig.rest[chain[0]]);
        const BodyVec3 elbow = Apply(foreM, g_rig.rest[chain[1]]);
        const BodyVec3 wrist = Apply(handM, g_rig.rest[chain[2]]);
        // Tracking/mapping sanity: far-off targets release the arm.
        const float reachX = target.x - shoulder.x, reachY = target.y - shoulder.y, reachZ = target.z - shoulder.z;
        if (reachX * reachX + reachY * reachY + reachZ * reachZ > 1.5f * 1.5f) continue;
        const auto solved = SolveTwoBoneIk(shoulder, elbow, wrist, target, ElbowPole(side == 0, right, forward));
        // Upper arm about the shoulder, then the forearm about the new elbow.
        const Affine upper = RotationBetween({elbow.x - shoulder.x, elbow.y - shoulder.y, elbow.z - shoulder.z},
                                             {solved.elbow.x - shoulder.x, solved.elbow.y - shoulder.y, solved.elbow.z - shoulder.z}, shoulder);
        const BodyVec3 movedWrist = Apply(upper, wrist);
        const Affine fore = RotationBetween({movedWrist.x - solved.elbow.x, movedWrist.y - solved.elbow.y, movedWrist.z - solved.elbow.z},
                                            {solved.wrist.x - solved.elbow.x, solved.wrist.y - solved.elbow.y, solved.wrist.z - solved.elbow.z}, solved.elbow);
        // Hand orientation: fingers along grip -Z, index->pinky along grip -Y,
        // measured on the already moved hand, rotated about the new wrist.
        Affine hand{};
        for (int i = 0; i != 3; ++i) hand.r[i][i] = 1.0f;
        if (orient) {
            const auto moved = [&](int bone) { return Apply(Compose(fore, Compose(upper, FromPalette(current[bone].data()))), g_rig.rest[bone]); };
            const BodyVec3 index = moved(fingers[0]), middle = moved(fingers[1]), pinky = moved(fingers[2]);
            hand = AlignFrames({middle.x - solved.wrist.x, middle.y - solved.wrist.y, middle.z - solved.wrist.z},
                               {pinky.x - index.x, pinky.y - index.y, pinky.z - index.z}, fingerDir, downDir, solved.wrist);
        }
        const auto& forearmBones = g_rig.subtree[chain[1]];
        const auto& handBones = g_rig.subtree[chain[2]];
        for (const int bone : g_rig.subtree[chain[0]]) {
            Affine m = Compose(upper, FromPalette(current[bone].data()));
            if (std::find(forearmBones.begin(), forearmBones.end(), bone) != forearmBones.end()) m = Compose(fore, m);
            if (std::find(handBones.begin(), handBones.end(), bone) != handBones.end()) m = Compose(hand, m);
            ToPalette(m, updated[bone].data());
        }
        if (haveWorld) {
            // Model-space motion per segment: upper; fore * upper; hand * fore * upper.
            const Affine foreTotal = Compose(fore, upper);
            g_carry.segment[side][0] = Compose(toWorld, Compose(upper, toModel));
            g_carry.segment[side][1] = Compose(toWorld, Compose(foreTotal, toModel));
            // Held objects stick to the controller, not to the arm's reach
            // (user 2026-09-30: "the weapon is tied to how far his arms can
            // go"): the solved wrist is clamped at full reach, so add the
            // remaining offset to the target.
            Affine reach{};
            for (int i = 0; i != 3; ++i) reach.r[i][i] = 1.0f;
            reach.t[0] = target.x - solved.wrist.x;
            reach.t[1] = target.y - solved.wrist.y;
            reach.t[2] = target.z - solved.wrist.z;
            const Affine handMotion = Compose(reach, Compose(hand, foreTotal));  // model space
            g_carry.segment[side][2] = Compose(toWorld, Compose(handMotion, toModel));
            g_carry.valid[side] = true;
            handMotionModel[side] = handMotion;
            handMotionValid[side] = true;
            if (nativeHands && orient)
                visibleHand[side] = BuildRicoVisualHand(side, chain[2], handMotion, target, fingerDir,
                    downDir, fingerInput[side], current, visibleHands[side]);
        }
        wrote = true;
    }
    {
        // Publish the gun mounts for vr_weapons (game pose and drawn pose).
        AttachPublish publish;
        for (int k = 0; k != 4; ++k) {
            const int bone = kAttachBones[k];
            const int side = k < 2 ? 0 : 1;
            if (!handMotionValid[side]) continue;
            Affine rest = QuatToAffine(g_rig.restQ[bone]);
            rest.t[0] = g_rig.rest[bone].x;
            rest.t[1] = g_rig.rest[bone].y;
            rest.t[2] = g_rig.rest[bone].z;
            const Affine game = Compose(FromPalette(current[bone].data()), rest);
            AffineToRowMajor(game, publish.game[k]);
            AffineToRowMajor(Compose(handMotionModel[side], game), publish.carried[k]);
            publish.valid[k] = true;
        }
        publish.tick = GetTickCount64();
        std::lock_guard attachLock(g_attachMutex);
        g_attach = publish;
    }
    if (g_hideHead) {
        // All head vertices collapse onto the (animated) neck joint, slightly
        // behind the eyes: invisible from the camera, body untouched.
        const BodyVec3 neck = Apply(FromPalette(current[kNeckBone].data()), g_rig.rest[kNeckBone]);
        for (const int bone : g_rig.subtree[kHeadBone]) {
            Affine collapsed{};
            collapsed.t[0] = neck.x;
            collapsed.t[1] = neck.y;
            collapsed.t[2] = neck.z;
            ToPalette(collapsed, updated[bone].data());
        }
        wrote = true;
    }
    if (g_hideAll) {
        // Whole body hidden (user 2026-09-30: controllers + gun only; the body
        // is a stretch goal). Every vertex collapses onto the animated hips,
        // ~1 m below the camera. The IK above still runs: the carried gun
        // uses its per-segment corrections. His shadow goes too.
        const BodyVec3 hips = Apply(FromPalette(current[2].data()), g_rig.rest[2]);
        for (int bone = 0; bone != static_cast<int>(kBoneCount); ++bone) {
            Affine collapsed{};
            collapsed.t[0] = hips.x;
            collapsed.t[1] = hips.y;
            collapsed.t[2] = hips.z;
            ToPalette(collapsed, updated[bone].data());
        }
        if (nativeHands) g_ricoVisibleBones.fill(false);
        for (int side = 0; side != 2; ++side) {
            if (!visibleHand[side]) continue;
            const int first = side ? kRightArm[2] : kLeftArm[2];
            std::copy(visibleHands[side].begin(), visibleHands[side].end(), updated.begin() + first);
            std::fill_n(g_ricoVisibleBones.begin() + first, kRicoVisualHandBones, true);
            for (int bone : {first - 1, first + 20, first + 21, first + 22}) {
                updated[bone] = visibleHands[side][0];
                g_ricoVisibleBones[bone] = true;
            }
        }
        wrote = true;
    }
    if (!wrote) return;
    // Only owned entries are written; the rest stays the game's.
    bool copied = true;
    for (const int bone : owned) copied = SafeCopy(data + bone * 12, updated[bone].data(), 48) && copied;
    if (nativeHands) {
        for (int side = 0; side != 2; ++side)
            g_ricoHandPrepared[side] = copied && visibleHand[side] ? GetTickCount64() : 0;
    }
    g_ownedBones.assign(kBoneCount, false);
    for (const int bone : owned) g_ownedBones[bone] = true;
    g_written = updated;
    ++g_applies;
    g_appliedThisFrame.store(true);
}
}
