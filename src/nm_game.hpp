#pragma once
// Shared declarations between the game setup (nm_main.cpp) and the platform backends
// (headless/headless_backend.cpp for Linux testing, vita/vita_backend.cpp for the Vita).
#include <cstdint>
#include <filesystem>
#include <string>
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"

namespace nm {
    struct Backend {
        ultramodern::renderer::callbacks_t renderer;
        ultramodern::gfx_callbacks_t gfx;
        ultramodern::audio_callbacks_t audio;
        ultramodern::input::callbacks_t input;
        ultramodern::events::callbacks_t events;
        ultramodern::error_handling::callbacks_t error_handling;
        // Wraps the audio microcode (for timing); may be null.
        RspUcodeFunc* (*wrap_rsp)(RspUcodeFunc* real) = nullptr;
    };
    Backend make_backend(int argc, char** argv);

    // Folder holding the stored ROM copy, saves and settings.
    std::filesystem::path data_folder();

    // settings.ini, kept deliberately small.
    struct Settings {
        std::string resolution = "native";  // native (320x240 x1) or 2x
        bool show_fps = false;
        bool perf_overlay = true;           // timings in the corner (SELECT + TRIANGLE)
        int volume = 100;                   // percent
        int audio_latency = 2;              // VIs of extra audio kept queued
        int frameskip = 0;
        std::string audio_microcode = "auto"; // auto (hle on the Vita), hle, recompiled                  // draw 1 of every (frameskip+1) frames; 0 = draw all
    };
    Settings& settings();
    void init_settings();

    // Shown on the Vita status screen / printed by the headless build.
    void set_error(const std::string& msg);
}
