# Castlevania: Curse of Darkness - XBE Recomp

An unofficial native PC port of the original Xbox version of Castlevania: Curse of Darkness (USA), statically
recompiled with [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp). Rendering runs on Direct3D 12,
sound through XAudio2.

> [!NOTE]
> This project does not include any game assets. The game files in `assets` come from your own copy of the game.

## Running

`out\build\release\Castlevania.exe`. Settings are in `settings.toml` beside it, saves in `hdd\E\UDATA\4b4e002d`.

## Controls

Xbox, PlayStation and Switch controllers work through SDL. The keyboard plays on port 1 while the window is in front:

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

## Building

Needs Visual Studio with **Desktop development with C++** and **C++ CMake tools**, and Clang (LLVM, for example
`winget install LLVM.LLVM`). The SDK is expected beside this folder as `..\XboxXBE-Recomp`, at commit `99078b1` with
this repo's SDK changes applied:

```
git clone https://github.com/TGP482/XboxXBERecomp ..\XboxXBE-Recomp
git -C ..\XboxXBE-Recomp checkout 99078b1
git -C ..\XboxXBE-Recomp apply ..\Castle-XBERecomp\sdk-patches\xboxxbe-recomp.patch
```

Put your own extracted game files (`default.xbe` and the `data`, `movie`, `sound` folders) in `assets`, then run
`build.bat`.

## Port notes

Changes this game needed, beyond the generated config:

- `config\game.toml`, by hand: `D3DDevice_SetPalette`, `D3DDevice_DrawIndexedVerticesUP` and five DirectSound buffer
  methods (`SetPitch`, `Pause`, `PlayEx`, `SetDopplerFactor`, `SetRolloffFactor`) that no 5849 signature matched; the
  DirectSound ones are named from the image's `.XTLID` table.
- SDK changes (in `..\XboxXBE-Recomp`): host versions of `DrawIndexedVerticesUP` and the five DirectSound methods; a
  fixed-function stage with no texture that reads `D3DTA_TEXTURE` ends the stage cascade, as XDK D3D does (without it
  every single texture quad drew white); `XBE_VERBOSE` lets a release build log info lines.

## Testing

- `tools\run_test.ps1 -Name x -At 10,20 -Env "CV_PAD_SCRIPT=@tools\scripts\newgame.txt"` runs the game, screenshots
  it at the given seconds into `tools\shots`, and closes it.
- `CV_PAD_SCRIPT` presses buttons on a timeline (see `src\common\keyboard.cpp`).
- `XBE_VERBOSE=1` logs info lines to `logs\castlevania.log`; with `XBE_CAPTURE_DIR` set, F3 saves a frame with its
  draw list.

## Known issues

- A band of streaks along the bottom edge of the 3D view.
- Only the first area has been tested so far.
