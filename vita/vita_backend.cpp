// PS Vita backend for WWF No Mercy: Recompiled (adapted from The New Tetris: Recompiled).
//
// Runs the recompiled game with sound, the Vita's controls, and graphics drawn by the portable
// GLES2 renderer (src/gles) on vitaGL. Until the game's first frame (or if something goes wrong
// first, such as a missing ROM) a CPU-drawn status screen is shown instead.
//
// Files: ux0:data/NoMercyRecompiled/
//   *.z64            your ROM (USA Rev 1), picked up on first run and stored in nm_data/
//   nm_data/         stored ROM copy and saves (nm_data/saves)
//   settings.ini     a few options
//   log.txt          what the game and runtime print
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>

#include "nm_game.hpp"
#include "font5x7.h"
#include "librecomp/rsp.hpp"
#include <vitaGL.h>
#include "gles/n64_renderer.hpp"

// Thread stacks: the runtime runs every N64 thread on its own host thread.
extern "C" unsigned int _pthread_stack_default_user = 1024 * 1024;
// Heap: stored ROM (32 MB), RDRAM (17 MB), the ROM read for validation, and everything else.
extern "C" unsigned int _newlib_heap_size_user = 200 * 1024 * 1024;

extern "C" void tnt_vita_register_thread(const char* name);

namespace {
    uint64_t now_ms() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ---- Shared state ----
    std::mutex status_mutex;
    std::string error_text;
    std::atomic<uint64_t> dl_count{ 0 }, vi_count{ 0 };
    std::atomic<uint64_t> audio_frames_in{ 0 }, audio_underruns{ 0 }, audio_dropped{ 0 };
    std::atomic<uint32_t> game_rate{ 0 };
    std::atomic<uint16_t> pad_buttons{ 0 };
    std::atomic<bool> game_started{ false };
    std::atomic<bool> gl_active{ false };
    std::mutex display_mutex;
    std::atomic<uint64_t> gfx_swaps{ 0 }, gfx_dl_us{ 0 }, gfx_present_us{ 0 }, gfx_draws{ 0 }, gfx_uploads{ 0 }, gfx_tris{ 0 };

    constexpr uint16_t N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000,
        N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100,
        N64_L = 0x0020, N64_R = 0x0010, N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001;

    // ---- Per-thread CPU usage ----
    struct ThreadStat { SceUID uid; std::string name; uint64_t last_clocks = 0; double percent = 0; };
    std::mutex threads_mutex;
    std::vector<ThreadStat> thread_stats;

    void register_thread_uid(SceUID uid, const char* name) {
        std::lock_guard lock{ threads_mutex };
        for (auto& t : thread_stats) if (t.uid == uid) { t.name = name; return; }
        thread_stats.push_back({ uid, name });
    }
    void update_thread_stats(double dt) {
        std::lock_guard lock{ threads_mutex };
        for (auto& t : thread_stats) {
            SceKernelThreadInfo info{};
            info.size = sizeof(info);
            if (sceKernelGetThreadInfo(t.uid, &info) != 0) { t.percent = -1; continue; }
            uint64_t clocks = info.runClocks;
            if (t.last_clocks) t.percent = (clocks - t.last_clocks) / (dt * 1e6) * 100.0;
            t.last_clocks = clocks;
        }
    }

    std::atomic<uint64_t> n64_busy_us[256];
    uint64_t n64_busy_last[256];
    double n64_busy_pct[256];
    std::atomic<uint64_t> rsp_busy_us{ 0 }, rsp_tasks{ 0 };
    uint64_t rsp_busy_last = 0, rsp_tasks_last = 0;
    double rsp_busy_pct = 0, rsp_task_ms = 0;

    // ---- Status screen ----
    constexpr int SCREEN_W = 960, SCREEN_H = 544;
    uint32_t* framebuffers[2] = {};
    int fb_index = 0;

