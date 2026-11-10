// Linux test backend: runs the recompiled game with the same GLES2 renderer as the Vita,
// on Mesa (llvmpipe) through a surfaceless EGL pbuffer. No window, no sound card.
//
// Usage: nm_headless [rom.z64]   (the ROM is validated and stored in $NM_DATA or ./nm_data)
// Environment:
//   NM_SECONDS=n        quit after n seconds (default 30)
//   NM_DUMP=prefix      write presented frames as prefix_NNNNN.ppm
//   NM_DUMP_EVERY=n     every n presents (default 60)
//   NM_INPUT=file       scripted controller 1: lines "first_poll last_poll buttons_hex" (polls,
//                       not seconds, so a script replays the same whatever the host speed)
//   NM_WAV=file         write the game's audio to a .wav
//   NM_SCALE=n          renderer scale (default 1)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "gles/gles_includes.h"
#include "gles/n64_renderer.hpp"
#include "nm_game.hpp"
#include "nm_perf.hpp"

void nm_set_rdram(uint8_t* rdram); // nm_main.cpp
extern uint8_t* nm_audio_shadow; extern uint8_t* nm_audio_shadow_base; // NM_AUDIO_COMPARE
namespace {
    constexpr int OUT_W = 960, OUT_H = 544;
    std::atomic<uint64_t> dl_count{ 0 }, vi_count{ 0 }, presents{ 0 }, polls{ 0 }, audio_frames{ 0 };
    std::atomic<uint64_t> gfx_us{ 0 }, rsp_us{ 0 }, rsp_tasks{ 0 };
    std::atomic<uint32_t> audio_rate{ 0 };
    double run_seconds = 30;
    std::string dump_prefix;
    int dump_every = 60;
    std::chrono::steady_clock::time_point t_start;

