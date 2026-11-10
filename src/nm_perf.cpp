#include "nm_perf.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "ultramodern/perf.hpp"

namespace {
    std::atomic<int64_t> frames{ 0 }, frame_ns{ 0 }, busy_ns{ 0 }, audio_ns{ 0 }, audio_n{ 0 }, gfx_ns{ 0 }, gfx_n{ 0 };
    int64_t window_start = 0;

    // Running totals (never reset) for the per-frame hitch report.
    std::atomic<int64_t> t_busy{ 0 }, t_audio{ 0 }, t_audio_n{ 0 }, t_gfx{ 0 }, t_lists{ 0 }, t_uploads{ 0 };

    // A frame much longer than usual: log what happened during it, so hiccups can be traced.
    void hitch_check(int64_t d, int64_t end) {
        static double avg_ms = 33.3;
        static int64_t s_busy, s_audio, s_audio_n, s_gfx, s_lists, s_uploads, first = 0;
        static int reported = 0;
        if (!first) first = end;
        int64_t b = t_busy.load(), a = t_audio.load(), an = t_audio_n.load(), g = t_gfx.load(), l = t_lists.load(), u = t_uploads.load();
        double ms = d / 1e6;
        if (ms > 50.0 && ms > avg_ms * 1.6 && reported < 400) {
            reported++;
            printf("[hitch] %.0f ms frame at %.1f s (usual %.0f): game logic %.1f ms, audio %.1f ms (%lld tasks), gfx %.1f ms (%lld lists, %lld texture uploads)\n",
                ms, (end - first) / 1e9, avg_ms, (b - s_busy) / 1e6, (a - s_audio) / 1e6, (long long)(an - s_audio_n),
                (g - s_gfx) / 1e6, (long long)(l - s_lists), (long long)(u - s_uploads));
        }
        avg_ms = avg_ms * 0.95 + std::min(ms, 100.0) * 0.05;
        s_busy = b; s_audio = a; s_audio_n = an; s_gfx = g; s_lists = l; s_uploads = u;
    }

    void sink(const char* phase, int64_t start, int64_t end) {
        int64_t d = end - start;
        if (!strcmp(phase, "game_frame")) { frames++; frame_ns += d; hitch_check(d, end); }
        else if (!strcmp(phase, "game_busy")) { busy_ns += d; t_busy += d; }
        else if (!strcmp(phase, "rsp_audio")) { audio_ns += d; audio_n++; t_audio += d; t_audio_n++; }
    }
}

void nm::perf::install() {
    ultramodern::perf::set_phase_sink(sink);
    window_start = ultramodern::perf::now_ns();
}

void nm::perf::add_gfx(int64_t ns, int uploads) { gfx_ns += ns; gfx_n++; t_gfx += ns; t_lists++; t_uploads += uploads; }

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
