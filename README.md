# Castlevania: Curse of Darkness - XBE Recomp

An unofficial native PC port of the original Xbox version of Castlevania: Curse of Darkness, statically recompiled
into a 64 bit Windows executable. Rendering runs on Direct3D 12 and sound on XAudio2.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!IMPORTANT]
> This project does not include any game assets or game code. You must provide the files from your own legally
> acquired copy of the game. See [Legal](#legal).

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

1. Download the latest release from [Releases](https://github.com/Pazzi7054/CastlevaniaCOD-PC/releases) and extract it
   anywhere.
2. Run `Castlevania.exe`. The `assets` folder opens.
3. Put your game in it: the disc image (`.iso`), or the extracted disc (`default.xbe` with its `data`, `movie` and
   `sound` folders).
4. Run `Castlevania.exe` again. The first start builds the game code from your copy, which takes a minute or two and
   shows its progress. After that the game starts straight away.

A release contains no game code. `Castlevania.exe` holds the runtime, and the first start makes `CastlevaniaGame.dll`
from your own `default.xbe` with the recompiler, [Clang and LLD](https://llvm.org) in the `compiler` folder.

- `CastlevaniaLauncher.exe` has every setting and checks your game files; `Castlevania.exe` skips it.
- The game can run from the extracted files in `assets`, or from an ISO of the disc placed in `assets`.
- Recomp settings can be adjusted in `castlevania.toml` beside the exe. Saves go to `hdd\E\UDATA\4b4e002d`.

## Features

### Launcher
`CastlevaniaLauncher.exe` checks your game files (the USA `default.xbe` by its hash, or a disc image), adds a disc
image to `assets` for you (a hard link on the same drive, otherwise a copy), and has every setting: display mode,
resolution, v-sync, widescreen, render scale, anisotropic filtering with quick presets, FPS counter, pause on focus
loss and the settings menu key. Changes are saved as you make them; **Play** starts the game. Saves and logs are a
click away.

### Settings Menu
Press **F10** in game for the settings menu, or click both sticks in (**L3 + R3**) on a controller. The key can be
changed in the launcher (Game tab), for keyboards where F10 needs Fn or is taken by another program. Every change
applies at once and is saved to `castlevania.toml`. While the menu is open the game gets no input; Escape, the menu
key or L3 + R3 closes it.

### Display
- **Any Resolution** - Renders at your display's resolution instead of 640x480.
- **Display Modes** - Fullscreen, borderless or windowed, with vsync.
- **Widescreen** - On a 16:9 or wider display the game is shown 16:9 with more of the world at the sides (Hor+),
  without stretching. The HUD, menus and movies stay 4:3 in the middle. Turn it off in the menu (Graphics) for the
  original 4:3 picture.
- **No Overscan Streaks** - The rows a TV's overscan used to hide at the top and bottom of the picture no longer
  smear; the frame fills the screen.

### HD Texture Packs
Texture packs made for PCSX2 work as they are: put a pack's replacement pictures in a `textures` folder beside the
game (folders inside it are fine). The Xbox release kept the PS2's textures, so each one is matched by the same
hashes PCSX2 names the pictures by. Pictures load in the background with full mipmaps, scaled to the size set in the
launcher (1024, 2048 by default, or full size). Paletted textures are replaced; region (`-rWxH`) and true colour
pictures are not used yet.

### Graphics
- **Native Direct3D 12 Renderer** - The game's Direct3D 8 calls, fixed function stages and paletted textures are
  drawn natively.
- **Render Scale** - 50% to 200% of the output height; above 100% supersamples.
- **Anisotropic Filtering** - Up to 16x.

### General
- **FPS Counter** - Optional, in the corner.
- **Pause on Focus Loss** - The game stops while another window is in front (on by default).

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
| F10 (changeable) | settings menu (L3 + R3 on a controller) |

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
Run `build.bat`. The first build recompiles the game and takes a while. `Castlevania.exe` and
`CastlevaniaLauncher.exe` are written to `out\build\release`, with the game code built in.

### 5. Make a Release
`release.bat 1.0.0` writes `out\release\CastlevaniaCOD-PC-1.0.0.zip`: `Castlevania.exe` without the game code, the
launcher, and the `compiler` folder its first start builds the game code with on the player's PC. It needs Python,
and checks that nothing of the game went into the package.

## Port Notes

Changes this game needed beyond the generated config:

- `config\game.toml`, by hand: `D3DDevice_SetPalette`, `D3DDevice_DrawIndexedVerticesUP` and five DirectSound buffer
  methods (`SetPitch`, `Pause`, `PlayEx`, `SetDopplerFactor`, `SetRolloffFactor`) that no 5849 signature matched; the
  DirectSound ones are named from the image's `.XTLID` table.
- `sdk-patches\xboxxbe-recomp.patch`: host versions of `DrawIndexedVerticesUP` and the five DirectSound methods; a
  fixed-function stage with no texture that reads `D3DTA_TEXTURE` ends the stage cascade, as XDK D3D does (without it
  every single texture quad drew white); `XBE_VERBOSE` lets a release build log info lines; a `widescreen` setting;
  for renderer extensions, `ShaderHooks::Position` (HLSL after the vertex position is set), `Extension::Vertices`
  (each draw's assembled vertices) and `gpu::ReadsConstant`; the DirectSound mixer plays every channel of a voice,
  not only the first two (cutscene audio is a 5.1 stream with the voices in the centre channel, which was silent).
- Releases (`release.bat`, `split\`, `src\common\setup.cpp`): the game code lives in `CastlevaniaGame.dll`, built on the
  player's PC on the first start. `split\glue.py` reads a whole build's objects and writes both sides of the split:
  the DLL forwards the runtime's functions and the 240 game functions the host replaces to `Castlevania.exe` through
  a table it is handed, and the exe reaches the 7 recompiled originals it calls and fills the function table from
  the DLL. The recompiled code compiles against small freestanding headers (`split\include`) and links against
  import libraries made from `split\crt` for `vcruntime140.dll` and `ucrtbase.dll`, so no Visual Studio is needed on
  the player's PC. A DLL whose stamp does not match the exe is built again.
- `src\common\menu.cpp`: the menu key's state is read every frame instead of waiting for its key messages, which an
  overlay or another program's hotkey can take first; the messages are still swallowed so F10 never puts the window
  in menu mode.
- `src\launcher`: the launcher, Direct3D 11 and Dear ImGui, no game code; it checks `default.xbe` against the SHA-1
  the build wrote for `Castlevania.exe`.
- `src\common\texpack.cpp`: PCSX2 texture packs. A P8 texture's indices are put back into GS memory order, 16x16
  (8 bit) or 32x16 (4 bit, indices under 16) blocks in the GS's column layout, and hashed with XXH3-64 block by
  block as PCSX2 does; its palette is hashed as the GS holds it (R G B A, alpha halved). The pair names the picture.
  PNGs load on worker threads through WIC with their PS2 alpha (128 opaque) doubled. The SDK patch adds
  `Extension::Replace` and `Replaced`, `gpu::PaletteIndices`, and the backend draws a replacement with every level.
- `src\game\hooks.cpp`: widescreen culling. The camera (`sub_00012CD0`, a PS2 libvu0 style camera) makes its
  view screen matrices and the clip data objects are culled against; with widescreen on, the clip data's x scale
  (`0x012FC500`, `0x012FC5C0`, `0x012FC640`, `0x012FC660`) is made 3/4, so objects in the wider view are drawn
  instead of popping in at the edges. The matrices the scene is drawn with are untouched.
- `src\common\widescreen.cpp`, a renderer extension:
  - Widescreen. The game projects with a PS2 style view screen matrix (`c68`, or `c85` with the world matrix) and
    maps PS2 screen space to console pixels with `c94`/`c95`. Each draw into the back buffer is squeezed to 3/4 of its
    width around the centre: for draws that use the camera that is a wider field of view, for 2D draws a 4:3 shape.
    Draws over the whole width stay whole when they are untextured (fades) or read a render target (the frame pass).
  - Overscan. The last pass of each frame draws a copy of the frame back with its rows -16 to 496 over rows 0 to 480
    (PS2 448 lines plus border shrunk into the NTSC frame); the 16 rows past each edge clamped into streaks. The pass
    is drawn that much taller, so the copy fills the frame row for row.

### Testing
- `tools\run_test.ps1 -Name x -At 10,20 -Env "CV_PAD_SCRIPT=@tools\scripts\newgame.txt"` runs the game, screenshots
  it at the given seconds into `tools\shots`, and closes it.
- `CV_PAD_SCRIPT` presses buttons on a timeline (see `src\common\keyboard.cpp`).
- `XBE_VERBOSE=1` logs info lines to `logs\castlevania.log`; with `XBE_CAPTURE_DIR` set, F3 saves a frame with its
  draw list.

## Known Issues
- Only the first areas have been tested so far.

## Legal

This is a non-commercial fan project for preservation and interoperability. It is not affiliated with, endorsed by
or sponsored by Konami or Microsoft.

- This repository and its releases contain only original source code, configuration and documentation, the port's
  own runtime, and the LLVM compiler. They contain **no** game assets, game code (original or recompiled), disc
  images, BIOS, keys or Xbox SDK libraries, and none may be added. `assets`, `generated` and all build output are
  excluded by `.gitignore`; `release.bat` refuses a package with game files or objects in it.
- `config\game.toml` lists addresses and names of functions in the game's executable. It contains no code from it.
- The recompiled code is generated on your own PC from your own copy of the game. `CastlevaniaGame.dll`, and an
  executable from `build.bat`, are for your personal use only. Do not distribute them, generated code or game files.
- Castlevania and Curse of Darkness are trademarks of Konami Digital Entertainment. Xbox is a trademark of
  Microsoft Corporation. All other trademarks belong to their respective owners and are used only to identify the
  game this port works with.

The source in this repository is released under the [GNU General Public License v2.0](LICENSE). The SDK patch in
`sdk-patches` modifies
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) and is under its MIT License, Copyright (c) 2026 TGP482.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482, the recompiler and runtime this is built on.
- [LLVM](https://llvm.org) for Clang and LLD, which releases bundle to build the game code on the player's PC.
- [xxHash](https://github.com/Cyan4973/xxHash) for hashing textures, and [PCSX2](https://pcsx2.net) for the texture
  pack naming its replacement pictures follow.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behaviour.
