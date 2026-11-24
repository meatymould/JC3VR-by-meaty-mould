# Just Cause 3 engine reference (JC3VR findings)

Consolidated from `docs/DEVELOPMENT_NOTES.md` (chronological, with the full evidence) as of 2026-09-30. Every fact applies only to the fingerprinted Steam x64 build in `docs/BUILD_FINGERPRINTS.md` (SHA-256 `A53C2D6B…C293`). Offsets are bytes. "RVA" means relative to the `JustCause3.exe` image base.

Labels:
- **CONFIRMED:** direct measurement.
- **STRONGLY SUPPORTED:** consistent evidence, no direct proof.
- **HYPOTHESIS**.
- **DISPROVEN / FAILED**.
- **UNKNOWN**.

When a fact here disagrees with the notes, the notes and the live evidence win; fix this file.

## 1. Process, launch and settings

- **Imports (CONFIRMED):**
  - `DINPUT8.dll` for `DirectInput8Create` only. This is why JC3VR's proxy is `dinput8.dll`.
  - `dxgi.dll`, `d3d11.dll`, `d3d9.dll`.
  - `XINPUT9_1_0.dll`, statically imported.
- **Launch (CONFIRMED):**
  - The game must be started through Steam (`steam://run/225540` or `steam.exe -applaunch 225540 …`). Running the exe directly exits at the entitlement check.
  - The RAM FFP first-person overlay (`dropzone\`) loads only with the VFS arguments `--vfs-fs dropzone --vfs-archive patch_win64 --vfs-archive archives_win64 --vfs-fs .`. Without them the game is third person.
- **Settings file:** `Documents\Square Enix\Just Cause 3\Saves\0\settings.json`.
  - **Render size (CONFIRMED):** JC3 sizes its internal render targets from this file's windowed size, not from the swapchain. Overriding the swapchain alone made JC3 draw into the top-left corner. VR uses a windowed `2048×2208`, which matches the eye aspect of about 0.93.
  - `GraphicsAA: 3` is temporal (**HYPOTHESIS**: SMAA T2x). With alternate-eye rendering it blends the other eye, so VR uses `GraphicsAA: 2` and `GraphicsBokehDOF: 0`.
  - Opening the in-game display options can rewrite the file.
- **LOW MEMORY dialog:** shown at every launch on this machine (about 3.3 GB free). With the tall window its OK button sits off-screen, so unattended runs press pad A instead.
- **Unattended input in 2D:**
  - Pad input injected through JC3VR's XInput hook works, including in menus.
  - Keyboard scan codes via `SendInput` work in gameplay.
  - Injected mouse buttons are ignored (**CONFIRMED** 2026-09-30).
  - `GetCursorInfo` reports the cursor visible during gameplay, so it cannot gate menus.

## 2. D3D11 frame structure

- **Threads (CONFIRMED):** `Present` runs on one thread. Scene uploads run on a render thread (about 82 "Globals" uploads per frame in-world). All sampled draws were on the immediate context.
- **Hooked vtable slots (CONFIRMED, SDK-verified):**
  - Context: `Map` 14, `Unmap` 15, `UpdateSubresource` 48, `ExecuteCommandList` (hooked).
  - The hook historically labelled "DSSetShader slot 60" is actually `HSSetShader`; `DSSetShader` is 64.
- **Globals block (CONFIRMED):** a 784-byte dynamic constant buffer bound at VS slot 0. The decompiled shaders declare it as `GlobalConstants { float4 Globals[49]; }`.

  | Registers | Contents |
  |---|---|
  | c0–c3 | rotated view-projection applied to camera-relative world positions (the main scene VP) |
  | c29–c32 | camera-relative rotated VP / projection (used by the shadow VS) |
  | c33–c36 | "standard depth" VP (render camera `+0x254`) |
  | c40–c43 | frustum side planes |
  | c44–c47 | projection copy |

  The old "canonical projection" detector (`c0[11] == -1`) only matches an unrotated view, which caused the earlier "uploads froze" misreading.
- **Projection convention (CONFIRMED):**
  - Row-vector D3D matrices with `m23 = -1`.
  - Reversed, infinite depth with near 0.1.
  - Off-axis terms: `m20 = ∓0.2425` and `m21 ≈ -0.193` for the Quest eyes.
  - JC3's own desktop projection is `m00 = 0.883397`, `m11 = 1.570483` (about 97° × 65°).
- **Per-object world matrices (CONFIRMED 2026-09-30):**
  - Uploaded as **absolute world, row-major 4×4** (translation at floats 12–14, w column 0,0,0,1), not camera-relative.
  - Seen in:
    - 208-byte constant buffers: `+0x0` world, `+0x40` presumably previous world;
    - 128-byte buffers at `+0x0`;
    - 512-byte buffers at `+0x0`;
    - 4 KB buffers: Rico's own world at various offsets; `+0x0` of a bone-riding object.
- **Skinned characters (CONFIRMED):** per-draw bone subsets are uploaded to 4096-byte and 528-byte constant buffers (the 528-byte ones for hair and eyes). Entries are 3×4 rows in the same layout as the palette (section 5).
- **Depth and shadows (CONFIRMED):**
  - Scene depth DSV format 20. Shadow cascades are 2048×2048 depth-only, format 55.
  - The shadow VS binds the same 784-byte slot-0 block, plus slot 1 (64 B), slot 2 (80 B) and slot 3 (10240 B).
  - The cascade matrix source is **UNKNOWN**.
- **HUD (CONFIRMED):**
  - The final scene pass is one unblended full-screen draw into the backbuffer.
  - Immediately afterwards JC3 calls `ExecuteCommandList` once on the immediate context with the backbuffer bound, just before `Present`. That command list is the HUD, and its draws never pass through the immediate-context hooks.
  - JC3VR copies the backbuffer at that call, which gives a pre-HUD image. Pixels that differ by more than 3/255 in the final image are HUD.
- **Terrain (CONFIRMED):** the tessellated terrain domain shader has the 3DMigoto hash `7a1ee521893b9809` (FNV-1 over bytecode). D3D debug names are not available.
- **Screen-space reconstruction (ferrobrew/pyxis-defs, MIT; bytes verified on our exe):** deferred clustered lighting, SSR, SSAO, screen-space subsurface, atmospheric scattering and depth of field rebuild their clip-to-view matrix with `Matrix4::PerspectiveFovInverse(out, fov, aspect, far, near)` (RVA 0x390E0).
  - Output: `[0][0] = aspect·tan(fov/2)`, `[1][1] = tan(fov/2)`, and a reverse-Z depth block (`[2][3] = (f−n)/(fn)`, `[3][2] = −1`, `[3][3] = 1/f`).
  - It cannot describe an off-axis frustum, so with our eye projections (m20 = ∓0.2425, m21 ≈ −0.193) every reconstructed position was skewed, mirror-opposite per eye.
  - The exact inverse of the engine's reverse-Z P is `[1/m00 0 0 0; 0 1/m11 0 0; 0 0 0 (f−n)/(fn); m20/m00 m21/m11 −1 1/f]`; its symmetric case equals the engine's output.
  - `engine_lighting.cpp` substitutes it for main-view calls (near/far equal to the active camera's) while an eye renders, after a one-time live layout check against the engine's own result. **Visual effect: pending headset test.**

## 3. Camera system

- **Owner (CONFIRMED, RTTI):**
  - `CCameraManager@NGraphicsEngine` (vtable RVA `0x2309c20`), whose pointer lives at `image+0x02ED0E20`.
  - `owner+0x5C0`: the **engine camera**, embedded at manager `+0x10`. It is upstream and drives aim.
  - `owner+0x5C8`: a pointer to the **render camera**.
  - Neither record has a vtable.
  - RTTI lists 41 camera classes (`tools/rtti_classes.py`).
- **Camera record layout (CONFIRMED, both records):**

  | Offset | Contents |
  |---|---|
  | `+0x14/+0x54/+0x94/+0xD4` | world transform ×4 (pyxis: `m_PreviousTransformF`, `m_TransformF`, `m_TransformT0`, `m_TransformT1`), row-major; right, up, back rows, **view forward = −row2** |
  | `+0x154` | projection P |
  | `+0x194` | view-projection VP |
  | `+0x1D4` | P duplicate |
  | `+0x214/+0x254/+0x294` | culling sets {inverse view, V·P2, P2}, stride 0xC0 (the third set has no P2). pyxis names: `+0x1D4` `m_PreviousProjF`, `+0x214` `m_PreviousViewF`, `+0x254` `m_PreviousViewProjectionF`, `+0x294` `m_Projection` (standard depth), `+0x2D4` `m_View`, `+0x314` `m_ViewProjection` (what `UpdateFrustum` reads), `+0x354/+0x394/+0x3D4` previous P/V/VP |
  | `+0x414` | six outward planes: near, far, left, right, top, bottom |
  | `+0x474` | per-plane positive/negative split normals |
  | `+0x534` | negated plane distances |
  | `+0x560` / `+0x56C` | culling position / forward |
  | `+0x578, +0x57C` | 0.868, 1.324 |
  | `+0x580/+0x584/+0x588` | vertical FOV (1.13446 rad = 65°) |
  | `+0x58C` | 1/tan(v/2) |
  | `+0x590` | ≈ P2 tan(v/2) |
  | `+0x594` / `+0x598` | near 0.1 / far 40000 |
  | `+0x5A0` / `+0x5A4` | tan(v/2) / aspect |
  | `+0x5C0` onward | small integers (the `+0x5CC…` "matrices" belong to a different base) |

- **Refresh behaviour (CONFIRMED):**
  - The engine rewrites both records before `Present`, copying `0x5C0` into `0x5C8` whenever the engine camera moves. The game thread rewrites `0x5C0` a few uploads into the render.
  - The engine resets `0x5C8`'s P to its symmetric value every frame after upload, so JC3VR re-applies per frame.
  - With no input, the engine may not rewrite the records at all.
  - The culling set (`+0x214…+0x5A4`) is refreshed only when the engine camera changes.
- **Culling rebuild rules (CONFIRMED):**
  - When JC3VR moves the render camera it must rebuild the inverse views, V·P2, the planes, the split normals, **the `+0x534` negated distances and the position/forward**.
  - Planes written without their companions culled an in-view house and turned the town into black LOD proxies.
- **FAILED — writing `0x5C0` mid-frame from the render thread:** it propagates into `0x5C8` and breaks the camera. Do not repeat this without mapping the engine's `0x5C0 → 0x5C8` derivation.
- **Other copies of the engine camera rotation (CONFIRMED, one scan):**
  - `CCameraObject` (`+0xd0/+0x110/+0x150`);
  - `CCameraTransition` (`+0x240…+0x700`);
  - `CAISystemJC3 +0x1a0`;
  - `CSoundSystem` (`+0xc0/+0x100/+0x160`);
  - `NGraphicsEngine::CGIPass +0x8c8`;
  - `CRestartPoint +0xcc`;
  - static rows near `0x142ED7704` and at `0x142F34190/0x142F38190`.
- **Visibility still tied to the engine camera (CONFIRMED):** some culling, LOD or streaming input still follows the engine camera. With the gun steering the aim, objects popped in and out. Which input it is: **UNKNOWN**. JC3VR therefore keeps the engine camera following the head, except while a trigger or grip steers it.
- **Engine camera steering (CONFIRMED):**
  - The engine camera turns in response to relative mouse input. Its sensitivity varies: more than 45° in a frame from ≤200 counts has been seen.
  - JC3VR measures the response online and caps its gain.
- **Render entry points (for reference; hooks there FAILED):**
  - Upload caller chain: RVAs `0x1963D3C, 0x1738A5, 0x0F1E8E, 0x1DF9CBB, 0x1DFA3EF, 0x111545B`.
  - Renderer entry `0x0F1D10` (RCX = render frame; `+0x720` = camera render block).
  - MinHook rejects that prologue, and an inline bridge crashed the game.

## 4. Visibility culling

JC3 decides visibility in the simulation phase, once per frame, against **copies** of the active camera (`CameraManager::m_ActiveCamera`, owner `+0x5C0`), not against the render camera that JC3VR writes per eye. The mechanism, names and addresses come from ferrobrew/jc3vr and ferrobrew/pyxis-defs (MIT, build "Steam 20206564"). Same build as ours: **CONFIRMED** by the singleton address, the `GetRenderCamera` bytes, and the field layout. They are cross-checked here against our own findings.

| System | Camera it culls against | What JC3VR does (`src/game/engine_culling.cpp`) |
|---|---|---|
| Terrain patches | `STerrainPatchSystem::m_TerrainCamera` (+0x58), copied from the active camera by `TerrainPatchSystemUpdate` (RVA 0x32F780) when `m_UpdateCamera` (+0x50) is set | After the copy: head view + union projection, VP rebuilt, planes rebuilt with the engine's `Camera::UpdateFrustum` (RVA 0xB2FC0) |
| Terrain hull shader | Back-patch cull against the render camera's forward axis (threshold -0.3), and a frustum cull baked once per frame; flags on `CRenderBlockTypeTerrainPatch` (singleton 0x142EED238, +0x44D/+0x44E; both 1 in the shipped game) | Both set to 0 while VR is active, restored otherwise |
| Scene cull (BFBC: models, streaming, AO volumes) | `OccluderCollectionManager`'s own cull camera (this+0x8), via `GetBFBCFrustumParamsForCameraAndTime` (RVA 0xD68B0) | Head view + union projection before the frustum is built; `m_FOVT1` (+0x584) set to 65°; frustum count (`BFBCFrustumCullParameters` +0x1280) set to 1, dropping the software-occlusion frusta |
| Per-render-block model cull, instance fade, road meshes, far lights | The active camera's precomputed `m_FrustumPlane` (+0x414), rebuilt by `Camera::UpdateFrustum` | That call is hooked for the active camera: the planes are rebuilt a second time from the head view × union, then `m_ViewProjection` is restored |
| Spawn system, character occlusion | The active camera's projection | Not widened yet (ferrobrew: `widen_spawn_cull`, `spawn_budget_scale`) |

- **Union projection:** for each side, take the wider of the two eyes, scale it by 1.4 (`cull_fov_padding`), add (IPD/2)/near for the eye offset, and clamp at 85°. Standard depth, right-handed, off-centre, row-major (`m[8] = (l+r)/(r-l)`, `m[11] = -1`).
- **Why the head view and not the copied camera:** in our design the engine camera is *steered* toward the head. It lags it by up to about 22° in fast turns (captures, 2026-10-01) and follows the flight path in the wingsuit. ferrobrew's engine camera *is* the head, so they don't need this.
- **Size cull:** reads `tan(m_FOVT1/2)`. JC3VR's earlier FOV-field widening (culling mode 2, about 119°) made it about 2.7× more aggressive (**HYPOTHESIS** for "objects disappear at a weird distance").

History (each step disproved or superseded by the next):
- JC3VR's own render-record widens (culling modes 1–3: union planes, P2, FOV fields) helped models, but never reached terrain.
- The previous-frame hypothesis is **DISPROVEN** (F12 double-render made no difference).
- The engine camera pointing away is **DISPROVEN** for the on-foot holes (it was aligned within 1.5°).
- The terrain camera widen: **user, 2026-10-01:** on foot "rarely, if ever" see terrain holes. Destructibles still disappeared and the wingsuit was "better, but still bad", which led to the head-view change and the model-cull widen (b83537b).
- **CONFIRMED fixed (user, 2026-10-01, 585273F37A66 / b83537b):** "the solution to the terrain and destructibles disappearing is perfect", with terrain, scene and model culls built from the head view plus the both-eye union.
- The P2 screen-size LOD scale is **DISPROVEN** as the cause (slabs identical at scale 1 and 2).
- Blue slabs and terrain holes are caused by JC3VR (**CONFIRMED** by control).
## 5. Rico: character, skeleton and animation

- **`CCharacter`:** vtable RVA `0x23d16d8`, with another `CCharacter` vtable (`0x23d16b8`) at `+0x8`. Offsets below are from O, the address holding `0x23d16d8`.
  - Older notes use a base P = O − 8, so P+0x4E8 is O+0x4E0. Mixing the two bases broke discovery once.

  | Offset | Contents |
  |---|---|
  | `+0x410` | model path `models/jc_characters/main_characters/rico…` |
  | `+0x4E0` | skeleton path `animations/jc_skeletons/characters/rico` (used to identify Rico) |
  | `+0x508` | ragdoll path |
  | `+0x978` | `CInventory`, followed by shared_ptr slots (object, control block): `+0x998` and `+0x9A8` `CBulletWeaponBase`, `+0x9C8` `CThrownGrenadeWeapon`, `+0x9D8` `CPlantedExplosiveWeapon`, `+0x9F8` `CRPGWeapon`, `+0xA08` `CBeaconWeapon`, `+0xA18` `CParachuteObject`, `+0xA28` `CWingsuitObject`, `+0xA38` `CGrapplingHook`, `+0xA58` `CHijackingExplosive` |
  | `+0xDF0` | `CCharacterIK` (mostly zero while idle) |
  | `+0x1778` | `CAnimatedModel` (vtable `0x237e850`) |
  | `+0x27E8` | world transform, row-major, yaw only: `p_world = p_model · R + t` |

- **Animation chain:**
  - `CAnimatedModel +0x178` → `NCharacterSystem::CAnimationControl` (vtable `0x23773d0`).
  - Its `+0xA8` is a shared_ptr to `NGraphicsEngine::CSkinningPaletteData`: `+0x0` begin (= self + 0x50), `+0x8` end, `+0x48` count = **177**, then 177 × 48 B of 3×4 row-major entries in the column-vector convention (`p' = R p + t`, the translation in each row's fourth float). Each entry = animated × inverse rest (model space). Bone 0 is identity.
  - `+0xB8` is `CPoseProducer` (state machine `rico_partial_system`).
  - **Only one 177-bone palette object exists** (`live_peek palettes 177`).
