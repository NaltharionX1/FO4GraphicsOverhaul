# Third-party notices

FO4GraphicsOverhaul is licensed under the GNU General Public License v3.0 or later with the exceptions in
`EXCEPTIONS.md`. It incorporates, links against, or redistributes the following works. Their licence texts are in
`LICENSES/` (and ship in the release package under `F4SE/Plugins/FO4GraphicsOverhaul/LICENSES/`).

## Code adapted or ported into this project

| Work | Copyright | Licence | Used in | Licence text |
|---|---|---|---|---|
| [High FPS Physics Fix](https://github.com/AntoniX35/High-FPS-Physics-Fix) | 2025 AntoniX35 | MIT | Physics and timing fixes, present and window policy | `LICENSES/HighFPSPhysicsFix-LICENSE.txt` |
| [Motion Vector Fixes (fo4test)](https://github.com/doodlum/fo4test) | doodlum | GPL-3.0-or-later with modding exception | Motion-vector corrections; the renderer's jitter, sampler bias, dynamic-resolution drive and in-place DLAA model | `LICENSE`, `LICENSES/fo4test-LICENSE`, `LICENSES/fo4test-EXCEPTIONS.md` |
| [Community Shaders for Fallout 4](https://github.com/northaxosky/fallout4-community-shaders) | northaxosky and contributors | GPL-3.0 | Upscaler integration details, FSR masks, first-person conditioning, motion-vector dilation, the RCAS sharpness mapping | `LICENSE` |
| MFGAdaUnlock-RenoDx | 2026 ImDreamt | MIT | RTX 40 multi frame generation timing correction | `LICENSES/MFGAdaUnlock-RenoDx-LICENSE` |
| dlss5-bridge | 2026 NIGos | MIT | DirectX 12 NGX session handling | `LICENSES/dlss5-bridge-LICENSE` |
| ComfyUI-DLSS5-NR | 2026 ComfyUI-DLSS5-NR contributors | MIT | Caller shim | `LICENSES/ComfyUI-DLSS5-NR-LICENSE` |

Files containing adapted or ported code carry an SPDX identifier and the upstream notice at the top.

## Libraries fetched at their pinned commits (`dependencies.json`)

| Library | Copyright | Licence | Local changes | Licence text |
|---|---|---|---|---|
| CommonLibF4 (pre-next-gen line) | 2019 Ryan McKenzie and contributors | MIT | Two bug fixes, `patches/CommonLibF4/` | `LICENSES/CommonLibF4-LICENSE` |
| Dear ImGui 1.92.8 | 2014-2026 Omar Cornut | MIT | None | `LICENSES/imgui-LICENSE` |
| NVIDIA NVAPI R615 | NVIDIA Corporation | MIT | None | `LICENSES/NVAPI-LICENSE.txt` |
| XeGTAO | 2016-2021 Intel Corporation | MIT | Shader adapted for fxc in `shaders/XeGTAO_fxc.hlsli` | `LICENSES/XeGTAO-LICENSE.txt` |
| detours | 2019 Nukem | MIT | None | `LICENSES/detours-LICENSE` |
| Zydis and Zycore (bundled with detours) | 2014-2019 Florian Bernd, Joel Höner | MIT | None | `LICENSES/Zydis-LICENSE`, `LICENSES/Zycore-LICENSE` |
| FidelityFX SDK, DirectX 11 port | Advanced Micro Devices, Inc.; port by alandtse | MIT | Robustness fixes to the DX11 backend, `patches/FidelityFX-SDK-DX11/` | `LICENSES/FidelityFX-SDK-DX11-LICENSE.txt`, `LICENSES/FidelityFX-SDK-DX11-THIRD-PARTY-NOTICES.txt` |
| FidelityFX SDK 2.3.0 (API headers and signed runtime DLLs) | Advanced Micro Devices, Inc. | MIT; signed DLLs under AMD's redistribution terms | None | `LICENSES/FidelityFX-SDK-LICENSE.md`, `LICENSES/FidelityFX-SDK-THIRD-PARTY-NOTICE.md` |

## Libraries from vcpkg

fmt (MIT, `LICENSES/fmt-LICENSE`), spdlog (MIT, `LICENSES/spdlog-LICENSE`), Boost (Boost Software License 1.0,
`LICENSES/Boost-LICENSE`), Xbyak (BSD-3-Clause, `LICENSES/Xbyak-LICENSE`), rsm-mmio (MIT, `LICENSES/rsm-mmio-LICENSE`).
Also resolved through the manifest: args, catch2 (tests only), frozen, nowide, robin-hood-hashing and srell, each
under its own upstream licence.

## Runtimes downloaded at setup, never stored in this repository

NVIDIA DLSS 310.9.1 (`nvngx_dlss.dll`, `nvngx_dlssg.dll`) from NVIDIA's public Streamline SDK 2.14.1 release, under
the NVIDIA DLSS SDK licence. `nvngx_dlss.license.txt` travels with the files in the release package.

The DLSS 5 Neural Rendering runtime (`nvngx_dlssnr.dll`) is not part of this project and is not distributed with it.

## The game

Fallout 4, its executable and its data are the property of Bethesda Softworks. This plugin patches the running game
and ships none of its files.
