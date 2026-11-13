# WWF No Mercy: Recompiled — PS Vita

A native PS Vita port of [WWF No Mercy: Recompiled](https://github.com/jessetbh/WWFNoMercyRecomp), the
static recompilation of the Nintendo 64 game. It runs the game's own code converted to native ARM code,
not an emulator.

**No game data is included.** You need your own ROM.

## Requirements

- A PS Vita / PS TV with HENkaku or Ensō, and VitaShell
- `libshacccg.suprx` extracted to `ur0:data/` (the usual shader compiler setup for vitaGL homebrew)
- Your own **WWF No Mercy (USA) Rev 1** ROM in `.z64` format. Other versions are not supported.

## Install

1. Install `NoMercyRecompiled.vpk` with VitaShell. Updates install over the previous version, and your saves are kept.
2. Copy your ROM into `ux0:data/NoMercyRecompiled/` (any file name ending in `.z64`).
3. Launch it. The ROM is checked and stored on first launch.

Settings (`settings.ini`), saves, save backups and `log.txt` are all in `ux0:data/NoMercyRecompiled/`.

## Controls

| Vita | N64 | In game |
|---|---|---|
| Cross | A | Grapple |
| Square | B | Strike |
| Circle | C-Down | Run |
| Triangle | C-Up | Climb / ropes |
| L / R | L / R | R: block / counter |
| START | START | |
| SELECT | Z | |
| D-pad / left stick | D-pad | Move |
| Right stick | Analog stick | Taunts / finishers |
| Rear touch pad, left / right half | C-Left / C-Right | |

Hold **START + SELECT** for 2 seconds to quit. **SELECT + TRIANGLE** toggles the timings overlay.

## Known issues

- 4-player matches can drop to around 20 fps.
- Occasional brief stutters.
- Models can disappear for a single frame now and then (fixed in the next build).

## Building from source

This repo holds no game code. It builds against a
[WWFNoMercyRecomp](https://github.com/jessetbh/WWFNoMercyRecomp) checkout whose `RecompiledFuncs/` and
`rsp/nomercy_audio.cpp` you generate yourself from your own ROM (that repo's BUILDING.md), with
`runtime/runtime-vita-wcw.patch` applied to its `lib/N64ModernRuntime`. You also need
[VitaSDK](https://vitasdk.org) with vitaGL, vitaShaRK, SceShaccCgExt, libmathneon and zlib.

```
cd WWFNoMercyRecomp/lib/N64ModernRuntime && git apply ../../../wwf-no-mercy-vita/runtime/runtime-vita-wcw.patch
cmake -S wwf-no-mercy-vita -B build-vita -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DNM_GAME_DIR=<path to WWFNoMercyRecomp>
cmake --build build-vita            # -> build-vita/NoMercyRecompiled.vpk
```

A Linux test build (headless, Mesa GLES2 through surfaceless EGL, with frame dumps and scripted input) is
available with `-DNM_HOST=ON -DNM_KHR_DIR=<EGL/GLES2 headers and libs>`.

## Credits

- jessetbh: WWF No Mercy: Recompiled
- Mr-Wiseguy and contributors: N64Recomp and N64ModernRuntime
- Rinnegatamante: vitaGL
- The mupen64plus team: RSP audio HLE (`src/hle_audio`)
- The VitaSDK team
- CodebreakerDX: testing

## Licence

GPL-3.0 (see `LICENSE`), as required by the runtime it is built on. The audio HLE in `src/hle_audio` comes
from mupen64plus-rsp-hle (GPL-2.0-or-later).
