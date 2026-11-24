# Test matrix (updated 2026-09-30)

"User" = observed by the user in the headset. "2D" = unattended desktop run. Evidence is in `docs/DEVELOPMENT_NOTES.md`.

| Scenario | State | Evidence / notes |
|---|---|---|
| Steam launch, proxy, fingerprint, OpenXR session and retry | Works | Many sessions; the loader is kept resident (the −10 crash is fixed) |
| Native alternate-eye stereo, direct head rotation | Works (user) | "Lag is gone. I am in first person" |
| 6DOF with automatic recenter | Works (user) | "It's great" |
| Right-eye ghost / world shaking | Fixed (user) | `reproject_to_frame_pose`, steering gain measured online |
| Render resolution 2048×2208 via `settings.json` | Works (user) | "a lot sharper" |
| Virtual pad from the Touch controllers | Works (user) | RT/LS/LB reach JC3 |
| Point-to-aim (trigger / grip steer the engine camera) | Works (user) | "fucking beautiful" |
| Gesture D-pad weapon switching | Works (user) | |
| HUD fixed depth per eye | Works (user) | "no longer glitchy"; 3 m, scale 0.65 |
| Crosshairs on the gun and grapple rays | Works (user) | "great for now" |
| Quest render-model controllers | Works (user) | |
| Head-follow culling | Works (user) | culling follows the head |
| Union culling (mode 2) | Superseded | "way better", holes remained at the edges; replaced by engine culling |
| Terrain holes (on foot and wingsuit) | Fixed (user, 2026-10-01) | `engine_culling`: terrain, scene and model culls from the head view + both-eye union; "perfect" |
| Destructibles disappearing | Fixed (user, 2026-10-01) | active-camera model-cull widen; "perfect" |
| Lighting / per-eye shadows | Fixed (user, 2026-10-01) | `engine_lighting`: off-axis reconstruction, shadows every frame + union/heading fit; "so good" |
| Hand depth occlusion | Failed (user) | "broke everything"; code in history |
| Arms following the controllers (visible body) | Works but twisted (user) | aim-frame hands better than grip-frame; replaced by the hidden body |
| Hidden body (`vr_body_hide_all`) | Mostly works (user) | brief hair/gear flashes at animation starts |
| Gun pinned to the controller | Works (user) | "perfect"; fires at the crosshair |
| Grapple gauntlet on the left hand | Broken (user) | not on the hand |
| Grapple wire origin | Broken (user) | starts at the animated hand |
| Bullets from the drawn barrel | Broken (user) | they spawn at the animated gun |
| Pistol hold angle | Odd (user) | |
| Aim point from the controller (`UpdateWeaponAiming`) | Works, no felt change (user, 2026-10-01) | camera steering already aimed the bullets |
| Hand bones in the game pose (`vr_hands_in_game`) | Pending headset test | 2D: bone map and attach-child check confirmed; Shift+F8 A/B |
| Dual-wield revolvers | Broken (user, 2026-10-01) | guns off the controllers; Rico visible while shooting |
| Parachute | Fine (user) | |
| Vehicles (car, heli, plane, boat) | Untested | |
| Menus, map, cutscenes | Partly | the menu falls back to JC3's own camera after 45 stale frames |
| Per-eye shadow differences | Fixed (user, 2026-10-01) | see lighting |
| NPCs with the upload rewrite | Fine (user) | "perfect" |
