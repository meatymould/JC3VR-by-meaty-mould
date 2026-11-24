# JC3VR architecture (code map)

Updated 2026-09-30. See `docs/ENGINE_REFERENCE.md` for the JC3 engine facts the code relies on, and `docs/DEVELOPMENT_NOTES.md` for the history and evidence. `PROJECT_STATE.md` has the current validated state.

## 1. Load chain

```
Steam (-applaunch 225540 + Dropzone VFS args)
  JustCause3.exe
    dinput8.dll  (src/proxy: forwards DirectInput8Create; a loader-safe worker)
      mods\jc3vr.dll  (src/core/dllmain.cpp: JC3VR_Start, idempotent)
        ArmEarlyGraphicsHooks: config -> Configure*() -> DXGI/D3D11 creation hooks
        bootstrap worker: fingerprint, OpenXR loader probe, XInput hook, Present hook
```

- `jc3vr.ini` beside the exe is read by `src/core/config.cpp`. Every `Configure*` call in `dllmain.cpp` exists twice, once for the early-hook path and once for the worker path; keep the two in sync.
- The executable fingerprint (`build_fingerprint.cpp`) gates guarded addresses. All engine offsets are validated by RTTI/vtable, layout or signature checks before use; a mismatch disables the feature.

## 2. Threads and where work happens

| Thread | Entry (hook) | Work |
|---|---|---|
| Present (game main) | `render/dx11_hook.cpp PresentDetour` | `ObserveLiveSwapchain`, `VrBodyPresentTick`, `EnsureOpenXrSession`, `SubmitMonoOpenXrFrame` → `SubmitNativeAlternateFrame` (the whole XR frame), head-look |
| Render (game) | `game/camera_capture.cpp MapDetour / UnmapDetour / UpdateSubresourceDetour` | Globals tracking, paired-camera re-sync, VR body palette re-assert (`VrBodyApplyAtUpload`), upload rewrite / carry (`VrBodyCarryUpload`, `VrBodyRewriteUpload`), diagnostics |
| Render (game) | `ExecuteCommandListDetour` | pre-HUD backbuffer copy (the HUD is one command list, just before Present) |
| Input (game) | `game/vr_gamepad.cpp XInputGetStateDetour` | virtual pad from the controllers; automation trigger files |
| JC3VR background | `std::thread` | VR body discovery scan, camera/FOV/skeleton scans, controller model loading |

## 3. One XR frame (`SubmitNativeAlternateFrame`, `src/openxr/xr_session.cpp`)

1. `xrWaitFrame` / `xrBeginFrame` / `xrLocateViews` at the predicted display time. Hand grip/aim poses and controller inputs (`UpdateHandPoses`).
2. Inputs → `VrControllerInputs`:
   - snap turn, stick-click recenter;
   - hand-aim ownership (right trigger → right hand, left grip → left hand), with fire held back until the engine camera is aligned (250 ms max);
   - the result is published to the virtual pad.
3. `UpdateHandAim`: steers JC3's **engine camera** (relative mouse input, online-measured gain) toward the head (culling/LOD/streaming follow it) or toward the active hand's aim ray.
4. Eye retain:
   - The just-rendered backbuffer is identified by the projection it was rendered with (`matchesEye`), not by the alternation label.
   - It is copied into `nativeEyeSources[eye]`.
   - The HUD is re-composed per eye (`ComposeHudForEye`: HUD = pixels changed since the pre-HUD copy; placed at `hud_distance_m`; crosshair copies on the gun and grapple hand rays).
5. The VR body hand targets (`SetVrBodyHandTarget`) are computed from the grip positions and aim orientations, in engine-camera world space.
6. The next eye is chosen (`ShouldAdvanceNativeEye`; F8 phase shift; F12 double-render test) and written for JC3's next render:
   - projection (`SetNativeEyeProjection`);
   - camera records `owner+0x5C0/0x5C8` via `SetNativeCameraOffset` → `SetNativeCameraOffsetForCameras`: head rotation, eye offset, VP reconciliation and the culling rebuild (`RebuildNativeCullingState`, culling modes 0–3).
