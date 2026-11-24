# Research findings

## Native architectural reference

`theHunterCotW-VR` is a strong Apex-family reference, not an address source. Its `docs/ARCHITECTURE.md` documents an outermost `Present` owner, always forwarding desktop presentation, `ResizeBuffers`-driven resource recreation, pose timing, and camera probing on the presenting thread. Its source exposes a full OpenXR frame lifecycle and an alternate-eye implementation. JC3VR will reuse those experimental principles, not its executable data. The project is GPL-3.0; no code was copied.

## JC3 render-pipeline evidence

Luma's current `Source/Games/Just Cause 3/main.cpp` identifies scene depth around the motion-vector pass, tracks per-frame jitter, and states that its JC3 projection matrix is CPU-side rather than in a current constant buffer. That makes Luma/RenderDoc capture and CPU buffer-update tracing a higher priority than blind CB scanning. It also contains a late-2025 Steam-version jitter patch offset; this is explicitly untrusted until independently fingerprinted and validated.

## Injection and prototype references

`magicalcookie/jc3-mod-loader` confirms a `dinput8.dll`/`DirectInput8Create` route for JC3 DLL mods. JC3VR deliberately does not reuse its experimental filesystem, hot-unload, or fixed `VirtualAlloc` patterns.

Osiris currently accepts Geo-11/Katanga, SBS/TAB, and depth-stereo sources in OpenXR and can emit mouse/gamepad/6DoF tracking. It remains the preferred first headset test once the game directory is available. Luma's release documentation warns that it uses `dxgi.dll`, which reinforces validating a non-DXGI injection route before combining Luma and Geo-11.

DECA documents support for Just Cause 3 plus RTPC extraction/parsing. It and the Gibbed tools are the chosen first tools for future offline searches for camera, first-person, wingsuit, parachute, and vehicle settings. No game archives or mod assets were obtained or extracted.
