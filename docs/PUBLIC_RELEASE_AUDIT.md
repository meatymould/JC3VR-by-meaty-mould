# Public source release audit · 2026-11-24

This public snapshot is based on gameplay checkpoint `05a274b5d334c31c4754799eb47ab39d36b38f40`. The complete private development repository and its working installation are preserved separately.

## Scope

- Examined all 454 commits reachable from the local development refs and all 2,592 historical blobs, totaling about 139 MB of file versions, for credential patterns, personal home paths, binary files and common game-asset extensions.
- Four initial credential-pattern hits were packaging denylist text. Review found no actual credential in those matches. The historical blob scan found no binary assets.
- Personal machine paths appeared in 339 historical text versions. Those histories are excluded from this public import. The destination repository's existing initial license commit is retained.
- Private journals, agent handoffs and raw local test accounts are excluded. The source, tools, tests and selected technical references are retained. Personal paths in the retained references are replaced by generic examples.
- Only the build dependencies needed for the gameplay source are vendored. MinHook BSD/HDE notices, OpenXR licensing and Philpax's MIT notice are included. The root copyright holder is meaty mold.
- Original game files, RAM FFP, extracted textures/icons, Rico models, controller artwork, generated asset caches, saved games, DLLs, PDBs and capture media are excluded.

## Functionality

Runtime source is imported from the gameplay branch, including the latest optional hands and native-contact diagnostics. The rendering backend remains the gameplay alternate-eye implementation. The separate native double-pass research branch is preserved in the private development repository.

Changes for publication cover licensing, dependency locations, portable tool paths, documentation, ignore rules, packaging and checks. Historical Git-revision tests are replaced by source-archive-compatible presentation checks and a SHA-256 manifest matching the imported gameplay sources.

All original preparation tools remain available for locally owned assets. Generated artwork is excluded from Git and source archives. First-person behavior requires a separate RAM FFP installation and the documented Steam launch options.

## Verification limits

Automated source scanning detects patterns and file types. It cannot establish ownership of every possible idea, exhaustively detect all secrets, or grant rights to game content. Third-party materials carry their own terms. Optional gameplay prototypes still require live testing.

Run `python tools/audit_public_release.py`, compile the Release build, run CTest, and run the Python checks before publishing further changes. The audit checks current publishable files and every reachable Git blob.

## Validation of this snapshot

- Fresh Windows x64 Release build succeeded using the vendored dependencies.
- All 11 CTests and 19 Python tests passed.
- The public file/history audit passed.
- Every runtime source file matched the imported gameplay source after line-ending normalization. Rendering, tracking, input and native hook behavior were preserved.
- The installed gameplay DLL and INI were checked after preparation and matched their pre-audit values.
- The repository publishes source. Optional gameplay systems retain their existing validation status.
