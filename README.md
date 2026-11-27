JC3VR

Developed by MeatyMold

WARNING  NOT READY FOR PUBLIC USE

JC3VR is currently in active development. This repository contains unfinished, experimental, and potentially broken code, including features and implementations that have not yet been tested.

This is not an official playable release.

There is no installer.

There is no stable release.

There is no guarantee that the current code will compile or function correctly.

Features may be incomplete, broken, removed, or significantly changed throughout development.

Support is not currently provided for development builds.

About the Project

JC3VR is an independent, fan-made project aiming to bring an immersive virtual reality experience to Just Cause 3, including stereoscopic rendering, six-degrees-of-freedom tracking, motion-controlled weapons, and VR vehicle interactions.

Purpose of This Repository

This repository has been made public primarily for development transparency, source code preservation, and backup purposes. Its availability does not indicate that the project is ready for public use.

Public Release

A playable build will be made available when it is ready. There is currently no confirmed release date.

Development progress and release information will be shared when appropriate.

---

# JC3VR

**An experimental, unfinished VR mod for Just Cause 3**, developed by **meaty mold**.

JC3VR adds OpenXR stereo rendering, headset rotation and positional tracking, motion-controller weapon aiming, a left-hand grapple, gesture weapon selection, wrist HUD elements and experimental interactions to the Windows Steam game.

This is a development source release. Features, comfort, performance and compatibility are still being tested. The gameplay renderer uses alternate-eye rendering, which can produce temporal differences between eyes during fast movement. Frame rates vary substantially with the scene and hardware. Stop playing if you feel uncomfortable.

## Requirements

- A legitimate Steam installation of **Just Cause 3** on Windows x64.
- A PCVR headset and an active OpenXR runtime. Development testing primarily used Quest 3 through SteamVR and Steam Link.
- **RAM FFP (First Person Perspective) v0.1, installed separately.** This is the first-person mod used with JC3VR. Follow its author's installation instructions and download it from the [author's distribution page](https://www.nexusmods.com/justcause3/mods/14).
- Steam launch options that enable the first-person mod's `dropzone` files.

The executable supported by the current address guards has SHA-256
`a53c2d6b2837f45d97244fc096e68f27ba9036e0582a0d9aada2e2f222dac293`.
An unsupported executable disables the engine hooks by default. See [build fingerprints](docs/BUILD_FINGERPRINTS.md).

## Installation

Back up your saves and any existing mod files before installing a development build. Close JC3 before changing DLLs.

1. Install **RAM FFP (First Person Perspective)** separately. Its files belong directly inside the game's `dropzone` folder. Avoid nesting one `dropzone` folder inside another.
2. Build JC3VR using the instructions below, or use a separately published JC3VR binary package whose contents and checksums you trust.
3. Copy the built `dinput8.dll` beside `JustCause3.exe`, and `jc3vr.dll` into the game's `mods` folder. Preserve any existing DLLs before replacing files. An existing `dinput8.dll` may belong to another mod loader and requires a compatibility check.
4. Copy `packaging/jc3vr.ini` beside the game executable as `jc3vr.ini`. The example configuration documents additional options.
5. Add these Steam launch options:

```text
--vfs-fs dropzone --vfs-archive patch_win64 --vfs-archive archives_win64 --vfs-fs .
```

6. Start your PCVR connection and OpenXR runtime, then launch **through Steam**. Direct execution of `JustCause3.exe` failed the normal Steam-session check in development testing.

The resulting layout is:

```text
Just Cause 3/
  JustCause3.exe
  dinput8.dll
  jc3vr.ini
  mods/
    jc3vr.dll
  dropzone/
    ...files from your separately installed RAM FFP mod...
```

### Local game artwork

The game, RAM FFP files, weapon icons, textures, controller models and extracted Rico meshes have separate owners. Obtain these from your own installation or the original publisher. This repository distributes code and licensed programming dependencies.

The weapon selector needs an icon cache generated locally. With Python 3 and Pillow available:

```powershell
python tools/extract_weapon_icons.py "<JC3 game folder>" "<JC3 game folder>/mods/jc3vr_weapon_icons"
```

Additional optional original-game model preparation is documented in [local assets](docs/LOCAL_ASSETS.md). Original tracked Rico hands, pocket holstering, native contact probes, physical hijacking, independent pistol firing and scope behavior are experimental. Hands and contact probes are disabled by default. Physical object grabbing and vehicle pushing remain incomplete.

## Build from source

Use Visual Studio 2022 Build Tools with the x64 C++ toolchain, Windows SDK, and CMake 3.24 or later. Open an **x64 Native Tools Command Prompt**:

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

MinHook and the required OpenXR headers are included with their licenses. The OpenXR loader comes from your installed runtime.

Optional Python checks use:

```powershell
python -m pip install -r requirements-dev.txt
python -m unittest discover -s tests -v
python tools/audit_public_release.py
```

## Controls and troubleshooting

- **F2** recenters the headset. A simultaneous tap of both controller sticks also recenters.
- **F3** requests diagnostic eye captures during gameplay.
- Weapon selection uses the existing right-stick gesture selector.
- Experimental pocket mode uses a released-then-squeezed right grip near the right hip to holster or draw.
- Set `enabled=0` in `jc3vr.ini` to disable JC3VR's hooks on the next launch.

Logs and F3 captures are saved under `%LOCALAPPDATA%\JC3VR`. Review logs and screenshots for personal information before sharing them. If VR fails to start, confirm your runtime is active, the proxy and mod DLLs are loaded, the executable fingerprint matches, and the game was launched through Steam.

For third-person behavior, check the separate first-person dependency, launch options and folder nesting. Tracked hands can still have visibility and alignment problems. Fast movement, vehicles, scope rendering and runtime reconnects need further testing.

For removal, close JC3, move JC3VR's DLLs and INI out of the game folder, then restore your backups. Remove optional locally generated JC3VR assets separately. Follow RAM FFP's own uninstall instructions.

## License and credits

Original JC3VR contributions are available under the **MIT License**, copyright Â© 2026 **meaty mold**. The [third-party notices](THIRD_PARTY_NOTICES.md) preserve the licenses and attribution for MinHook, OpenXR, Philpax's JC3VR and engine-definition work, and format research.

Development has included assistance from OpenAI Codex and Anthropic Claude. This project is an independent, unofficial mod. Just Cause 3 and its artwork belong to their respective owners. The MIT license applies to the code covered by it.

See [release audit](docs/PUBLIC_RELEASE_AUDIT.md) for this source snapshot's scope and validation. Gameplay verification is still required for a playable release.
