// WWF No Mercy: Recompiled - game setup shared by the Vita and Linux test backends.
// The game-specific parts are taken from WWFNoMercyRecomp's src/main/main.cpp (ROM hash,
// save type, entry point, the [wcw] runtime switches, the audio microcode); the PC
// launcher, RT64, RecompFrontend, mods and RetroAchievements are left out.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "nm_game.hpp"
#include "librecomp/rsp.hpp"
#include "librecomp/overlays.hpp"
#include "ovl_patches.hpp"

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
extern RspUcodeFunc nomercy_audio_ucode;   // rsp/nomercy_audio.cpp (RSPRecomp)
RspExitReason nm_hle_audio_ucode(uint8_t* rdram, uint32_t ucode_addr); // nm_hle_audio.cpp

// Test aid (NM_AUDIO_COMPARE=1, Linux build): runs every audio task through both the recompiled
// microcode and the HLE version from the same starting memory, and counts the bytes of RDRAM
// that end up different.
uint8_t* nm_audio_shadow = nullptr; uint8_t* nm_audio_shadow_base = nullptr;
static RspExitReason compare_audio_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    extern uint8_t dmem[];
    // Both run on private copies of the same snapshot (the game keeps running meanwhile).
    static std::vector<uint8_t> c1(0x800000), c2(0x800000);
    static uint8_t dmem_before[0x1000];
    static uint64_t tasks = 0, diff_tasks = 0, diff_bytes = 0;
    memcpy(c1.data(), rdram, 0x800000);
    memcpy(c2.data(), c1.data(), 0x800000);
    memcpy(dmem_before, dmem, 0x1000);
    nomercy_audio_ucode(c1.data(), ucode_addr);
    // What the recompiled microcode wrote (c2 is still the untouched snapshot) accumulates in a
    // shadow RDRAM, so any audio buffer can later be read as the recompiled version made it.
    static std::vector<uint8_t> shadow(0x800000);
    static bool shadow_init = false;
    if (!shadow_init) { memcpy(shadow.data(), c2.data(), 0x800000); shadow_init = true; }
    for (uint32_t i = 0; i < 0x800000; i++) if (c1[i] != c2[i]) shadow[i] = c1[i];
    memcpy(dmem, dmem_before, 0x1000);
    nm_hle_audio_ucode(c2.data(), ucode_addr);
    uint64_t d = 0; uint32_t first = 0, last = 0;
    static uint64_t small = 0, big = 0; static double sum_abs = 0;
    for (uint32_t i = 0; i < 0x800000; i += 2) {
        if (c1[i] != c2[i] || c1[i + 1] != c2[i + 1]) {
            if (!d) first = i; last = i; d += 2;
            int16_t x = *reinterpret_cast<int16_t*>(&c1[i]), y = *reinterpret_cast<int16_t*>(&c2[i]);
            int e = std::abs(x - y); sum_abs += e;
            if (e <= 4) small++; else big++;
        }
    }
    if ((tasks % 500) == 499) printf("[audio-compare] halfwords differing: %llu by <=4, %llu by more, mean |diff| %.1f\n",
        (unsigned long long)small, (unsigned long long)big, (small + big) ? sum_abs / (small + big) : 0.0);
    tasks++;
    if (d) { diff_tasks++; diff_bytes += d; }
    if (d && diff_tasks <= 10) printf("[audio-compare] task %llu: %llu bytes differ (0x%06X..0x%06X)\n", (unsigned long long)tasks, (unsigned long long)d, first, last);
    if ((tasks % 500) == 0) printf("[audio-compare] %llu tasks, %llu differ, %llu bytes total\n", (unsigned long long)tasks, (unsigned long long)diff_tasks, (unsigned long long)diff_bytes);
    // Keep the recompiled result where the backend can find the same buffer for a second .wav.
    nm_audio_shadow = shadow.data(); nm_audio_shadow_base = rdram;
    // The real run, on the real memory.
    memcpy(dmem, dmem_before, 0x1000);
    return nm_hle_audio_ucode(rdram, ucode_addr);
}

static bool use_hle_audio() {
    static const bool hle = [] {
        const std::string& m = nm::settings().audio_microcode;
        bool h;
        if (m == "hle") h = true;
        else if (m == "recompiled") h = false;
        else {
#if defined(__vita__)
            h = true;
#else
            h = getenv("NM_HLE") != nullptr;
#endif
        }
        printf("[audio] sound processor: %s\n", h ? "high-level (HLE, naudio)" : "recompiled microcode");
        return h;
    }();
    return hle;
}

static const recomp::Version project_version{ 0, 1, 0, "-vita" };
static const char8_t* game_id = u8"wwf.nomercy.us";
static nm::Backend g_backend;

// Never return null here: the runtime treats that as fatal. Anything that isn't audio
// gets a no-op microcode that reports a clean break (as the PC build does).
static RspExitReason null_ucode(uint8_t*, uint32_t) { return RspExitReason::Broke; }

