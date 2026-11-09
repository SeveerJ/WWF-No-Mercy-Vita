// Functions the recompiled game calls that the PC build gets from RT64, RecompFrontend and
// src/game (recomp_api.cpp, raytracing.cpp). On the Vita they do nothing: no 60 fps
// interpolation, no ray tracing, no widescreen, no RetroAchievements.
#include <cstdio>
#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/helpers.hpp"

extern "C" {
// [wcw fix] 60 Hz matrix hooks (RT64's frame interpolation).
void wcw_interp_process_multiply(uint32_t, uint32_t, uint32_t, const float*, const float*) {}
void wcw_interp_invalidate_local_matrix(uint32_t) {}
void wcw_interp_forward_local_matrix(uint32_t, uint32_t) {}

// [wcw fix] widescreen cull: 1 = the original 4:3 limit.
double wcw_widescreen_cull_scale(void) { return 1.0; }

// Ray tracing camera hand-off: no camera is latched, and the display list is left untouched
// (the PC build inserts RT64 extended commands there).
void wcw_rt_latch_camera_multiply(uint8_t*, uint32_t, uint32_t, uint32_t) {}
uint32_t wcw_rt_emit_camera(uint8_t*, uint32_t gfx) { return gfx; }

// nomercy.toml ignores osDriveRomInit; the runtime exports it as the _recomp shim.
void osDriveRomInit_recomp(uint8_t* rdram, recomp_context* ctx);
void osDriveRomInit(uint8_t* rdram, recomp_context* ctx) { osDriveRomInit_recomp(rdram, ctx); }

void recomp_puts(uint8_t* rdram, recomp_context* ctx) {
    PTR(char) s = _arg<0, PTR(char)>(rdram, ctx);
    u32 n = _arg<1, u32>(rdram, ctx);
    for (u32 i = 0; i < n; i++) putchar(MEM_B(i, (gpr)s));
}
void recomp_exit(uint8_t*, recomp_context*) { ultramodern::quit(); }
}

// Generated on PC from RecompiledFuncs/repaired_functions.txt for ROM-patch mods; no mods here.
void wcw_register_repaired_functions() {}
