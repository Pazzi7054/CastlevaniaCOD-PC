# Castlevania: Curse of Darkness - XBE Recomp

An unofficial native PC port of the original Xbox version of Castlevania: Curse of Darkness, statically recompiled
into a 64 bit Windows executable. Rendering runs on Direct3D 12 and sound on XAudio2.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!IMPORTANT]
> This project does not include any game assets, game code or executables. You must provide the files from your own
> legally acquired copy of the game to build it. See [Legal](#legal).

## Required Game Version

Only the USA release is supported. The config is tied to the exact code addresses in its `default.xbe`, and the
built exe refuses any other `default.xbe`.

| | |
|---|---|
| Game | Castlevania: Curse of Darkness, original Xbox, **USA (NTSC-U)** |
| Title ID | `4B4E002D` (KN-045), version 1, XDK 5849 |
| `default.xbe` size | 7,352,320 bytes |
| `default.xbe` SHA-1 | `423BC19B7CED263570DCE5E7CD5D81954DFBE13C` |
| `default.xbe` MD5 | `92176FDDF071667F596680DF64DED7FE` |

Other regions (PAL, Japan) will not work. To check your copy, run `Get-FileHash assets\default.xbe -Algorithm SHA1`
in PowerShell.

## Installation

There are no prebuilt downloads: the executable contains code recompiled from the game, so it is built on your own
PC from your own copy (see [Building](#building)).

- Run `out\build\release\Castlevania.exe`.
- The game can run from the extracted files in `assets`, or from an ISO of the disc placed in `assets`.
- Recomp settings can be adjusted in `castlevania.toml` beside the exe. Saves go to `hdd\E\UDATA\4b4e002d`.

## Features

### Display
- **Any Resolution** - Renders at your display's resolution instead of 640x480, with `render_percent` for
  supersampling.
- **Display Modes** - Fullscreen, borderless or windowed, with a frame rate cap and vsync.

### Graphics
- **Native Direct3D 12 Renderer** - The game's Direct3D 8 calls, fixed function stages and paletted textures are
  drawn natively.
- **Anisotropic Filtering** - Up to 16x.

### Controller
Supports Xbox, PlayStation and Switch controllers, with hot plugging and rumble.

### Keyboard
The keyboard plays as player 1 while the window is in front:

| Keys | Pad |
|---|---|
| W A S D | left stick (move) |
| arrow keys | right stick (camera) |
| K or Space | A (jump) |
| J | X (attack) |
| L | B |
| I | Y |
| Q / E | left / right trigger |
| Z / C | white / black |
| Enter / Backspace | Start / Back |
| numpad 8 2 4 6 | d-pad |

### Miscellaneous
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.
- **Crash Dumps** - Written to a `CrashDumps` folder beside the exe.

## System Requirements

### Minimum
- **OS**
  - Windows 10 64-bit
- **GPU** with support for Direct3D 12

## Building

### 1. Clone the Repositories
Clone this repository and the SDK beside it with [Git](https://git-scm.com/), then apply this port's SDK patch:

```
git clone https://github.com/Pazzi7054/CastlevaniaCOD-PC
git clone https://github.com/TGP482/XboxXBERecomp XboxXBE-Recomp
git -C XboxXBE-Recomp checkout 99078b1
git -C XboxXBE-Recomp apply ..\CastlevaniaCOD-PC\sdk-patches\xboxxbe-recomp.patch
```

### 2. Add the Required Game Files
Extract your own disc and place its files in the `assets` folder: `default.xbe` with the `data`, `movie` and `sound`
folders beside it.

### 3. Install Dependencies
Install Visual Studio 2022 or newer with **Desktop development with C++** and **C++ CMake tools for Windows**, and
Clang (**C++ Clang tools for Windows**, or `winget install LLVM.LLVM`).

### 4. Build the Project
Run `build.bat`. The first build recompiles the game and takes a while. The exe is written to `out\build\release`.

## Port Notes

Changes this game needed beyond the generated config:

- `config\game.toml`, by hand: `D3DDevice_SetPalette`, `D3DDevice_DrawIndexedVerticesUP` and five DirectSound buffer
  methods (`SetPitch`, `Pause`, `PlayEx`, `SetDopplerFactor`, `SetRolloffFactor`) that no 5849 signature matched; the
  DirectSound ones are named from the image's `.XTLID` table.
- `sdk-patches\xboxxbe-recomp.patch`: host versions of `DrawIndexedVerticesUP` and the five DirectSound methods; a
  fixed-function stage with no texture that reads `D3DTA_TEXTURE` ends the stage cascade, as XDK D3D does (without it
  every single texture quad drew white); `XBE_VERBOSE` lets a release build log info lines.

### Testing
- `tools\run_test.ps1 -Name x -At 10,20 -Env "CV_PAD_SCRIPT=@tools\scripts\newgame.txt"` runs the game, screenshots
  it at the given seconds into `tools\shots`, and closes it.
- `CV_PAD_SCRIPT` presses buttons on a timeline (see `src\common\keyboard.cpp`).
- `XBE_VERBOSE=1` logs info lines to `logs\castlevania.log`; with `XBE_CAPTURE_DIR` set, F3 saves a frame with its
  draw list.

## Known Issues
- A band of streaks along the bottom edge of the 3D view.
- Only the first area has been tested so far.

## Legal

This is a non-commercial fan project for preservation and interoperability. It is not affiliated with, endorsed by
or sponsored by Konami or Microsoft.

- This repository contains only original source code, configuration and documentation. It contains **no** game
  assets, game code (original or recompiled), disc images, BIOS, keys or Xbox SDK libraries, and none may be added.
  `assets`, `generated` and all build output are excluded by `.gitignore`.
- `config\game.toml` lists addresses and names of functions in the game's executable. It contains no code from it.
- The recompiled code is generated on your own PC from your own copy of the game, and the resulting executable is
  for your personal use only. Do not distribute builds, generated code or game files.
- Castlevania and Curse of Darkness are trademarks of Konami Digital Entertainment. Xbox is a trademark of
  Microsoft Corporation. All other trademarks belong to their respective owners and are used only to identify the
  game this port works with.

The source in this repository is released under the [MIT License](LICENSE). The SDK patch in `sdk-patches` modifies
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) and is under its MIT License, Copyright (c) 2026 TGP482.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482, the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behaviour.
