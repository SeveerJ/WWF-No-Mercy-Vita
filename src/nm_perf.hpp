#pragma once
// Frame timings from the runtime's [wcw perf] phases (ultramodern/perf.hpp), summed over one-second
// windows: game frames per second, the game's own CPU time per frame, the audio microcode, and the
// renderer's time per display list.
#include <cstdint>
#include <string>

namespace nm::perf {
    struct Window {
        double game_fps = 0;          // osViSwapBuffer calls per second
        double game_busy_ms = 0;      // game threads' run time per frame (frame minus idle)
        double game_frame_ms = 0;     // average frame length
        double audio_ms = 0;          // per audio task
        double audio_tasks = 0;       // audio tasks per second
        double audio_pct = 0;         // share of a core
        double gfx_ms = 0;            // renderer time per display list
        double gfx_lists = 0;         // display lists per second
        double gfx_pct = 0;           // share of a core
    };
    void install();                   // registers the phase sink
    void add_gfx(int64_t ns);         // a display list drawn by the backend
    // Closes the current window if a second has passed; returns true and fills `out` then.
    bool update(Window& out);
    std::string summary(const Window& w);
}