    bool init_display() {
        const size_t size = (SCREEN_W * SCREEN_H * 4 + 0xFFFFF) & ~0xFFFFFu;
        for (int i = 0; i < 2; i++) {
            SceUID block = sceKernelAllocMemBlock("nm_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, nullptr);
            if (block < 0) block = sceKernelAllocMemBlock("nm_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, (size + 0x3FFFF) & ~0x3FFFFu, nullptr);
            if (block < 0) { printf("[vita] framebuffer allocation failed: 0x%08X\n", block); return false; }
            void* base = nullptr;
            sceKernelGetMemBlockBase(block, &base);
            framebuffers[i] = static_cast<uint32_t*>(base);
            std::memset(base, 0, size);
        }
        return true;
    }

    constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r; }

    void fill(uint32_t* fb, int x0, int y0, int w, int h, uint32_t c) {
        int xa = std::max(0, x0), xb = std::min(SCREEN_W, x0 + w);
        if (xa >= xb) return;
        for (int y = std::max(0, y0); y < std::min(SCREEN_H, y0 + h); y++)
            std::fill(fb + y * SCREEN_W + xa, fb + y * SCREEN_W + xb, c);
    }
    int text(uint32_t* fb, int x, int y, int s, uint32_t c, const std::string& str) {
        for (char ch : str) {
            if (ch != ' ') {
                const uint8_t* g = tnt_font::glyph(ch);
                if (!g) g = tnt_font::glyph('?');
                for (int row = 0; row < 7; row++)
                    for (int col = 0; col < 5; col++)
                        if (g[row] & (0x10 >> col)) fill(fb, x + col * s, y + row * s, s, s, c);
            }
            x += 6 * s;
        }
        return x;
    }
    std::string fmt(const char* f, ...) {
        char buf[256];
        va_list ap; va_start(ap, f); vsnprintf(buf, sizeof(buf), f, ap); va_end(ap);
        return buf;
    }

    // ---- Audio: game rate -> 48 kHz through a ring buffer and an output thread ----
    constexpr int OUT_RATE = 48000, GRAIN = 512;
    std::mutex audio_mutex;
    std::vector<int16_t> ring(OUT_RATE * 2);
    size_t ring_read = 0, ring_count = 0;
    double resample_pos = 0.0;
    int16_t last_l = 0, last_r = 0;
    int headroom_vis = 2;
    int volume_percent = 100;
    std::atomic<bool> audio_running{ false };
    std::thread audio_thread;

    void ring_push(int16_t l, int16_t r) {
        size_t cap = ring.size() / 2;
        if (ring_count >= cap) { audio_dropped++; return; }
        size_t w = (ring_read + ring_count) % cap;
        ring[w * 2] = l; ring[w * 2 + 1] = r;
        ring_count++;
    }
    void audio_thread_func() {
        tnt_vita_register_thread("Audio output");
        int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0) { printf("[vita] sceAudioOutOpenPort failed: 0x%08X\n", port); return; }
        float v = volume_percent / 100.0f;
        int vol[2] = { int(SCE_AUDIO_VOLUME_0DB * v * v), int(SCE_AUDIO_VOLUME_0DB * v * v) };
        sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
        std::vector<int16_t> out(GRAIN * 2);
        while (audio_running) {
            {
                std::lock_guard lock{ audio_mutex };
                size_t n = std::min<size_t>(ring_count, GRAIN), cap = ring.size() / 2;
                for (size_t i = 0; i < n; i++) {
                    size_t r = (ring_read + i) % cap;
                    out[i * 2] = ring[r * 2]; out[i * 2 + 1] = ring[r * 2 + 1];
                }
                if (n < GRAIN) {
                    if (game_started && audio_frames_in > 0) audio_underruns++;
                    std::fill(out.begin() + n * 2, out.end(), 0);
                }
                ring_read = (ring_read + n) % cap;
                ring_count -= n;
            }
            sceAudioOutOutput(port, out.data());
        }
        sceAudioOutReleasePort(port);
    }
    void queue_samples(int16_t* data, size_t count) {
        if (count * sizeof(int16_t) > 0x3FFF8) { audio_dropped++; return; }
        uint32_t in_rate = game_rate.load();
        if (in_rate == 0) return;
        size_t frames = count / 2;
        audio_frames_in += frames;
        double step = double(in_rate) / OUT_RATE;
        std::lock_guard lock{ audio_mutex };
        for (size_t i = 0; i < frames; i++) {
            int16_t l = data[2 * i + 1], r = data[2 * i];
            while (resample_pos < 1.0) {
                float t = float(resample_pos);
                ring_push(int16_t(last_l + (l - last_l) * t), int16_t(last_r + (r - last_r) * t));
                resample_pos += step;
            }
            resample_pos -= 1.0;
            last_l = l; last_r = r;
        }
    }
    size_t get_frames_remaining() {
        uint32_t in_rate = game_rate.load();
        if (in_rate == 0) return 0;
        uint64_t queued_out;
        { std::lock_guard lock{ audio_mutex }; queued_out = ring_count + GRAIN; }
        uint64_t queued = queued_out * in_rate / OUT_RATE;
        uint64_t per_vi = in_rate / 60, headroom = per_vi * uint64_t(headroom_vis);
        uint64_t reported = queued > headroom ? queued - headroom : 0;
        return (size_t)std::min<uint64_t>(reported, per_vi);
    }
    void set_frequency(uint32_t freq) {
        printf("[vita] game audio rate %u Hz\n", freq);
        game_rate = freq;
    }