- **Skeleton (CONFIRMED):**
  - `hkaSkeleton` (vtable `0x258bd30`) named "Reference": `+0x10` name, `+0x18` parents (int16), `+0x28` bones {name, lockTranslation}×16 B, `+0x38` reference pose (hkQsTransform t,q,s; 48 B; parent-local).
  - Rest pose: +Y up, facing −Z, left = −X. Head at 1.635 m, eyes at 1.704 m.

  | Bones | Names |
  |---|---|
  | 2 / 3–5 / 6 | Hips / Spine / Sternum |
  | 7 / 8 / 9–21 | Neck / Head / face |
  | 22 / 23 / 24 / 25 | LeftShoulder / Arm / ForeArm / Hand |
  | 26, 44 | LeftHandAttach, LeftHandAttach2 |
  | 27–43 | left fingers (Index1 30, Middle1 33, Pinky1 41) |
  | 45–47 / 48 / 49–53 | LeftForeArmRoll + df rolls / LeftElbow / arm rolls |
  | 54–61 | left shoulder helpers, harness, para attach |
  | 62–65 | right shoulder / arm / forearm / hand |
  | 66, 84 | RightHandAttach, RightHandAttach2 |
  | 67–83 | right fingers (Index1 70, Middle1 73, Pinky1 81) |
  | 85–100 | right mirror of 45–61 |
  | 101–104 | back harness; BackAttach 103/104 |
  | 134–138 / 147–150 | Pelvis, left leg / right leg |
  | 143 / 156 | LeftHolsterAttach / RightHolsterAttach |
  | 163–166 / 169–174 / 175 / 176 | CharacterCam1–4 / IK targets / AimTarget / rf_GrappleWirePosition |

