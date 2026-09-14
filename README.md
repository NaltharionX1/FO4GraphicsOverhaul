# FO4GraphicsOverhaul

A graphics overhaul for Fallout 4, built as an F4SE plugin. It runs NVIDIA DLSS and AMD FSR on a private DirectX 12
layer beside the game's DirectX 11 renderer, adds frame generation, its own ambient occlusion, bloom and colour
grading, and drives all of it live from an in-game menu.

## Features

- **Upscaling and anti-aliasing:** NVIDIA DLSS (Super Resolution / DLAA) or AMD FSR 3.1, switchable live, with
  supersampling above native and RCAS sharpening.
- **Frame generation:** DLSS Frame Generation from 2x to 6x (multi frame generation on RTX 40 and RTX 50), a dynamic
  multiplier that holds a target output rate, and AMD FSR Frame Generation as a second backend.
- **DLSS 5 Neural Rendering filter:** up to three chained passes beside DLSS or FSR. Its runtime is not distributed
  with this mod; without it the filter reports itself unavailable.
- **Ambient occlusion, bloom and grading:** Intel XeGTAO in the engine's own AO slot, plus bloom and colour grading
  passes over the finished image.
- **Low latency:** NVIDIA Reflex, with its sleep and the frame limiter placed on the game thread right before input.
- **Display:** a high-resolution frame limiter (general and loading-screen caps) and an optional fix for G-Sync /
  variable-refresh flicker under a frame cap.
- **Engine fixes:** physics and timing fixes, and motion-vector corrections for temporal techniques.
- **In-game menu and overlay:** every setting live, an on-screen display, and a diagnostics tab.

## Requirements

- Fallout 4 **1.10.163.0** (the pre-next-gen version). Other versions are refused at load.
- [F4SE](https://f4se.silverlock.org/) 0.6.23.
- [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327), the 1.10.163 version.
- Windows 10 or 11, x64, a GPU with DirectX 12 support.
- NVIDIA RTX for DLSS, DLSS Frame Generation and Reflex; RTX 40 or newer for frame generation above 2x. FSR and FSR
  Frame Generation run on any DirectX 11 GPU with typed-UAV support.

Not compatible with ENB. The game always runs borderless fullscreen: exclusive fullscreen is not supported. If High
FPS Physics Fix is installed, both coexist and this plugin's settings win where they overlap. If the standalone
Motion Vector Fixes plugin is installed, this plugin's copy of those fixes stands down.

## Installation

Install the release archive with a mod manager, or extract its `Data` folder into the game's `Data` folder:

```
Data/F4SE/Plugins/FO4GraphicsOverhaul.dll
Data/F4SE/Plugins/FO4GraphicsOverhaul/NGX/        caller shim
Data/F4SE/Plugins/FO4GraphicsOverhaul/Streamline/ NVIDIA DLSS runtimes and their licence
Data/F4SE/Plugins/FO4GraphicsOverhaul/FidelityFX/ AMD frame generation runtimes
Data/F4SE/Plugins/FO4GraphicsOverhaul/LICENSES/
```

Settings are written to `Data/F4SE/Plugins/FO4GraphicsOverhaul.ini` and `FO4GraphicsOverhaul.DLAA.ini` on first
launch. The menu opens with Insert by default; the key can be changed in the menu.

## Building from source

Prerequisites: Visual Studio 2026 with the C++ desktop workload, CMake 3.25 or newer, the Windows 10/11 SDK (for
`fxc.exe`), Git, and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```powershell
git clone https://github.com/NaltharionX1/FO4GraphicsOverhaul.git
cd FO4GraphicsOverhaul
./scripts/setup.ps1
cmake --preset release
cmake --build --preset release
ctest --preset release
```

`setup.ps1` fetches every library in `dependencies.json` at its pinned commit into `extern/`, applies the patches in
`patches/`, and downloads NVIDIA's DLSS runtimes from the official Streamline SDK release into `runtime/`, verifying
each file's SHA-256. The build writes the installable package to `dist/Data/`. Some tests exercise the GPU and the
NVIDIA driver and need a matching machine.

## Dependencies

| Dependency | Version | Licence | Used for |
|---|---|---|---|
| [CommonLibF4](https://github.com/shad0wshayd3-FO4/CommonLibF4) | `2e5b661` (pre-next-gen line) + 2 fixes | MIT | Game API, Address Library |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.8 | MIT | Menu and overlay |
| [NVAPI](https://github.com/NVIDIA/nvapi) | R615 (`a08f45f`) | MIT | Reflex, display queries |
| [XeGTAO](https://github.com/GameTechDev/XeGTAO) | `a5b1686` | MIT | Ambient occlusion |
| [detours](https://github.com/Nukem9/detours) (with Zydis) | `cc5a2e4` | MIT | Function hooks |
| [FidelityFX SDK DX11 port](https://github.com/alandtse/FidelityFX-SDK-DX11) | `054f0ad` + patches | MIT | FSR 3.1 upscaler |
| [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | 2.3.0 | MIT / AMD redistributable | FSR frame generation |
| [NVIDIA Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) runtimes | 2.14.1 (DLSS 310.9.1) | NVIDIA DLSS SDK licence | DLSS runtimes |
| vcpkg: fmt, spdlog, boost-stl-interfaces, rsm-mmio, xbyak, args, frozen, nowide, robin-hood-hashing, srell, catch2 | vcpkg baseline `98aa639` | MIT / BSL-1.0 / BSD / Apache-2.0 | Support libraries, tests |

## Credits

- **Naltharion**: creator, design and engineering.
- **Claude (Anthropic)**: engineering collaborator.
- **AntoniX35** for the physics and timing fixes from High FPS Physics Fix.
- **doodlum** for Motion Vector Fixes.
- **northaxosky** and the Community Shaders contributors.
- **Ryan McKenzie** and the CommonLibF4 contributors.
- **Nukem** for detours; **Florian Bernd** and **Joel Höner** for Zydis.
- **Omar Cornut** for Dear ImGui.
- **ImDreamt** for MFGAdaUnlock-RenoDx.
- **NIGos** for dlss5-bridge, and the **ComfyUI-DLSS5-NR** contributors.
- **alandtse** for the FidelityFX SDK DirectX 11 port.
- **NVIDIA**, **AMD** and **Intel** for their SDKs.

## Licence

GNU General Public License v3.0 or later, with the modding and linking exceptions in `EXCEPTIONS.md`. See `LICENSE`
and `THIRD-PARTY-NOTICES.md`.