7. Render and submit: each retained eye is ray-resampled into the OpenXR swapchain image (`RenderNativeEyeToOpenXr`), reprojected to this frame's pose (`reproject_to_frame_pose`). Then controller models and markers are drawn (`RenderHandMarkers`), then `xrEndFrame` runs.

JC3 renders **one eye per game frame** (alternate-eye). Anything the engine carries from the previous frame therefore belongs to the other eye. This is the root of several bugs: the body showing on one eye, and possibly the terrain holes (see `ENGINE_REFERENCE.md` §4–5).

## 4. Modules

| File | Role | Status |
|---|---|---|
| `src/core/dllmain.cpp` | bootstrap, config fan-out | production |
| `src/core/config.{h,cpp}` | ini parsing (keys in §5) | production |
| `src/core/logging.*` | `%LOCALAPPDATA%\JC3VR\jc3vr.log` and `bootstrap_worker.log` (`TraceRuntime`) | production |
| `src/core/build_fingerprint.*`, `signatures.*` | exe SHA-256/PE fingerprint, guarded-address validation | production |
| `src/proxy/dinput8_proxy.cpp` | the `dinput8.dll` proxy | production |
| `src/render/dx11_hook.*` | DXGI Present/ResizeBuffers/CreateSwapChain hooks, optional swapchain size override | production |
| `src/openxr/xr_loader.*`, `xr_instance.*` | loader discovery (resident for the process lifetime) | production |
| `src/openxr/xr_session.cpp` | session/retry, swapchains, alternate-eye frame, resample and reprojection, HUD compose, hand aim and steering, controllers, captures (F3) | production (also contains the legacy Geo-11 SBS and mono-quad paths) |
| `src/game/camera_capture.*` | D3D11 context hooks (Map/Unmap/UpdateSubresource/draws/ExecuteCommandList), native camera record writes, VP sync, culling rebuild, projection tracking, eye freshness, HUD pre-copy; many bounded probes | production + probes (off by default) |
| `src/game/vr_body.*` | pure math, unit-tested: two-bone IK, elbow pole, neck, body yaw | production |
| `src/game/vr_body_rig.*` | Rico discovery (vtable scan, skeleton), arm IK on the skin palette, head collapse / hide-all, upload rewrite, carrying attached objects (gun/gauntlet), probes (attach, parity) | production (`vr_body=1`) |
| `src/game/vr_gamepad.*` | XInput hook, controller → Xbox mapping, automation files | production |
| `src/game/engine_culling.*` | widens JC3's own visibility culls (terrain camera, terrain hull culls, scene/BFBC cull + occlusion, active-camera model cull) to the both-eye union, built from the head view; mechanism from ferrobrew/jc3vr (MIT) | production |
| `src/game/engine_lighting.*` | lighting fixes for off-axis eyes; so far the exact clip-to-view inverse for JC3's screen-space reconstruction (`PerspectiveFovInverse` hook, with a live layout self-check); mechanism from ferrobrew/jc3vr (MIT) | production (new) |
| `src/game/vr_weapons.*` | weapons in the hands: the weapon aim point from the right controller ray (`UpdateWeaponAiming` hook) and Rico's hand bones placed on the controllers in the game's own pose (`UpdatePropEffects` post-call `SetJoint`), so the game draws and fires the guns from the hands; mechanism from ferrobrew/jc3vr (MIT) | production (new, awaiting headset test) |
| `src/render/controller_model.*` | loads SteamVR's Quest Touch Plus OBJ/PNG/JSON at runtime | production |
| `src/render/vr_hands.*` | procedural hand/controller meshes and aim beam (fallback) | production fallback |
| `src/game/head_look.*` | legacy mouse head-look (pre-direct rotation) | legacy |
| `src/game/camera_source_scan.*` | engine-camera copy scan, FOV scan, skeleton probe, cull-override A/B | diagnostic |
| `src/game/grapple_probe.*` | vtable call-count probe of grapple classes | diagnostic (parked) |
| `src/game/camera_probe.*`, `render/swapchain.*` | small helpers | support |

## 5. Configuration (`jc3vr.ini`)