- **Palette timing (CONFIRMED):**
  - The animation thread rewrites the palette mid-frame.
  - The pose is refreshed every second frame. On those frames the render thread fills Rico's per-draw bone uploads **from its own copy of the fresh pose**, so edits to the palette object never reach those draws.
  - Parity probe, 29 uploads per frame: odd frames 20 JC3VR / 7 game; even frames 7 / 7 / 15 "neither".
  - This is why a body edit showed on one eye only, why F8 swapped it, and where the untouched "animation Rico" came from. JC3VR therefore also rewrites the entries inside each upload at `Unmap`.
- **Attached objects (CONFIRMED):**
  - Weapons and gear are separate draws with absolute world matrices exactly on a Rico bone.
  - While firing, the gun's world matrix equals **RightHandAttach (66)**: 0.000 m, identity rotation relative to it. Its rotation relative to RightHand (65) is `[0,0,1; -1,0,0; 0,-1,0]`, so the gun's −Z (the barrel, if models face −Z) runs along the hand's −X (wrist → knuckles).
  - The grapple gauntlet sits exactly on LeftForeArmRoll (45) at `+0x0` of a 4 KB buffer.
  - Hair and eyes are drawn with head-bone subsets.
- **The gun is drawn only while firing (CONFIRMED in 2D):** it did not appear at idle, when aiming with LT, or with injected mouse input. Bullets spawn from the animated muzzle and travel toward the engine-camera crosshair.
- **`CBulletWeaponBase`:**
  - `+0x508` owner `CCharacter`.
  - ADF tuning pointers: `SWeaponTuning +0x480`, `SAutoAim +0x498`, `SAttachSettings +0x4B0`, `SGripPointSettings +0x4C8`, `SRigidObjectEffects +0x4E0`.
  - Its `+0xD0` matrix is stale (not the drawn transform).
