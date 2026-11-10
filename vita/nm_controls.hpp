#pragma once
// Remappable controls and the in-game controls menu (SELECT + START).
// Plain C++ with no Vita SDK calls, so the mapping and menu logic can be tested on a PC.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nm::controls {
    // Vita inputs that can be assigned an N64 button.
    enum Source {
        CROSS, CIRCLE, SQUARE, TRIANGLE, L, R, START, SELECT,
        DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
        REAR_LEFT, REAR_RIGHT, FRONT_LEFT, FRONT_RIGHT,
        SOURCE_COUNT
    };
    // What each stick does.
    enum StickMode { STICK_DPAD, STICK_ANALOG, STICK_CBUTTONS, STICK_OFF, STICK_MODE_COUNT };

    // N64 controller bits (as the game reads them).
    constexpr uint16_t N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000,
        N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100,
        N64_L = 0x0020, N64_R = 0x0010, N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001;

    struct Config {
        int target[SOURCE_COUNT];   // index into the N64 target list (0 = nothing)
        int left_stick, right_stick;
    };
    Config defaults();
    Config& current();
    void load(const std::filesystem::path& file);   // missing file: defaults (and writes it)
    bool save(const std::filesystem::path& file);

    struct Output { uint16_t buttons = 0; float x = 0, y = 0; };
    // sources: bit i set = Source i held. Sticks in -1..1 (y down positive, as the Vita reports).
    Output translate(const Config& c, uint32_t sources, float lx, float ly, float rx, float ry);

    // ---- Menu ----
    // Fixed navigation (not affected by the mapping): up/down choose, left/right change,
    // CROSS activates (reset / close), CIRCLE or START = close and save.
    enum Nav : uint32_t { NAV_UP = 1, NAV_DOWN = 2, NAV_LEFT = 4, NAV_RIGHT = 8, NAV_OK = 16, NAV_BACK = 32 };
    struct Menu {
        bool open = false;
        int cursor = 0;
        bool changed = true;        // needs redrawing
        std::string status;
        // Call ~60 times a second while open: nav = buttons held (Nav bits), now in ms.
        // Returns true when the menu closed this call (caller saves).
        bool update(uint32_t nav, uint64_t now_ms);
        std::vector<std::string> lines(int& highlight) const;
    private:
        uint32_t last_nav = 0; uint64_t repeat_at = 0;
    };
    int row_count();
    const char* source_name(int s);
    const char* target_name(int t);
    int target_count();
}
