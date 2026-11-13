# TotK Recomp

**TotK Recomp** is a static recompilation of *The Legend of Zelda: Tears of the Kingdom* (Nintendo Switch, 2023) into a native x86-64 application for Windows and Linux ([docs/architecture.md](docs/architecture.md)).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tears of the Kingdom*, dumped from your own console: the base game and the 1.1.0, 1.2.1 or 1.4.3 update. Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## About this fork

This is a fork of [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) (MIT) that adds AMD upscaling and frame generation, a native Linux FSR 4 path, and some tools for finding and trying out rendering fixes. It is built on upstream's `main` and everything upstream has is still here; the changes below are on top.

### Taken from other people's work

These are not mine. They came from open pull requests and another fork, merged here as they were:

| Change | From |
| --- | --- |
| AMD FSR 4 upscaling as a second DLAA backend, FSR shown in the performance overlay (upstream PR #876) | [VictorSCosta](https://github.com/VictorSCosta) |
| Cross-building the Windows exe from Linux, `build_game.py --build-windows` (upstream PR #906) | [Hatsune-Cthulhu](https://github.com/Hatsune-Cthulhu) |
| AMD FSR 3.1 upscaling (`fsr3`), motion blur (`motion_blur`), FSR 3.1 frame generation (`frame_gen`) with the HUD kept out of generated frames, frame generation doubling the frame limit (upstream PRs #1084 and #1089; the commits are authored as Claude) | [valentderah](https://github.com/valentderah) |
| Running The Second Quest on game version 1.4.3: its MSFix plugin as host hooks, its IPS data write, and mods without a code module | Eric Dallo, carried on [itaskaev2](https://github.com/itaskaev2/totk-pc-recomp)'s fork (`second-quest-msfix`) |

Thanks to all of them, and to everyone credited in upstream's history for the rest of the code.

### Added in this fork

Written with Claude, tested on an AMD RX 7900 XT (RDNA 3) on Linux, and on Windows through Proton.

- **FSR 4 on native Linux.** The runtime's FSR path is no longer Windows only. On Linux it loads `amd_fidelityfx_upscaler_vk.so` from [fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) next to the game. `tools/build_fsr4_vk.sh --dest <game folder>` builds that library on your machine; nothing of fsr4-vk is in this repository (it is GPL-3.0 and is only loaded at run time). Setup: [docs/building.md](docs/building.md), "FSR 4 on Linux".
- **FSR 4 controls.** `fsr_model` and `fsr_version` choose the FSR 4 network and pin a version (with menu rows), and the `sharpness` setting now reaches FSR 4.
- **Frame generation providers.** `fg_provider` picks AMD's FSR 3.1 frame generation or the FSR 4 ML one, and frame generation works with HDR output (a float compose pass). ML frame generation needs a GPU with FP8 and exits at load on RDNA 3, which `totk.ini.example` says. Frame generation is Windows only: AMD ships no Linux provider.
- **`shadow_split` and `tex_lod_bias`** settings.
- **Menu:** the Display tab follows the chosen upscaler.
- **Debugging tools** ([docs/agents/running.md](docs/agents/running.md)): `TOTK_DUMP=w<seconds>` (a frame dump at a world-clock time, repeatable in any run), `TOTK_SKIP_FS` (skip a fragment shader to see what it contributes), `TOTK_FG_SHOTS` (generated frames), and logs that say why frames were not generated.
- **Capture keys.** `F10` saves the current frame as a full-size PNG; `Shift+F10` also dumps every render pass of the next frame, so a glitch seen while playing can be reproduced. The file names carry the world-clock time. Tested on Linux; the Windows key path is written but not run.
- **Shader override.** A folder `shader_override` next to `totk` (or `shader_override = <folder>` in `totk.ini`) holds replacements for game fragment shaders, named by their SPIR-V hash, so a rendering fix can be tried without a rebuild; `shader_override = 0` turns it off. `tools/make_skin_override.py` uses it to give the character skin shader soft form shading: the game lights skin with a hard toon step and nothing inside the shaded side, which leaves a face turned from the sun one flat tone. It needs `spirv-cross` and `glslangValidator`. The override files derive from the game's shaders, so they stay on your machine. This is experimental, and it does not change the first-person hand, the clouds or the pause-menu blur.
- **Faster scenes with many barriers (`barrier_merge`, experimental, off by default).** Some places make the game issue about a thousand texture barriers a frame (the Nachoyah Shrine waterfall); each one ended the Vulkan render pass. With `barrier_merge = 1` the pass stays open where that is safe: at that waterfall 1236 passes became 130 and the GPU time fell from 24.5 to 7.8 ms a frame. Screenshots differ from the default at edges by about a pixel, partly from frame timing, so report anything odd in water, decals or particles.
- **Quieter console (`log_verbose`).** Periodic statistics and routine diagnostics now go only to `logs/totk.log`; `log_verbose = 1` prints them on the console too. Bug reports still get every line.
- **MAKO helper (Linux).** `tools/mako_run.sh [game folder]` (the current folder when it holds `totk`) runs the game with the [MAKO](https://github.com/eugeniosegala/MAKO) Vulkan layer for frame generation on Linux. The script downloads MAKO's own release (into `build/mako` in the repository, otherwise `~/.cache/totk-mako`), checks its SHA-256 and starts the game with the layer enabled. This repository contains no MAKO code (it is GPL-3.0-or-later); the script turns the game's own frame generation off (`TOTK_FRAME_GEN=0`), because two generators cannot share a swapchain.

## Status

Early development. The game is playable on Windows and Linux; visual bugs and crashes are still expected.

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- Game updates 1.1.0, 1.2.1 and 1.4.3; 1.2.1 and 1.4.3 saves carry over between them
- Takes NSP and XCI game files, and compressed NSZ and XCZ as they are: the setup unpacks them itself
- 60 fps by default; up to 144 fps or the monitor's refresh rate (experimental)
- Custom resolutions (1080p, 2k...), supersampling and AMD FSR 1 upscaling
- NVIDIA DLSS / DLAA on RTX GPUs: the setup downloads `nvngx_dlss.dll` from NVIDIA's DLSS SDK repository
  (github.com/NVIDIA/DLSS, NVIDIA RTX SDK license) when it finds an RTX GPU; untick it in the setup to skip it
- AMD FSR 4 on Radeon RX 9000, 7000 and 6000 GPUs (FSR 3.1 on others), in the same modes as DLSS: the setup downloads
  AMD's signed FSR SDK libraries (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, AMD FSR SDK license) when it
  finds an AMD GPU; pick *Upscaler: AMD FSR* in the menu. On Linux AMD ships no libraries: FSR 4 comes from the
  independent open-source fsr4-vk, which `tools/build_fsr4_vk.sh` builds on your machine (see
  [docs/building.md](docs/building.md#fsr-4-on-linux-fsr4-vk))
- HDR10 output with peak, game and HUD brightness settings
- 16:10 (Steam Deck, handhelds), 3:2 (Surface, Framework), 21:9 and 32:9 (ultrawide) game resolutions, with the camera and the interface in that shape
- Anisotropic texture filtering up to 16x, for sharp ground into the distance
- In-game settings menu (F1) and performance overlay (F10)
- Play time across runs on the Game tab of the settings menu, kept in `playtime.txt` next to the saves
- Discord Rich Presence ("Playing TotK Recomp" with the play time), off by default, own application id supported
- Setup and in-game menu in English, Russian, Brazilian Portuguese and Simplified Chinese
- Keyboard, mouse and gamepad with remapping, rumble and gyro
- Stereo or 5.1 surround sound output
- Xbox button prompts: LB, RB, LT, RT, Menu and View, and A, B, X and Y as labelled on the button to press, with either button layout
- Shaders prepared at install instead of during play
- Mods made for emulators: unpack a mod into the `mods` folder next to `totk.exe`; switch and order them in the menu. RomFS
  mods, TKMM's merged output and ExeFS code patches (`.ips`, `.pchtxt`) work; the game rebuilds itself for code patches
  ([docs/mods.md](docs/mods.md)). HD texture packs work: the port grows the texture heap a pack asks for by itself. Tested
  with the HDPacks.com 4K Texture Pack v1.0 on 1.4.3 and 1.2.1; other packs come with no guarantee
- Atmosphère cheats: put a cheat file into a mod and switch its cheats in the menu ([docs/cheats.md](docs/cheats.md))

## Planned

- VRR

## Installing

### Windows

Requirements: Windows 10 or later, an x86-64 CPU with SSE4.2 (AVX2/FMA machines get a faster build), a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, about 36 GiB free during setup (20 GiB after).

1. Download `TotK-Setup-<version>-windows.zip` from [Releases](../../releases/latest) and unpack it.
2. Start `TotK-Setup.exe`. If SmartScreen warns about an unsigned app, choose *More info*, then *Run anyway*.
3. Pick your game files, the language and an install folder, and press *Start*. The setup builds the game once. With
   an NVIDIA RTX GPU it also downloads NVIDIA DLSS, unless you untick *Download NVIDIA DLSS*; turn DLSS on in the
   menu (F1). With an AMD GPU it downloads AMD FSR the same way; in the menu turn on *NVIDIA DLSS / DLAA* and pick
   *Upscaler: AMD FSR*.
4. Play `totk.exe` from the install folder.

### Linux

Requirements: as on Windows, plus Linux 6.7 or later and glibc 2.34 or later (Ubuntu 22.04, Debian 12, Fedora 35, SteamOS 3).

1. Download `TotK-Setup-x86_64.AppImage` from [Releases](../../releases/latest), or build it ([docs/installer.md](docs/installer.md), "Linux release").
2. Make it executable (`chmod +x TotK-Setup-x86_64.AppImage`) and start it. On a Steam Deck, do this in Desktop Mode.
3. Pick your game files, the language and an install folder, and press *Start*.
4. Play `totk` from the install folder.

To update, run a newer setup on the same folder; saves and settings are kept. Settings are in the in-game menu and in `totk.ini` ([totk.ini.example](totk.ini.example) lists every key). Controls: [docs/controls.md](docs/controls.md).

## Credits

TotK Recomp builds on the projects below. What was taken from each, with its license, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [LAS Recomp](https://github.com/peiikai/LASRecomp)
- [MacNativeGames](https://github.com/ly05010419/MacNativeGames)
- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [nxnative](https://github.com/seedds/nxnative)
- [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)
- [Ryujinx](https://github.com/Ryujinx/Ryujinx)
- [deko3d](https://github.com/devkitPro/deko3d)
- [hactool](https://github.com/SciresM/hactool)
- [SDL 3](https://libsdl.org), [Dear ImGui](https://github.com/ocornut/imgui), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [libopus](https://opus-codec.org), [libvpx](https://chromium.googlesource.com/webm/libvpx), [Zstandard](https://github.com/facebook/zstd)
- [Switchbrew](https://switchbrew.org) and the Switch homebrew community
- This fork also uses [fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) by Astyyyyy (a separate program, loaded at run time, GPL-3.0), [MAKO](https://github.com/eugeniosegala/MAKO) by eugeniosegala (a separate program, downloaded by a helper script, GPL-3.0-or-later), AMD's FidelityFX SDK, and [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) and [glslang](https://github.com/KhronosGroup/glslang) as command-line tools. None of their code is in this repository.

## License

The project's own code is under the [MIT License](LICENSE); third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The license grants nothing for Nintendo's game.

TotK Recomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo. *The Legend of Zelda*, *Tears of the Kingdom* and *Nintendo Switch* are trademarks of Nintendo.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. Everything the build writes stays under `build\` and `generated\`, which git ignores; run `tools/check_content.py` before a commit. Testers: [docs/tester-guide.md](docs/tester-guide.md).

Most of the code and documentation is written by AI coding agents (Claude Code) under the maintainer's direction; the maintainer plays every build and decides what is merged. [AGENTS.md](AGENTS.md) holds the rules they work under.