- **`CBoneAttachment`:** vtable `0x23c7aa8`; `+0x70` owner `CCharacter`; world at `+0xD4` (one instance, at head height).
- **Sim-side pose API (pyxis defs, ferrobrew; byte-guarded in our exe, 2026-10-01).** Addresses are RVAs; `this` for Character functions is the pyxis base O − 8.

  | Function / field | Where | Notes |
  |---|---|---|
  | `Character::UpdatePropEffects(this, dt)` | `0x7C2380` | last step of pose finalisation (SIM). A `SetJoint` after it reaches the skinning job, the weapon attach chain and the camera |
  | `Character::GetSafeIndex(this, hash)` | `0x79AB30` | `NBone::ESafeBoneIndex` hash → bone index |
  | `AnimationController::GetJoint / SetJoint(this, index, Joint*)` | `0x43FF90` / `0x43FFF0` | model-space hkQsTransform (t, q xyzw, s; 16-byte vectors). `SetJoint` re-derives every descendant |
  | `m_AnimatedModel.m_AnimationController` | O − 8 + `0x18F8` | = O + `0x1778` + `0x178` |
  | `m_IsLocalCharacter` | O − 8 + `0x262A` | bool |
  | `m_WorldMatrixT1` | O − 8 + `0x2830` | the next-step world transform (T0 at O + `0x27E8`) |
  | `Character::UpdateWeaponAiming(this, const Vector3*)` | `0x7602C0` | stores the single weapon aim point at O − 8 + `0x26BC`; every gun fires from its muzzle toward it |

  **CONFIRMED live (2D, 716a63e):** `GetSafeIndex` gives LEFT_HAND = 25, RIGHT_HAND = 65, ATTACH_HAND_LEFT = 26, ATTACH_HAND_RIGHT = 66, the same indices as the skin palette. Moving RIGHT_HAND by 0.1 m with `SetJoint` moved ATTACH_HAND_RIGHT by 0.100 m, so the gun attach bones are children of the hands. Per ferrobrew's docs, the weapon model, muzzle effects, the weapon's physics proxy and the shot origin all derive from the attach bone, and a dual-wield pair uses one attach bone per gun (`…2` for the second). **STRONGLY SUPPORTED** (their docs; not yet verified by us in the headset).
