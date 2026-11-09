#include "nm_perf.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include "ultramodern/perf.hpp"

namespace {
    std::atomic<int64_t> frames{ 0 }, frame_ns{ 0 }, busy_ns{ 0 }, audio_ns{ 0 }, audio_n{ 0 }, gfx_ns{ 0 }, gfx_n{ 0 };
    int64_t window_start = 0;

    void sink(const char* phase, int64_t start, int64_t end) {
        int64_t d = end - start;
        if (!strcmp(phase, "game_frame")) { frames++; frame_ns += d; }
        else if (!strcmp(phase, "game_busy")) busy_ns += d;
        else if (!strcmp(phase, "rsp_audio")) { audio_ns += d; audio_n++; }
    }
}

void nm::perf::install() {
    ultramodern::perf::set_phase_sink(sink);
    window_start = ultramodern::perf::now_ns();
}

void nm::perf::add_gfx(int64_t ns) { gfx_ns += ns; gfx_n++; }

bool nm::perf::update(Window& w) {
    int64_t now = ultramodern::perf::now_ns();
    double dt = (now - window_start) / 1e9;
    if (dt < 1.0) return false;
    window_start = now;
    int64_t f = frames.exchange(0), fns = frame_ns.exchange(0), b = busy_ns.exchange(0);
    int64_t an = audio_n.exchange(0), ans = audio_ns.exchange(0), gn = gfx_n.exchange(0), gns = gfx_ns.exchange(0);
    w.game_fps = f / dt;
    w.game_frame_ms = f ? fns / 1e6 / f : 0;
    w.game_busy_ms = f ? b / 1e6 / f : 0;
    w.audio_tasks = an / dt;
    w.audio_ms = an ? ans / 1e6 / an : 0;
    w.audio_pct = ans / 1e7 / dt;
    w.gfx_lists = gn / dt;
    w.gfx_ms = gn ? gns / 1e6 / gn : 0;
    w.gfx_pct = gns / 1e7 / dt;
    return true;
}

std::string nm::perf::summary(const Window& w) {
    char buf[256];
    snprintf(buf, sizeof(buf), "game %.1f fps, logic %.1f ms/frame | audio %.2f ms x %.0f/s (%.0f%%) | gfx %.2f ms x %.0f/s (%.0f%%)",
        w.game_fps, w.game_busy_ms, w.audio_ms, w.audio_tasks, w.audio_pct, w.gfx_ms, w.gfx_lists, w.gfx_pct);
    return buf;
}