static uint8_t* g_rdram = nullptr;
void nm_set_rdram(uint8_t* rdram) { g_rdram = rdram; } // called by the backends' render context
static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    static int n = 0;
    if (n++ < 2) {
        printf("[rsp] task type=%u ucode=0x%08X size 0x%X data=0x%08X size 0x%X\n", (unsigned)task->t.type, (unsigned)task->t.ucode,
            (unsigned)task->t.ucode_size, (unsigned)task->t.ucode_data, (unsigned)task->t.ucode_data_size);
        if (g_rdram) {
            uint32_t d = (uint32_t)task->t.ucode_data & 0x7FFFFF;
            auto w = [&](uint32_t o) { return *reinterpret_cast<uint32_t*>(g_rdram + d + o); };
            printf("[rsp] ucode data words: +0 %08X +10 %08X +28 %08X +30 %08X\n", w(0), w(0x10), w(0x28), w(0x30));
        }
    }
    if (task->t.type == M_AUDTASK) {
        static const bool compare = getenv("NM_AUDIO_COMPARE") != nullptr;
        RspUcodeFunc* f = compare ? compare_audio_ucode : use_hle_audio() ? nm_hle_audio_ucode : nomercy_audio_ucode;
        return g_backend.wrap_rsp ? g_backend.wrap_rsp(f) : f;
    }
    printf("[rsp] unknown task type %u: skipped\n", (unsigned)task->t.type);
    return null_ucode;
}

static const char* rom_error_text(recomp::RomValidationError e) {
    switch (e) {
        case recomp::RomValidationError::Good: return "ok";
        case recomp::RomValidationError::FailedToOpen: return "could not open the file";
        case recomp::RomValidationError::NotARom: return "not an N64 ROM";
        case recomp::RomValidationError::IncorrectRom: return "this is not WWF No Mercy";
        case recomp::RomValidationError::NotYet: return "unsupported ROM";
        case recomp::RomValidationError::IncorrectVersion:
            return "wrong version: only WWF No Mercy USA Rev 1 works (SHA1 91CEE3D0...ABD1)";
        default: return "unknown error";
    }
}

// Platform hooks (vita_backend.cpp / headless_backend.cpp).
void nm_platform_early_init(int argc, char** argv);
std::string nm_platform_find_rom(int argc, char** argv); // "" if none to import

int main(int argc, char** argv) {
    nm_platform_early_init(argc, argv);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    std::error_code ec;
    std::filesystem::create_directories(nm::data_folder(), ec);
    std::filesystem::create_directories(nm::data_folder() / "saves", ec);
    recomp::register_config_path(nm::data_folder());
    nm::init_settings();

    // [wcw] runtime switches the PC build sets (see WWFNoMercyRecomp src/main/main.cpp):
    // keep the pak emulation's backing apart from the flash save, and return the
    // hardware-quantised audio rate from osAiSetFrequency.
    extern bool wcw_pak_separate_backing;
    wcw_pak_separate_backing = true;
    extern bool wcw_ai_accurate_freq;
    wcw_ai_accurate_freq = true;

    recomp::GameEntry game{};
    game.rom_hash = 0x0455F72A0F9FF778ULL;          // XXH3-64 of the USA Rev 1 ROM
    game.internal_name = "WWF No Mercy";
    game.display_name = "WWF No Mercy";
    game.game_id = game_id;
    game.mod_game_id = "wwfnomercy";
    game.save_type = recomp::SaveType::Flashram;
    game.is_enabled = true;
    game.entrypoint_address = (gpr)(int32_t)0x80000400; // sign-extended, see the PC main.cpp
    game.entrypoint = recomp_entrypoint;
    recomp::register_game(game);
    wcw::register_wcw_overlays();

    std::string rom = nm_platform_find_rom(argc, argv);
    if (!rom.empty()) {
        std::u8string gid = game_id;
        auto err = recomp::select_rom(rom, gid);
        if (err == recomp::RomValidationError::Good) {
            printf("ROM accepted and stored: %s\n", rom.c_str());
        } else {
            nm::set_error(std::string("ROM REJECTED: ") + rom + " - " + rom_error_text(err));
        }
    }

    g_backend = nm::make_backend(argc, argv);

    // Start as soon as the runtime is up (there is no launcher menu). If no valid ROM is
    // stored, the runtime reports it through the message box and the status screen shows it.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::u8string gid = game_id;
        recomp::start_game(gid, "");
    }).detach();

    recomp::rsp::callbacks_t rsp_callbacks{ .get_rsp_microcode = get_rsp_microcode };
    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = [](const OSThread* t) -> std::string { return "N64 thread " + std::to_string(t->id); } };
    printf("[nm] starting the runtime\n");
    recomp::start(project_version, {}, rsp_callbacks, g_backend.renderer, g_backend.audio,
        g_backend.input, g_backend.gfx, g_backend.events, g_backend.error_handling, threads_callbacks);
    printf("[nm] runtime returned\n");
    return 0;
}
