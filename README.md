# WWF No Mercy: Recompiled — PS Vita port

Vita build of [WWFNoMercyRecomp](https://github.com/jessetbh/WWFNoMercyRecomp) (USA Rev 1), reusing the
PS Vita work from The New Tetris: Recompiled (GLES2 N64 renderer on vitaGL, Vita backend, runtime patch).

This repo holds no game code. It builds against a WWFNoMercyRecomp checkout (`NM_GAME_DIR`) whose
`RecompiledFuncs/` and `rsp/nomercy_audio.cpp` you generate yourself (that repo's BUILDING.md §4), with
`runtime/runtime-vita-wcw.patch` applied to its `lib/N64ModernRuntime` (wcw branch).

```
cd WWFNoMercyRecomp/lib/N64ModernRuntime && git apply ../../../nm-vita/runtime/runtime-vita-wcw.patch
# Vita
cmake -S nm-vita -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build-vita            # -> build-vita/NoMercyRecompiled.vpk
# Linux test build (Mesa GLES2 through surfaceless EGL; frame dumps, scripted input)
cmake -S nm-vita -B build-host -G Ninja -DNM_HOST=ON -DNM_KHR_DIR=<EGL/GLES2 headers+libs>
```

Vita files: `ux0:data/NoMercyRecompiled/` — put the ROM (any `.z64`) there; it is checked and stored in
`nm_data/` on first launch. `settings.ini`, `log.txt` and saves (`nm_data/saves/`) live there too.

Controls: Cross = A (grapple), Square = B (strike), Circle = C-Down (run), Triangle = C-Up,
L/R = L/R, START, SELECT = Z, D-pad/left stick = D-pad, right stick = analog stick (taunts),
rear touch left/right half = C-Left/C-Right. Hold START+SELECT 2 s to quit.

Licences: runtime is GPL-3.0; the renderer and backend come from The New Tetris: Recompiled (GPL-3.0).
