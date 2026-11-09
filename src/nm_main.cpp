// WWF No Mercy: Recompiled - game setup shared by the Vita and Linux test backends.
// The game-specific parts are taken from WWFNoMercyRecomp's src/main/main.cpp (ROM hash,
// save type, entry point, the [wcw] runtime switches, the audio microcode); the PC
// launcher, RT64, RecompFrontend, mods and RetroAchievements are left out.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include "nm_game.hpp"
#include "librecomp/rsp.hpp"
#include "librecomp/overlays.hpp"
#include "ovl_patches.hpp"

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
extern RspUcodeFunc nomercy_audio_ucode;   // rsp/nomercy_audio.cpp (RSPRecomp)

static const recomp::Version project_version{ 0, 1, 0, "-vita" };
static const char8_t* game_id = u8"wwf.nomercy.us";
static nm::Backend g_backend;

// Never return null here: the runtime treats that as fatal. Anything that isn't audio
// gets a no-op microcode that reports a clean break (as the PC build does).
static RspExitReason null_ucode(uint8_t*, uint32_t) { return RspExitReason::Broke; }

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    static int n = 0;
    if (n++ < 2) printf("[rsp] task type=%u ucode=0x%08X\n", (unsigned)task->t.type, (unsigned)task->t.ucode);
    if (task->t.type == M_AUDTASK) {
        return g_backend.wrap_rsp ? g_backend.wrap_rsp(nomercy_audio_ucode) : nomercy_audio_ucode;
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
