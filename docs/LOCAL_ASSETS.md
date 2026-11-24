# Assets prepared from your own game

JC3VR reads or prepares some original artwork locally. Generated caches and modified game resources must stay outside the public repository and source archive.

## Weapon icons

Run `tools/extract_weapon_icons.py` against your own JC3 installation to create `mods/jc3vr_weapon_icons`. The selector reads that cache. This tool also reads the game's gamepad key map. The original game archives stay in place.

## Explosive model

`tools/prepare_c4_asset.py` prepares the original held charge mesh and diffuse image into a fresh output folder. It checks the installed bundle hash. The resulting `c4_inactive.mesh` and `c4_diffuse.png` can be placed in `mods/jc3vr_assets` for local testing. Pillow and NumPy are required for its preview.

## Experimental Rico hands

`tools/prepare_rico_hands.py "<game folder>" "<fresh output folder outside the game>"` creates three hand-and-cuff model overrides from the exact supported patch archive. It rejects an archive fingerprint mismatch. Back up any loose Rico models before using the generated `dropzone` files.

`rico_native_hands=1` requires these three matching files and the existing body/tracking configuration. `rico_pocket_holster=1` enables the experimental right-pocket squeeze gesture. `native_grab_probe=1` observes contacts and records bounded diagnostics. Holding objects through native physics remains incomplete.

The hand model override also affects other views using the same Rico body model, including cutscenes. Restoring the original DLL alone leaves the loose model overrides in place. Restore their backups or move the test overrides out of the game to finish rollback.

## First-person dependency

Install RAM FFP separately from its author. Its license and redistribution permissions are separate from JC3VR. Development used a local configuration where its loose Rico body override had been disabled to preserve visible skin while JC3VR handled body hiding. If you use the experimental native hand models, preserve and restore that asset conflict deliberately.

SteamVR controller art is loaded from the user's runtime installation. That art is also excluded from the public source tree and generated packages.