The production profile is `profiles/jc3vr-direct-headlook.ini`. Defaults are from `config.cpp`; "prod" is the production value.

- **Rendering and stereo:**
  - `stereo_mode` (prod `native_alternate`), `native_stereo_rig_mode` (prod `native_stereo`), `native_projection_override` (1).
  - `sync_native_view_projection` (1), `sync_native_projection_to_xr` (1), `sync_paired_camera_at_upload` (1), `sync_native_culling_camera` (1).
  - `culling_mode` (default 2: union + widened P2/FOV), `reproject_to_frame_pose` (1), `paired_eye_submission` (0).
  - `force_native_projection_buffer` (0), `render_width/height` (0 = off; use JC3's `settings.json` instead).
- **Head and body tracking:**
  - `head_tracking`, `positional_tracking`/`six_dof` (1), `six_dof_scale` (1), `six_dof_limit_m` (0.65).
  - `jc3_world_units_per_meter` (1), `recenter_key` (F2; auto recenter at world entry).
- **Controllers and aim:**
  - `motion_controllers` (1), `vr_gamepad` (1), `hand_aim` (1), `hand_aim_gain` (0.5), `snap_turn_deg` (30).
  - `engine_follow_head` (1), `aim_follows_right_hand` (0; 1 makes pop-in follow the gun), `controller_model` (1), `aim_lines` (0).
- **HUD:** `hud_align` (1), `hud_distance_m` (3), `hud_scale` (0.65).
- **Engine culling:** `widen_terrain_cull` (1), `relax_terrain_hull_culls` (1), `widen_scene_cull` (1), `disable_scene_occlusion` (1), `widen_model_cull` (1), `cull_from_head` (1), `cull_fov_padding` (0.4), `cull_size_fov_deg` (65).
- **Engine lighting:** `offaxis_reconstruction` (1), `shadows_every_frame` (1), `shadow_fit_union` (1), `shadow_fit_heading` (1).
- **Weapons and hands:** `aim_from_controller` (1), `vr_hands_in_game` (0; game-pose hands rejected), `guns_to_controller` (1), `weapon_controller_roll` (1; 0 restores barrel-only alignment with animated roll). Native gun placement uses the fifth placement argument's safe-bone selector, not a per-frame nearest-hand guess. A bounded cache keeps the mount-local offset stable across animation updates; exact successful placements are exempt from a second render carry.
- **VR body:**
  - `vr_body` (1), `vr_body_hide_all` (prod 1: controllers + gun only), `vr_body_hide_head` (1), `vr_body_carry` (1), `vr_body_hand_aim_frame` (1).
  - `vr_body_debug_pose` (0; 2D tests only: raises both hands and enables the attach probe).
- **Diagnostics (keep 0 unless a test needs them):**
  - `native_camera_probe_enabled`, `shadow_probe`, `stereo_coherence_probe`, `camera_timeline_probe`, `native_eye_capture` (1 enables F3).
  - `camera_source_scan`, `fov_source_scan`, `skeleton_probe`, `grapple_probe`, `cull_override_test`, `lod_scale_test`, `culling_mode_test`.
  - `debug_head_yaw_deg`, `debug_head_yaw_wobble_deg`, `debug_hand_aim*`, `hand_debug_preview`.
- **Legacy Geo-11 / mono:** `camera_mode`, `geo11_*`, `stereo_source`, `head_look_mode`, `head_yaw_scale`, `head_pitch_scale`, `show_body`, `show_head_mesh`, `near_clip`.

## 6. Hotkeys (desktop keyboard)

| Key | Action |
|---|---|
| F2 | recenter (also both stick clicks; automatic at world entry) |
| F3 | save both retained eyes (`native_eye_<pid>_<tick>_{left,right}.rgba` + `.txt`, which also records the engine camera vs head/hand steering state) |
| F4 | arm the gated native renderer probe (only with `native_camera_probe_enabled=1`) |
| F5 | cycle culling mode 0–3 (or the cull-override candidate when that test owns F5) |
| Shift+F5 | engine culling widen (`engine_culling`) on/off |
| F6 | pause/resume JC3VR camera writes (engine camera only) |
| Shift+F6 | lighting fixes (`engine_lighting`: reconstruction + shadow fit/refresh) on/off |
| F7 | pause/resume engine-camera head-follow |
| Shift+F7 | weapon aim point from the controller (`vr_weapons`) on/off |
| F8 | shift the eye phase by one frame (frame-parity discriminator) |
| Shift+F8 | hands in the game pose (`vr_weapons`) on/off |
| F9 | paired eye submission |
| F10 | eye reprojection to the frame pose |
| F11 | HUD re-placement |
| F12 | symmetric union eye frustum (both eyes render the same shape; experiment) |
| Shift+F12 | double-render test: each eye rendered twice, second shown |
| PgUp / PgDn | HUD distance ×/÷ 1.25 |
| Home / End | HUD size ± 0.05 |

## 7. Automation trigger files (`%LOCALAPPDATA%\JC3VR\`)

Create the file; JC3VR deletes it when consumed.

| File | Effect |
|---|---|
| `jc3vr_press_a` | pad A for 250 ms (works without XR; gets through LOW MEMORY/CONTINUE) |
| `jc3vr_pad` | text `"<control> <ms>"`, e.g. `RT 2500`; controls: A B X Y LB RB LS RS UP DOWN LEFT RIGHT START BACK LT RT |
| `jc3vr_attach_probe` | ~20 frames of attached-object matrix logging (debug-pose builds) |
| `jc3vr_parity_probe` | 60 frames of per-frame Rico upload counts (JC3VR vs game entries) |
| `jc3vr_carry_toggle` | flip carrying of attached objects |
| `jc3vr_debug_pose_alt` | move the debug right hand low and outward |

## 8. Tools

- **`tools/live_peek.cpp`:** a read-only `ReadProcessMemory` inspector. Build with `cl /nologo /EHsc /O2 /std:c++17 tools\live_peek.cpp /Fe:build\live_peek.exe`. Commands:

  | Command | Output |
  |---|---|
  | `dump <addr> [bytes]` | qwords/floats with RTTI class names of pointees |
  | `floats <addr> <n>` | raw floats |
  | `find <vtableRVA> [max]` | objects by vtable |
  | `refs <addr> [window] [max]` | pointers into a range |
  | `parents <count>` | Havok parent arrays |
  | `skeleton <addr>` | bone names and rest pose |
  | `matrices <addr> [bytes]` | rigid 4×4s |
  | `palettes <count>` | skinning palettes |

- **`tools/harness/`:** the unattended 2D test loop (see §9).
- **Other tools:** `rtti_classes.py` (offline RTTI listing), `find_floats.cpp`, `camera_state_scan.cpp`, `live_camera_snapshot.cpp`, `analyze_*.py`, `deploy.ps1`, `fingerprint.ps1`.

## 9. Build, test, deploy

- **Build:** `cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release`, then `cmake --build build`, from a VS x64 tools environment (`vcvars64.bat`).
- **Tests:** `build\jc3vr_core_tests.exe` (IK, body yaw, gamepad mapping, controller model parsing), plus the DLL and proxy smoke tests. A passing build and tests are **not** live proof.
- **Deploy:** with JC3 closed, back up `…\Just Cause 3\mods\jc3vr.dll` byte-for-byte to `backups\<name>\jc3vr.dll.<hash12>`, copy `build\jc3vr.dll`, and write `jc3vr.ini` from the profile (ASCII).
- **Unattended 2D loop:** `tools/harness/auto_world.ps1 -Name X -Set @('key=value') [-MenuSeconds 230] [-SettleSeconds N]`.
  - It closes JC3, writes the ini, backs up and replaces the DLL, launches through Steam with the VFS args, dismisses the SteamVR error box, presses A and screenshots.
  - `key.ps1` sends scan-code keys (mouse input is ignored by JC3), `shot.ps1` takes a screenshot, and `pair_png.py` converts an F3 pair to PNG.
  - Output goes to `%TEMP%\jc3vr-harness` (or `JC3VR_HARNESS_OUT`).
  - The harness uses 2D only: SteamVR closes with JC3, so unattended runs have no headset.
