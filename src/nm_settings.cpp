// settings.ini: a few keys, written with defaults on first run, never overwritten after.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "nm_game.hpp"

namespace {
    nm::Settings g_settings;

    std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
    bool to_bool(const std::string& v) { return v == "true" || v == "1" || v == "yes" || v == "on"; }

    const char* default_ini =
        "; WWF No Mercy: Recompiled - settings\n"
        "\n"
        "[display]\n"
        "; native = the N64's own 320x240, 2x = twice that (sharper, slower)\n"
        "resolution = native\n"
        "; timings in the corner (SELECT + TRIANGLE toggles it)\n"
        "perf_overlay = true\n"
        "; 0 = draw every frame, 1 = draw every other frame (faster, choppier)\n"
        "frameskip = 0\n"
        "\n"
        "[audio]\n"
        "; 0-100\n"
        "volume = 100\n"
        "; extra frames of sound kept queued: raise if the sound crackles\n"
        "audio_latency = 2\n"
        "; auto, hle (fast) or recompiled (exact, slow on the Vita)\n"
        "audio_microcode = auto\n";

    std::filesystem::path settings_path() {
        return nm::data_folder().parent_path() / "settings.ini";
    }
}

nm::Settings& nm::settings() { return g_settings; }

void nm::init_settings() {
    std::filesystem::path p = settings_path();
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        std::ofstream out(p);
        out << default_ini;
    }
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line.substr(0, line.find(';')));
        if (line.empty() || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "resolution") g_settings.resolution = v;
        else if (k == "show_fps") g_settings.show_fps = to_bool(v);
        else if (k == "perf_overlay") g_settings.perf_overlay = to_bool(v);
        else if (k == "frameskip") g_settings.frameskip = std::atoi(v.c_str()) < 0 ? 0 : std::atoi(v.c_str()) > 3 ? 3 : std::atoi(v.c_str());
        else if (k == "audio_microcode") g_settings.audio_microcode = v;
        else if (k == "volume") g_settings.volume = std::atoi(v.c_str()) < 0 ? 0 : std::atoi(v.c_str()) > 100 ? 100 : std::atoi(v.c_str());
        else if (k == "audio_latency") g_settings.audio_latency = std::atoi(v.c_str()) < 0 ? 0 : std::atoi(v.c_str()) > 8 ? 8 : std::atoi(v.c_str());
    }
    printf("[settings] resolution=%s frameskip=%d volume=%d audio_latency=%d audio_microcode=%s\n",
        g_settings.resolution.c_str(), g_settings.frameskip, g_settings.volume, g_settings.audio_latency, g_settings.audio_microcode.c_str());
}
