# Third-party notices

Original JC3VR contributions are licensed under MIT, copyright (c) 2026 meaty mold. The following components and reference-derived portions retain their upstream copyrights and terms.

| Component | Scope | License and notice |
| --- | --- | --- |
| MinHook | Vendored C source under third_party/minhook, used for native hooks | BSD 2-clause license, Copyright (C) 2009-2017 Tsuda Kageyu. Its bundled HDE carries Copyright (c) 2008-2009 Vyacheslav Patkov and its own BSD notice. Full notices are retained in third_party/minhook/LICENSE.txt and source headers. |
| Khronos OpenXR headers | Vendored generated headers under third_party/openxr/include | Copyright 2017-2026 The Khronos Group Inc. Each included header identifies Apache-2.0 OR MIT. This distribution uses Apache-2.0, with the complete license in third_party/openxr/LICENSE. |
| Philpax, ferrobrew/jc3vr and ferrobrew/pyxis-defs | Reference-derived culling, reconstruction, camera, weapon/pose mechanisms and verified type/address definitions. Exact origins are recorded in relevant source comments. | MIT, Copyright (c) 2026 Philpax. Full notice in third_party/licenses/Philpax-MIT.txt. |
| Rick Gibbed and the tim42 JC3 tools fork | Offline file-format research used by independently implemented asset readers | Upstream zlib terms and fork notices preserved in third_party/licenses/Gibbed-Zlib.txt. The upstream tool implementation is excluded. |
| DECA and jc-model-renderer | File-format research references | Their source and binaries are excluded. |
| theHunterCotW-VR | Architecture research reference | GPL-3.0 project. Its source and binaries are excluded. No GPL implementation is intentionally incorporated in this release. |
| Luma Framework | Rendering research reference | Its source, shader fixes and binaries are excluded. |
| RAM FFP (First Person Perspective), original game and SteamVR artwork | Separate installation dependencies and local asset sources | Their assets are excluded. Obtain them from their respective authors or your installed software. |
| Geo-11, 3Dmigoto, Katanga and Osiris | Historical experiment references | Their source, wrapper DLLs, shaders and binaries are excluded. |

## Provenance

- MinHook source matches the existing gameplay dependency. Upstream repository [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook).
- OpenXR generated headers match the existing gameplay dependency. Upstream [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK).
- Philpax references [ferrobrew/jc3vr](https://github.com/ferrobrew/jc3vr) and [ferrobrew/pyxis-defs](https://github.com/ferrobrew/pyxis-defs).
- Format research [Gibbed JC3 tools fork](https://github.com/tim42/gibbed-justcause3-tools-fork), [DECA](https://github.com/kk49/deca), [JC Model Renderer](https://github.com/aaronkirkham/jc-model-renderer).

Locally generated icons, DDS images, controller models, charge meshes and Rico body models remain subject to their owners' terms. Running a preparation tool does not grant redistribution rights. They must be excluded from source and binary packages.