    uint64_t usec(std::chrono::steady_clock::time_point t0) {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    void crash_handler(int sig) {
        void* frames[64];
        int n = backtrace(frames, 64);
        const char msg[] = "\n[headless] fatal signal, backtrace:\n";
        write(2, msg, sizeof(msg) - 1);
        backtrace_symbols_fd(frames, n, 2);
        _exit(128 + sig);
    }

    // ---- Input script ----
    struct Entry { uint64_t p0, p1; uint16_t buttons; int pad; };
    std::vector<Entry> script;
    void load_script(const char* path) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream is(line);
            Entry e{};
            std::string hex;
            if (is >> e.p0 >> e.p1 >> hex) { e.buttons = (uint16_t)std::stoul(hex, nullptr, 16); if (!(is >> e.pad)) e.pad = 0; script.push_back(e); }
        }
        printf("[headless] input script: %zu entries\n", script.size());
    }
    void poll_input() { polls++; }
    bool get_input(int n, uint16_t* buttons, float* x, float* y) {
        *x = 0; *y = 0; *buttons = 0;
        uint64_t p = polls.load();
        for (auto& e : script) if (e.pad == n && p >= e.p0 && p <= e.p1) *buttons |= e.buttons;
        return true;
    }
    void set_rumble(int, bool) {}
    ultramodern::input::connected_device_info_t get_connected_device_info(int n) {
        // As the Vita: one controller with the emulated Rumble Pak (NM_PADS=2 adds a second for scripts).
        static const int pads = getenv("NM_PADS") ? atoi(getenv("NM_PADS")) : 1;
        if (n >= pads) return { .connected_device = ultramodern::input::Device::None, .connected_pak = ultramodern::input::Pak::None };
        return { .connected_device = ultramodern::input::Device::Controller, .connected_pak = ultramodern::input::Pak::RumblePak };
    }

    // ---- Audio: a virtual device draining in real time ----
    std::mutex audio_mutex;
    double queued = 0;
    std::chrono::steady_clock::time_point last_drain = std::chrono::steady_clock::now();
    FILE* wav = nullptr; FILE* wav2 = nullptr;
    uint64_t wav_frames = 0;
    void drain() {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_drain).count();
        last_drain = now;
        queued = std::max(0.0, queued - dt * audio_rate.load());
    }
    void queue_samples(int16_t* data, size_t count) {
        std::lock_guard lock{ audio_mutex };
        drain();
        queued += count / 2;
        audio_frames += count / 2;
        if (wav) {
            for (size_t i = 0; i + 1 < count; i += 2) { int16_t lr[2] = { data[i + 1], data[i] }; fwrite(lr, 2, 2, wav); }
            wav_frames += count / 2;
        }
        if (wav2 && nm_audio_shadow) {
            const int16_t* sd = reinterpret_cast<const int16_t*>(nm_audio_shadow + ((uint8_t*)data - nm_audio_shadow_base));
            for (size_t i = 0; i + 1 < count; i += 2) { int16_t lr[2] = { sd[i + 1], sd[i] }; fwrite(lr, 2, 2, wav2); }
        }
    }
    size_t get_frames_remaining() { std::lock_guard lock{ audio_mutex }; drain(); return (size_t)queued; }
    void set_frequency(uint32_t f) { printf("[headless] audio rate %u Hz\n", f); audio_rate = f; }
    void wav_finish() {
        if (!wav) return;
        uint32_t rate = audio_rate ? audio_rate.load() : 32000, bytes = (uint32_t)(wav_frames * 4);
        fseek(wav, 0, SEEK_SET);
        auto w32 = [](uint32_t v) { fwrite(&v, 4, 1, wav); };
        auto w16 = [](uint16_t v) { fwrite(&v, 2, 1, wav); };
        fwrite("RIFF", 1, 4, wav); w32(36 + bytes); fwrite("WAVEfmt ", 1, 8, wav);
        w32(16); w16(1); w16(2); w32(rate); w32(rate * 4); w16(4); w16(16); fwrite("data", 1, 4, wav); w32(bytes);
        fclose(wav); wav = nullptr;
        if (wav2) fclose(wav2);
    }

    // ---- EGL ----
    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    bool init_egl() {
        auto gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        egl_dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint ma, mi;
        if (!eglInitialize(egl_dpy, &ma, &mi)) { printf("[headless] eglInitialize failed\n"); return false; }
        EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE };
        EGLConfig cfg; EGLint n = 0;
        eglChooseConfig(egl_dpy, attrs, &cfg, 1, &n);
        if (!n) { printf("[headless] no EGL config\n"); return false; }
        EGLint pb[] = { EGL_WIDTH, OUT_W, EGL_HEIGHT, OUT_H, EGL_NONE };
        EGLSurface surf = eglCreatePbufferSurface(egl_dpy, cfg, pb);
        eglBindAPI(EGL_OPENGL_ES_API);
        EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ca);
        if (!eglMakeCurrent(egl_dpy, surf, surf, ctx)) { printf("[headless] eglMakeCurrent failed\n"); return false; }
        printf("[headless] GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        return true;
    }

    void dump_frame(uint64_t n) {
        static std::vector<uint8_t> px(OUT_W * OUT_H * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(0, 0, OUT_W, OUT_H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        char name[512];
        snprintf(name, sizeof(name), "%s_%05llu.ppm", dump_prefix.c_str(), (unsigned long long)n);
        FILE* f = fopen(name, "wb");
        if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", OUT_W, OUT_H);
        for (int y = OUT_H - 1; y >= 0; y--)
            for (int x = 0; x < OUT_W; x++) fwrite(&px[(y * OUT_W + x) * 4], 1, 3, f);
        fclose(f);
        printf("[headless] wrote %s (dl %llu, polls %llu, t=%.1fs)\n", name, (unsigned long long)dl_count.load(), (unsigned long long)polls.load(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count());
    }

    class GlesContext final : public ultramodern::renderer::RendererContext {
    public:
        explicit GlesContext(uint8_t* rdram) : rdram(rdram) { setup_result = ultramodern::renderer::SetupResult::Success; }
        bool valid() override { return true; }
        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
        void enable_instant_present() override {}
        void send_dl(const OSTask* task) override {
            if (!start()) return;
            uint64_t n = ++dl_count;
            auto t0 = std::chrono::steady_clock::now();
            uint32_t dl = (uint32_t)(uintptr_t)task->t.data_ptr & 0x3FFFFFF;
            if (n <= 3) printf("[gfx] display list %llu at 0x%06X\n", (unsigned long long)n, dl);
            if (n == 1) {
                uint32_t d = (uint32_t)(uintptr_t)task->t.ucode_data & 0x7FFFFF, sz = task->t.ucode_data_size;
                std::string str;
                for (uint32_t i = 0; i < sz && i < 0x1000; i++) {
                    char c = (char)rdram[(d + i) ^ 3];
                    str += (c >= 32 && c < 127) ? c : '.';
                }
                size_t k = str.find("RSP");
                printf("[gfx] ucode text %08X data %08X size %X: %s\n", (uint32_t)(uintptr_t)task->t.ucode, d, sz,
                    k == std::string::npos ? "(no RSP string)" : str.substr(k, 60).c_str());
            }
            renderer->process_dl(dl);
            gfx_us += usec(t0);
            nm::perf::add_gfx((int64_t)usec(t0) * 1000);
            if (renderer->end_frame()) after_present();
        }
        void send_dummy_workload(uint32_t) override {}
        void update_screen() override {
            ++vi_count;
            if (!renderer) return;
            // The image the game last finished (its latest osViSwapBuffer), not the VI register:
            // that only moves at the next VI, and by then the game may already be redrawing
            // the old one (shown half-drawn: the "models vanish for a moment" flicker).
            uint32_t origin = (uint32_t)osViGetNextFramebuffer();
            if (!origin) origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
            uint64_t frames = dl_count.load();
            if (frames == presented_frames && origin == presented_origin) return;
            if (!renderer->screen_ready(origin)) return; // half-drawn: try again next VI
            presented_frames = frames; presented_origin = origin;
            static int logged = 0;
            if (logged++ < 3) {
                auto* vi = ultramodern::renderer::get_vi_regs();
                printf("[vi] origin=%08X width=%u xscale=%08X yscale=%08X hstart=%08X vstart=%08X status=%08X\n", origin,
                    vi->VI_WIDTH_REG, vi->VI_X_SCALE_REG, vi->VI_Y_SCALE_REG, vi->VI_H_START_REG, vi->VI_V_START_REG, vi->VI_STATUS_REG);
            }
            if (renderer->present(origin)) after_present();
        }
        void shutdown() override {}
        uint32_t get_display_framerate() const override { return 60; }
        float get_resolution_scale() const override { return 1.0f; }
    private:
        void after_present() {
            uint64_t p = ++presents;
            if (!dump_prefix.empty() && (p % dump_every) == 0) dump_frame(p);
        }
        bool start() {
            if (renderer) return true;
            if (!init_egl()) return false;
            n64gfx::Config cfg;
            const char* s = getenv("NM_SCALE");
            cfg.scale = s ? std::max(1, atoi(s)) : 1;
            cfg.output_width = OUT_W; cfg.output_height = OUT_H;
            cfg.screen_height = 240;
            cfg.direct_to_screen = false;
            renderer = std::make_unique<n64gfx::Renderer>(rdram, cfg);
            return true;
        }
        uint8_t* rdram;
        std::unique_ptr<n64gfx::Renderer> renderer;
        uint64_t presented_frames = ~0ull;
        uint32_t presented_origin = 0;
    };

    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
        nm_set_rdram(rdram);
        return std::make_unique<GlesContext>(rdram);
    }
    void* create_gfx() { return nullptr; }
    ultramodern::renderer::WindowHandle create_window(void*) { return {}; }

    void update_gfx(void*) {
        static auto last = std::chrono::steady_clock::now();
        static uint64_t l_dl = 0, l_vi = 0, l_au = 0, l_gfx = 0, l_rsp = 0, l_tasks = 0;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        nm::perf::Window pw;
        if (nm::perf::update(pw)) printf("[perf] t=%.0fs %s\n", std::chrono::duration<double>(now - t_start).count(), nm::perf::summary(pw).c_str());
        if (dt >= 2.0 && getenv("NM_STATS")) {
            uint64_t d = dl_count, v = vi_count, a = audio_frames, g = gfx_us, r = rsp_us, t = rsp_tasks;
            printf("[stat] t=%.0fs fps=%.1f vi=%.1f audio=%.0f/s gfx=%.2fms/frame rsp=%.2fms/task (%.0f%%) polls=%llu\n",
                std::chrono::duration<double>(now - t_start).count(), (d - l_dl) / dt, (v - l_vi) / dt, (a - l_au) / dt,
                d > l_dl ? (g - l_gfx) / 1000.0 / (d - l_dl) : 0.0, t > l_tasks ? (r - l_rsp) / 1000.0 / (t - l_tasks) : 0.0,
                (r - l_rsp) / (dt * 1e4), (unsigned long long)polls.load());
            l_dl = d; l_vi = v; l_au = a; l_gfx = g; l_rsp = r; l_tasks = t; last = now;
        }
        if (std::chrono::duration<double>(now - t_start).count() > run_seconds) {
            printf("[headless] time up: %llu display lists, %llu presents\n", (unsigned long long)dl_count.load(), (unsigned long long)presents.load());
            { std::lock_guard lock{ audio_mutex }; wav_finish(); }
            fflush(stdout);
            _exit(0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    void message_box(const char* msg) { printf("[headless] MESSAGE: %s\n", msg); }

    RspUcodeFunc* real_ucode = nullptr;
    RspExitReason timed_ucode(uint8_t* rdram, uint32_t addr) {
        auto t0 = std::chrono::steady_clock::now();
        RspExitReason r = real_ucode(rdram, addr);
        rsp_us += usec(t0); rsp_tasks++;
        return r;
    }
    RspUcodeFunc* wrap_rsp(RspUcodeFunc* real) { real_ucode = real; return timed_ucode; }
}

std::filesystem::path nm::data_folder() {
    if (const char* d = getenv("NM_DATA")) return d;
    return std::filesystem::current_path() / "nm_data";
}
void nm::set_error(const std::string& msg) { printf("[headless] ERROR: %s\n", msg.c_str()); }

void nm_platform_early_init(int, char**) {
    signal(SIGSEGV, crash_handler); signal(SIGABRT, crash_handler); signal(SIGBUS, crash_handler); signal(SIGILL, crash_handler);
    t_start = std::chrono::steady_clock::now();
    nm::perf::install();
    if (const char* s = getenv("NM_SECONDS")) run_seconds = atof(s);
    if (const char* s = getenv("NM_DUMP")) dump_prefix = s;
    if (const char* s = getenv("NM_DUMP_EVERY")) dump_every = std::max(1, atoi(s));
    if (const char* s = getenv("NM_INPUT")) load_script(s);
    if (const char* s = getenv("NM_WAV")) { wav = fopen(s, "wb"); if (wav) fseek(wav, 44, SEEK_SET); }
    if (const char* s = getenv("NM_WAV2")) { wav2 = fopen(s, "wb"); } // raw 16-bit stereo, recompiled ucode (NM_AUDIO_COMPARE)
}

std::string nm_platform_find_rom(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') return argv[i];
    return "";
}

nm::Backend nm::make_backend(int, char**) {
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
