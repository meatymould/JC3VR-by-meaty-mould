# Geo-11 test profile

Install the current JC3-specific Geo-11 fix independently, never into this repository. Before running, capture the exact fix commit/release, `d3dx.ini` settings, injected DLL names, output mode, game resolution, driver version, and executable SHA-256 in `docs/KNOWN_GOOD_PROTOTYPE.md`.

Requested first output: full-SBS/Katanga. Confirm stereo on a desktop first, then pass it to Osiris. Do not combine this with Luma until each stack works alone because both can contend for graphics-wrapper DLLs.
