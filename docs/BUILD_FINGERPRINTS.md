# Known JC3 builds

## Steam x64 build observed 2026-09-20

| Field | Value |
|---|---|
| Executable | `JustCause3.exe` |
| Full path | `D:\SteamLibrary\steamapps\common\Just Cause 3\JustCause3.exe` |
| File size | 51,083,264 bytes |
| SHA-256 | `A53C2D6B2837F45D97244FC096E68F27BA9036E0582A0D9AADA2E2F222DAC293` |
| PE timestamp | `0x5787641C` |
| PE image size | `0x03241000` |
| Architecture | x64 |

The import table directly imports `DINPUT8.dll` and only `DirectInput8Create`; it also imports `dxgi.dll`, `d3d11.dll`, `d3d9.dll`, and `XINPUT9_1_0.dll`. At inspection time, the game root contained no `dinput8.dll`, `dxgi.dll`, `d3d11.dll`, ReShade, Geo-11, Katanga, or JC3 DLL-mod-loader files. `dinput8.dll` is therefore the selected non-graphics-wrapper proxy for this exact build.

No engine address is approved by this fingerprint. It identifies only the executable on which future signatures/addresses must be discovered and validated.
