// High-level emulation (HLE) of No Mercy's audio microcode.
//
// AKI's audio microcode is the "naudio" ABI (its ucode data identifies as 0x127C at +0x10, the
// signature mupen64plus-rsp-hle matches for naudio). Instead of running the recompiled RSP code
// instruction by instruction, each audio command runs directly in C, using mupen64plus-rsp-hle
// (src/hle_audio/, GPL-2.0-or-later). The recompiled microcode costs up to 25 ms per task on the
// Vita with music and voices playing; this is the fast path. settings.ini [audio] audio_microcode.
//
// N64ModernRuntime copies the OSTask into DMEM at 0xFC0 before calling the microcode, and keeps
// DMEM and RDRAM in the same word-swapped layout mupen64plus uses.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "librecomp/rsp.hpp"

extern "C" {
#include "hle_audio/hle_internal.h"
void alist_process_naudio(struct hle_t* hle);

void HleWarnMessage(void*, const char* message, ...) {
    static int count = 0;
    if (count++ > 20) return;
    va_list ap; va_start(ap, message);
    char buf[256]; vsnprintf(buf, sizeof(buf), message, ap);
    va_end(ap);
    printf("[hle audio] %s\n", buf);
}
void HleVerboseMessage(void*, const char*, ...) {}
void HleInfoMessage(void*, const char*, ...) {}
void HleErrorMessage(void*, const char* message, ...) {
    va_list ap; va_start(ap, message);
    char buf[256]; vsnprintf(buf, sizeof(buf), message, ap);
    va_end(ap);
    printf("[hle audio] error: %s\n", buf);
}
// Only the naudio_mp3 variant (other games) uses this.
void mp3_task(struct hle_t*, unsigned int, uint32_t) {}
// The runtime handles task completion itself.
void rsp_break(struct hle_t*, unsigned int) {}
}

namespace {
    hle_t g_hle{};
}

RspExitReason nm_hle_audio_ucode(uint8_t* rdram, uint32_t /*ucode_addr*/) {
    g_hle.dram = rdram;
    g_hle.dmem = dmem;
    alist_process_naudio(&g_hle);
    return RspExitReason::Broke;
}