- **FFP body model:** `rico_body_lod1.rbm` (RAM FFP) uses the `mc_rico_body_camera` material, which renders the arm skin transparent. With the vanilla model plus JC3VR's head collapse the skin is correct (**CONFIRMED** in 2D). The FFP file is currently renamed `.off` (user-approved).

## 6. Grapple (RE parked)

- **Classes (RTTI):**
  - `CGrapplingHook` `0x023e8c98` (secondary vtables `0x023e8c80`, `0x023e8c60`, `0x023e8c30`);
  - `CGrapplingHookWire` `0x023e7938`;
  - `CGrapplingHookPropData` `0x023e9bc8`;
  - `CGrapplingHookWireEndCastFilter@CWireEnd` `0x023e1b98`;
  - `CGrappleAttachAimBlendState` `0x023bcd78`;
  - `CGrapplePointProxy` `0x023e1a98`.
- **Behaviour (CONFIRMED):**
  - Each shot creates a new `CGrapplingHookWire`.
  - The wire-end cast filter runs 10–25 times per shot.
  - No aim ray appears in virtual-call arguments, so the aim comes from non-virtual firing code.
- **FAILED:** stack scanning for the caller is too noisy (stale cookies).
- **Open:** the grapple wire starts at animated Rico's hand (bone 176 `rf_GrappleWirePosition` is the likely source).