    // ---- Input ----
    // Cross = A (grapple), Square = B (strike), Circle = C-Down (run), Triangle = C-Up,
    // L = L, R = R (block), START = START, SELECT = Z, D-pad and left stick = D-pad (movement),
    // right stick = N64 analog stick (taunts), rear touch pad left/right half = C-Left/C-Right.
    std::mutex input_mutex;
    uint16_t cur_buttons = 0;
    float cur_x = 0, cur_y = 0;
    uint64_t quit_held_since = 0;

    float stick(uint8_t v) {
        float f = (int(v) - 128) / 127.0f;
        return std::fabs(f) < 0.2f ? 0.0f : std::clamp(f, -1.0f, 1.0f);
    }
    void sample_inputs() {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint16_t b = 0;
        auto map = [&](uint32_t vita, uint16_t n64) { if (pad.buttons & vita) b |= n64; };
        map(SCE_CTRL_UP, N64_DU); map(SCE_CTRL_DOWN, N64_DD); map(SCE_CTRL_LEFT, N64_DL); map(SCE_CTRL_RIGHT, N64_DR);
        map(SCE_CTRL_CROSS, N64_A); map(SCE_CTRL_SQUARE, N64_B); map(SCE_CTRL_CIRCLE, N64_CD); map(SCE_CTRL_TRIANGLE, N64_CU);
        map(SCE_CTRL_LTRIGGER, N64_L); map(SCE_CTRL_RTRIGGER, N64_R);
        map(SCE_CTRL_START, N64_START); map(SCE_CTRL_SELECT, N64_Z);
        float lx = stick(pad.lx), ly = stick(pad.ly);
        if (ly < -0.6f) b |= N64_DU; if (ly > 0.6f) b |= N64_DD;
        if (lx < -0.6f) b |= N64_DL; if (lx > 0.6f) b |= N64_DR;
        SceTouchData touch{};
        if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) > 0) {
            for (int i = 0; i < touch.reportNum; i++) b |= touch.report[i].x < 960 ? N64_CL : N64_CR; // back pad is 1920 wide
        }
        {
            std::lock_guard lock{ input_mutex };
            cur_buttons = b;
            cur_x = stick(pad.rx);
            cur_y = -stick(pad.ry);
        }
        pad_buttons = b;

        // Hold START + SELECT for 2 seconds to quit.
        uint64_t now = now_ms();
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) {
            if (!quit_held_since) quit_held_since = now;
            else if (now - quit_held_since > 2000) {
                printf("[vita] quit requested\n");
                fflush(stdout);
                sceKernelExitProcess(0);
            }
        } else quit_held_since = 0;
    }
    void poll_input() {}
    bool get_input(int n, uint16_t* buttons, float* x, float* y) {
        if (n != 0) { *buttons = 0; *x = 0; *y = 0; return false; }
        std::lock_guard lock{ input_mutex };
        *buttons = cur_buttons; *x = cur_x; *y = cur_y;
        return true;
    }
    void set_rumble(int, bool) {}
    ultramodern::input::connected_device_info_t get_connected_device_info(int n) {
        // As on PC: port 1 has the emulated Rumble Pak (the Vita just doesn't rumble).
        if (n == 0) return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }

    // ---- Renderer ----
    uint64_t usec_since(std::chrono::steady_clock::time_point t0) {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    class VitaGLRenderer final : public ultramodern::renderer::RendererContext {
    public:
        explicit VitaGLRenderer(uint8_t* rdram) : rdram(rdram) { setup_result = ultramodern::renderer::SetupResult::Success; }
        bool valid() override { return true; }
        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
        void enable_instant_present() override {}
        void send_dl(const OSTask* task) override {
            uint64_t frame = ++dl_count;
            game_started = true;
            if (!start()) return;
            auto t0 = std::chrono::steady_clock::now();
            uint32_t dl = (uint32_t)(uintptr_t)task->t.data_ptr & 0x3FFFFFF;
            if (frame <= 3) printf("[gfx] display list %llu at 0x%06X\n", (unsigned long long)frame, dl);
            renderer->process_dl(dl);
            const n64gfx::Stats& st = renderer->last_stats();
            gfx_draws += st.draw_calls; gfx_uploads += st.texture_uploads; gfx_tris += st.triangles;
            gfx_dl_us += usec_since(t0);
            if (renderer->end_frame()) {
                auto t1 = std::chrono::steady_clock::now();
                vglSwapBuffers(GL_FALSE);
                gfx_present_us += usec_since(t1);
                gfx_swaps++;
            }
        }
        void send_dummy_workload(uint32_t) override {}
        void update_screen() override {
            ++vi_count;
            if (!renderer) return;
            uint32_t origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
            uint64_t frames = dl_count.load();
            if (frames == presented_frames && origin == presented_origin) return;
            presented_frames = frames; presented_origin = origin;
            auto t0 = std::chrono::steady_clock::now();
            if (!renderer->present(origin)) return;
            vglSwapBuffers(GL_FALSE);
            gfx_swaps++;
            if (++presents <= 3) printf("[gfx] present %d\n", presents);
            gfx_present_us += usec_since(t0);
        }
        void shutdown() override {}
        uint32_t get_display_framerate() const override { return 60; }
        float get_resolution_scale() const override { return 1.0f; }
    private:
        bool start() {
            if (renderer) return true;
            if (failed) return false;
            SceIoStat st{};
            if (sceIoGetstat("ur0:data/libshacccg.suprx", &st) < 0 && sceIoGetstat("ur0:data/external/libshacccg.suprx", &st) < 0) {
                failed = true;
                nm::set_error("LIBSHACCCG.SUPRX IS MISSING, SO THE GAME CAN'T BE DRAWN. INSTALL IT WITH SHARKF00D "
                              "(IT SAVES THE FILE TO UR0:DATA/), THEN START THE GAME AGAIN.");
                return false;
            }
            { std::lock_guard lock{ display_mutex }; gl_active = true; }
            vglWaitVblankStart(GL_FALSE);
            printf("[vita] starting vitaGL\n");
            // Not a success flag: GL_FALSE means the requested resolution was used as is.
            vglInitExtended(0, SCREEN_W, SCREEN_H, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
            printf("[vita] vitaGL started: %s / %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
            n64gfx::Config cfg;
            cfg.scale = nm::settings().resolution == "2x" ? 2 : 1;
            cfg.output_width = SCREEN_W;
            cfg.output_height = SCREEN_H;
            cfg.screen_height = 240;
            // No Mercy spreads a frame over several display lists, so draw into off-screen
            // images and show the one the VI points at.
            cfg.direct_to_screen = false;
            renderer = std::make_unique<n64gfx::Renderer>(rdram, cfg);
            return true;
        }
        uint8_t* rdram;
        std::unique_ptr<n64gfx::Renderer> renderer;
        bool failed = false;
        int presents = 0;
        uint64_t presented_frames = ~0ull;
        uint32_t presented_origin = 0;
    };
    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
        return std::make_unique<VitaGLRenderer>(rdram);
    }

    void* create_gfx() { return nullptr; }
    ultramodern::renderer::WindowHandle create_window(void*) { return ultramodern::renderer::WindowHandle{ framebuffers[0] }; }

    struct Rates { double fps = 0, vi = 0, audio_rate = 0; };
    Rates rates;

    void draw_status() {
        static auto last = std::chrono::steady_clock::now();
        static uint64_t last_dl = 0, last_vi = 0, last_audio = 0;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        if (dt >= 1.0) {
            uint64_t dl = dl_count, vi = vi_count, au = audio_frames_in;
            rates.fps = (dl - last_dl) / dt; rates.vi = (vi - last_vi) / dt; rates.audio_rate = (au - last_audio) / dt;
            static int report = 0;
            bool log_now = (report++ % 10) == 0;  // every ~10 s: log writes cost time
            {
                uint64_t frames = dl - last_dl, vis = gfx_swaps.exchange(0);
                uint64_t dl_us = gfx_dl_us.exchange(0), pr_us = gfx_present_us.exchange(0);
                uint64_t draws = gfx_draws.exchange(0), ups = gfx_uploads.exchange(0), tris = gfx_tris.exchange(0);
                if (log_now && gl_active && frames)
                    printf("[gfx] %.2f ms per display list, %.2f ms per flip, %llu flips/s, %llu draws, %llu tris, %llu uploads per list\n",
                        dl_us / 1000.0 / frames, vis ? pr_us / 1000.0 / vis : 0.0, (unsigned long long)(vis / dt), (unsigned long long)(draws / frames),
                        (unsigned long long)(tris / frames), (unsigned long long)(ups / frames));
            }
            last = now; last_dl = dl; last_vi = vi; last_audio = au;
            update_thread_stats(dt);
            for (int i = 0; i < 256; i++) {
                uint64_t b = n64_busy_us[i].load();
                n64_busy_pct[i] = (b - n64_busy_last[i]) / (dt * 1e6) * 100.0;
                n64_busy_last[i] = b;
            }
            {
                uint64_t b = rsp_busy_us.load(), n = rsp_tasks.load();
                rsp_busy_pct = (b - rsp_busy_last) / (dt * 1e6) * 100.0;
                rsp_task_ms = n > rsp_tasks_last ? (b - rsp_busy_last) / 1000.0 / double(n - rsp_tasks_last) : 0;
                rsp_busy_last = b; rsp_tasks_last = n;
            }
            if (log_now) {
                for (int i = 0; i < 256; i++) if (n64_busy_pct[i] >= 1.0) printf("[n64] thread %3d busy %5.1f%%\n", i, n64_busy_pct[i]);
                printf("[rsp] audio microcode %5.1f%% of a core, %.2f ms per task\n", rsp_busy_pct, rsp_task_ms);
                std::lock_guard lock{ threads_mutex };
                for (auto& t : thread_stats) if (t.percent >= 1.0) printf("[cpu] %-24s %5.1f%%\n", t.name.c_str(), t.percent);
                printf("[status] lists/s=%.1f vi=%.1f audio=%.0f Hz/%u underruns=%llu dropped=%llu lists=%llu\n",
                    rates.fps, rates.vi, rates.audio_rate, game_rate.load(), (unsigned long long)audio_underruns.load(),
                    (unsigned long long)audio_dropped.load(), (unsigned long long)dl_count.load());
            }
        }

        std::lock_guard display_lock{ display_mutex };
        if (gl_active) return;
        uint32_t* fb = framebuffers[fb_index];
        if (!fb) return;
        fill(fb, 0, 0, SCREEN_W, SCREEN_H, rgb(20, 14, 18));
        const uint32_t white = rgb(236, 236, 244), gold = rgb(255, 200, 70), grey = rgb(150, 140, 150),
                       green = rgb(90, 230, 120), red = rgb(255, 110, 110);
        int y = 24;
        text(fb, 32, y, 4, gold, "WWF NO MERCY: RECOMPILED"); y += 44;
        text(fb, 32, y, 2, grey, game_started ? "STARTING THE RENDERER..." : "STARTING..."); y += 36;
        auto row = [&](const std::string& label, const std::string& value, uint32_t c) {
            text(fb, 32, y, 3, grey, label);
            text(fb, 260, y, 3, c, value);
            y += 34;
        };
        row("VIDEO RATE", fmt("%.1f / 60", rates.vi), rates.vi >= 57 ? green : gold);
        row("FRAMES", fmt("%llu", (unsigned long long)dl_count.load()), white);
        uint32_t gr = game_rate;
        row("AUDIO", gr ? fmt("%.0f / %u HZ", rates.audio_rate, gr) : "-", gr && rates.audio_rate >= gr * 0.95 ? green : gold);
        row("CPU CLOCK", fmt("%d MHZ", scePowerGetArmClockFrequency()), white);
        y += 6;
        {
            std::lock_guard lock{ status_mutex };
            std::string e = error_text;
            while (!e.empty() && y < SCREEN_H - 60) {
                size_t n = std::min<size_t>(e.size(), 72);
                text(fb, 32, y, 2, red, e.substr(0, n)); e.erase(0, n); y += 22;
            }
        }
        text(fb, 32, SCREEN_H - 50, 2, grey, "HOLD START + SELECT 2 SECONDS TO QUIT.");
        text(fb, 32, SCREEN_H - 26, 2, grey, "LOG: UX0:DATA/NOMERCYRECOMPILED/LOG.TXT");

        SceDisplayFrameBuf frame{};
        frame.size = sizeof(frame);
        frame.base = fb;
        frame.pitch = SCREEN_W;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        frame.width = SCREEN_W;
        frame.height = SCREEN_H;
        sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
        fb_index ^= 1;
    }

    void update_gfx(void*) {
        static auto next = std::chrono::steady_clock::now();
        sample_inputs();
        auto now = std::chrono::steady_clock::now();
        if (now >= next) { next = now + std::chrono::milliseconds(200); draw_status(); }
    }

    void message_box(const char* msg) {
        printf("[vita] MESSAGE: %s\n", msg);
        std::lock_guard lock{ status_mutex };
        error_text = msg;
    }

    RspUcodeFunc* real_audio_ucode = nullptr;
    RspExitReason timed_audio_ucode(uint8_t* rdram, uint32_t ucode_addr) {
        auto t0 = std::chrono::steady_clock::now();
        RspExitReason r = real_audio_ucode(rdram, ucode_addr);
        rsp_busy_us += usec_since(t0);
        rsp_tasks++;
        return r;
    }
    RspUcodeFunc* wrap_rsp(RspUcodeFunc* real) { real_audio_ucode = real; return timed_audio_ucode; }
}

extern "C" void tnt_vita_thread_busy(int id, uint64_t usecs) { n64_busy_us[id & 0xFF] += usecs; }
extern "C" void tnt_vita_register_thread(const char* name) { register_thread_uid(sceKernelGetThreadId(), name); }

std::filesystem::path nm::data_folder() { return std::filesystem::path{ NM_VITA_APP_DIR } / "nm_data"; }

void nm::set_error(const std::string& msg) {
    printf("[vita] %s\n", msg.c_str());
    std::lock_guard lock{ status_mutex };
    error_text = msg;
}

#if !defined(NM_VITA_LOG_TO_TTY)
static void log_write(const char* text, size_t len);
static int log_stderr_write(void*, const char* buf, int n) { log_write(buf, (size_t)n); return n; }
#endif

void nm_platform_early_init(int, char**) {
    sceIoMkdir(NM_VITA_APP_DIR, 0777);
#if !defined(NM_VITA_LOG_TO_TTY)
    sceIoRemove(NM_VITA_APP_DIR "/log.txt");
    stderr = funopen(nullptr, nullptr, log_stderr_write, nullptr, nullptr);
    setvbuf(stderr, nullptr, _IONBF, 0);
#else
    stdout = stderr;
#endif
    printf("WWF No Mercy: Recompiled - PS Vita build\n");
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    if (!init_display()) printf("[vita] couldn't allocate the framebuffer\n");
    tnt_vita_register_thread("Main / status screen");
}

std::string nm_platform_find_rom(int, char**) {
    std::error_code ec;
    if (std::filesystem::exists(nm::data_folder() / "wwf.nomercy.us.z64", ec)) return ""; // already stored
    for (const auto& e : std::filesystem::directory_iterator(NM_VITA_APP_DIR, ec)) {
        std::string lower = e.path().filename().string();
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() > 4 && (lower.ends_with(".z64") || lower.ends_with(".n64") || lower.ends_with(".v64"))) return e.path().string();
    }
    nm::set_error("NO ROM FOUND. COPY YOUR WWF NO MERCY (USA, REV 1) ROM INTO UX0:DATA/NOMERCYRECOMPILED/ AND START AGAIN.");
    return "";
}

nm::Backend nm::make_backend(int, char**) {
    const nm::Settings& st = nm::settings();
    headroom_vis = st.audio_latency;
    volume_percent = st.volume;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    audio_running = true;
    audio_thread = std::thread{ audio_thread_func };

    nm::Backend b{};
    b.renderer = { .create_render_context = create_render_context };
    b.gfx = { .create_gfx = create_gfx, .create_window = create_window, .update_gfx = update_gfx };
    b.audio = { .queue_samples = queue_samples, .get_frames_remaining = get_frames_remaining, .set_frequency = set_frequency };
    b.input = { .poll_input = poll_input, .get_input = get_input, .set_rumble = set_rumble, .get_connected_device_info = get_connected_device_info };
    b.events = { .vi_callback = nullptr, .gfx_init_callback = nullptr };
    b.error_handling = { .message_box = message_box };
    b.wrap_rsp = wrap_rsp;
    return b;
}

#if !defined(NM_VITA_LOG_TO_TTY)
// printf -> log.txt in batches (at most 4 writes a second), appended and closed each time:
// the Vita only updates a file's size when it is closed.
static std::mutex log_mutex;
static std::string log_pending;
static void log_write(const char* text, size_t len) {
    std::lock_guard lock{ log_mutex };
    log_pending.append(text, len);
    static uint64_t last_flush = 0;
    uint64_t t = sceKernelGetProcessTimeWide();
    if (t - last_flush < 250000 && log_pending.size() < 16384) return;
    last_flush = t;
    size_t nl = log_pending.rfind('\n');
    if (nl == std::string::npos && log_pending.size() < 4096) return;
    size_t n = nl == std::string::npos ? log_pending.size() : nl + 1;
    SceUID fd = sceIoOpen(NM_VITA_APP_DIR "/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) { sceIoWrite(fd, log_pending.data(), n); sceIoClose(fd); }
    log_pending.erase(0, n);
}
extern "C" int vprintf(const char* fmt_str, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt_str, ap);
    if (n > 0) log_write(buf, std::min<size_t>(n, sizeof(buf) - 1));
    return n;
}
extern "C" int printf(const char* fmt_str, ...) {
    va_list ap; va_start(ap, fmt_str);
    int n = vprintf(fmt_str, ap);
    va_end(ap);
    return n;
}
extern "C" int puts(const char* s) { log_write(s, strlen(s)); log_write("\n", 1); return 1; }
extern "C" int putchar(int c) { char ch = (char)c; log_write(&ch, 1); return c; }
#else
extern "C" int printf(const char* fmt_str, ...) {
    va_list ap; va_start(ap, fmt_str);
    int n = vfprintf(stderr, fmt_str, ap);
    va_end(ap);
    return n;
}
extern "C" int puts(const char* s) { return fprintf(stderr, "%s\n", s); }
#endif