## 7. Input

- JC3 reads player 1 through `XInputGetState` and `XInputGetCapabilities` of `XINPUT9_1_0.dll`; JC3VR hooks both in-process.
- **Xbox layout:**
  - **CONFIRMED in VR:** RT fire, **LB grapple**, D-pad weapon selection, A menu confirm.
  - **Standard JC3 layout, not individually verified here:** LT aim, Start menu.
  - The RB action: **UNKNOWN** (not verified).
- JC3 switches input device automatically, and the pad is accepted in menus.

## 8. OpenXR / SteamVR on this machine

- **Loader:** `SteamVR\bin\win64\openxr_loader.dll`, found through the active-runtime registration.
  - Request API 1.0; the SDK's current version is rejected.
  - Keep the loader resident. Unloading it after a failed `xrCreateInstance` crashed the game (`-10`, then heap corruption).
- **Eye images:** SteamVR hands out `R8G8B8A8_TYPELESS` (27), which can't be a render target, so JC3VR renders to a typed intermediate and copies. The eye target is sRGB (29); JC3's backbuffer is UNORM (28), sampled through an sRGB view.
- **Quest per-eye FOV:** left L −54° R 40° U 44° D −55°, mirrored for the right eye. Tangent aspect 0.925. IPD about 63 mm.
- **Quest controller poses:** the SteamVR grip frame runs along the handle. The aim forward in grip coordinates is `(0, -0.866, -0.5)`, 60° from grip −Z (**CONFIRMED** in two sessions). The render model `oculus_quest_plus_controller_*` places `openxr_grip` at X rotation 20.6°.
- **Runtime behaviour:**
  - SteamVR closes when JC3 closes, so unattended relaunches lose the headset.
  - An idle headset throttles the loop to about 6 fps.
  - `xrCreateInstance` returns −2 while the headset is asleep.
  - Motion Smoothing is off in `steamvr.vrsettings`.

## 9. Dead ends (do not repeat without a new discriminator)

- Geo-11 / 3DMigoto side-by-side stereo and the Osiris reference path (historical; the native alternate-eye path replaced them).
- Swapchain-size override alone for render resolution.
- Hooking the renderer entry or projection producer (MinHook rejection; the inline bridge crashed).
- Writing engine camera `0x5C0` mid-frame.
- Reconstructing P from a live camera rotation (misidentified eyes and caused heavy lag).
- A blended-backbuffer-draw hypothesis for the HUD (the HUD is one command list).
- Always-on per-draw state hooks (they changed timing and caused eye mislabelling and flashing).
- Hand depth occlusion via the Present-time DSV ("broke everything"; code kept in history `e1c8d55`).
- Grip-frame hand orientation for Quest controllers ("very, very wrong").
- Separate cosmetic avatar instead of Rico's own skeleton (not recommended: it would lose lighting, shadows and depth).
