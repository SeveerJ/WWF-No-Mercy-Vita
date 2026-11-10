#include "nm_controls.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace nm::controls {
namespace {
    struct Target { const char* key; const char* label; uint16_t bits; };
    // Labels name the N64 button and, where it is unambiguous, what it does in No Mercy.
    const Target targets[] = {
        { "none", "NOTHING", 0 },
        { "A", "A  GRAPPLE", N64_A },
        { "B", "B  STRIKE", N64_B },
        { "Z", "Z", N64_Z },
        { "START", "START", N64_START },
        { "L", "L", N64_L },
        { "R", "R", N64_R },
        { "C_UP", "C-UP  CLIMB / ROPES", N64_CU },
        { "C_DOWN", "C-DOWN  RUN", N64_CD },
        { "C_LEFT", "C-LEFT", N64_CL },
        { "C_RIGHT", "C-RIGHT", N64_CR },
        { "D_UP", "D-PAD UP", N64_DU },
        { "D_DOWN", "D-PAD DOWN", N64_DD },
        { "D_LEFT", "D-PAD LEFT", N64_DL },
        { "D_RIGHT", "D-PAD RIGHT", N64_DR },
    };
    constexpr int TARGETS = sizeof(targets) / sizeof(targets[0]);

    const char* source_keys[SOURCE_COUNT] = {
        "cross", "circle", "square", "triangle", "l", "r", "start", "select",
        "dpad_up", "dpad_down", "dpad_left", "dpad_right",
        "rear_touch_left", "rear_touch_right", "front_touch_left", "front_touch_right",
    };
    const char* source_labels[SOURCE_COUNT] = {
        "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L", "R", "START", "SELECT",
        "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT",
        "REAR TOUCH LEFT", "REAR TOUCH RIGHT", "FRONT TOUCH LEFT", "FRONT TOUCH RIGHT",
    };
    const char* stick_keys[STICK_MODE_COUNT] = { "dpad", "analog", "c_buttons", "off" };
    const char* stick_labels[STICK_MODE_COUNT] = { "D-PAD (MOVE)", "N64 STICK (TAUNTS)", "C-BUTTONS", "OFF" };

    int target_by_key(const std::string& k) {
        for (int i = 0; i < TARGETS; i++) if (k == targets[i].key) return i;
        return -1;
    }
    int find_bits(uint16_t bits) {
        for (int i = 0; i < TARGETS; i++) if (targets[i].bits == bits) return i;
        return 0;
    }
    std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }

    Config g_config = defaults();

    // Menu rows after the button rows.
    enum { ROW_LSTICK = SOURCE_COUNT, ROW_RSTICK, ROW_RESET, ROW_CLOSE, ROWS };
}

int row_count() { return ROWS; }
const char* source_name(int s) { return s >= 0 && s < SOURCE_COUNT ? source_labels[s] : "?"; }
const char* target_name(int t) { return t >= 0 && t < TARGETS ? targets[t].label : "?"; }
int target_count() { return TARGETS; }

Config defaults() {
    Config c{};
    c.target[CROSS] = find_bits(N64_A);
    c.target[SQUARE] = find_bits(N64_B);
    c.target[CIRCLE] = find_bits(N64_CD);
    c.target[TRIANGLE] = find_bits(N64_CU);
    c.target[L] = find_bits(N64_L);
    c.target[R] = find_bits(N64_R);
    c.target[START] = find_bits(N64_START);
    c.target[SELECT] = find_bits(N64_Z);
    c.target[DPAD_UP] = find_bits(N64_DU);
    c.target[DPAD_DOWN] = find_bits(N64_DD);
    c.target[DPAD_LEFT] = find_bits(N64_DL);
    c.target[DPAD_RIGHT] = find_bits(N64_DR);
    c.target[REAR_LEFT] = find_bits(N64_CL);
    c.target[REAR_RIGHT] = find_bits(N64_CR);
    c.target[FRONT_LEFT] = 0;
    c.target[FRONT_RIGHT] = 0;
    c.left_stick = STICK_DPAD;
    c.right_stick = STICK_ANALOG;
    return c;
}

Config& current() { return g_config; }

void load(const std::filesystem::path& file) {
    g_config = defaults();
    std::ifstream in(file);
    if (!in) { save(file); return; }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line.substr(0, line.find(';')));
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '[' || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        for (int s = 0; s < SOURCE_COUNT; s++) {
            if (k != source_keys[s]) continue;
            int t = target_by_key(v);
            if (t >= 0) g_config.target[s] = t;
        }
        for (int m = 0; m < STICK_MODE_COUNT; m++) {
            if (v != stick_keys[m]) continue;
            if (k == "left_stick") g_config.left_stick = m;
            if (k == "right_stick") g_config.right_stick = m;
        }
    }
    printf("[controls] loaded %s\n", file.string().c_str());
}

bool save(const std::filesystem::path& file) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << "; WWF No Mercy: Recompiled - controls. Change them in game: press SELECT + START.\n"
           "; Buttons: none, A, B, Z, START, L, R, C_UP, C_DOWN, C_LEFT, C_RIGHT, D_UP, D_DOWN, D_LEFT, D_RIGHT\n"
           "; Sticks: dpad, analog (the N64 stick: taunts), c_buttons, off\n\n";
    for (int s = 0; s < SOURCE_COUNT; s++) out << source_keys[s] << " = " << targets[g_config.target[s]].key << "\n";
    out << "left_stick = " << stick_keys[g_config.left_stick] << "\n";
    out << "right_stick = " << stick_keys[g_config.right_stick] << "\n";
    return (bool)out;
}

Output translate(const Config& c, uint32_t sources, float lx, float ly, float rx, float ry) {
    Output o;
    for (int s = 0; s < SOURCE_COUNT; s++)
        if (sources & (1u << s)) o.buttons |= targets[c.target[s]].bits;
    auto stick = [&](int mode, float x, float y) {
        switch (mode) {
        case STICK_DPAD:
            if (y < -0.6f) o.buttons |= N64_DU; if (y > 0.6f) o.buttons |= N64_DD;
            if (x < -0.6f) o.buttons |= N64_DL; if (x > 0.6f) o.buttons |= N64_DR;
            break;
        case STICK_CBUTTONS:
            if (y < -0.6f) o.buttons |= N64_CU; if (y > 0.6f) o.buttons |= N64_CD;
            if (x < -0.6f) o.buttons |= N64_CL; if (x > 0.6f) o.buttons |= N64_CR;
            break;
        case STICK_ANALOG:
            // Both sticks on analog: the one pushed further wins.
            if (std::fabs(x) + std::fabs(y) > std::fabs(o.x) + std::fabs(o.y)) { o.x = x; o.y = -y; }
            break;
        default: break;
        }
    };
    stick(c.left_stick, lx, ly);
    stick(c.right_stick, rx, ry);
    return o;
}

bool Menu::update(uint32_t nav, uint64_t now_ms) {
    // Act on a new press, then repeat while held (after 400 ms, every 110 ms).
    uint32_t fresh = nav & ~last_nav;
    uint32_t act = fresh;
    if (nav && nav == last_nav && now_ms >= repeat_at) { act = nav & (NAV_UP | NAV_DOWN | NAV_LEFT | NAV_RIGHT); repeat_at = now_ms + 110; }
    if (fresh) repeat_at = now_ms + 400;
    last_nav = nav;
    if (!act) return false;
    Config& c = g_config;
    changed = true;
    status.clear();
    if (act & NAV_UP) cursor = (cursor + ROWS - 1) % ROWS;
    if (act & NAV_DOWN) cursor = (cursor + 1) % ROWS;
    int step = (act & NAV_LEFT) ? -1 : (act & NAV_RIGHT) ? 1 : 0;
    if (step) {
        if (cursor < SOURCE_COUNT) c.target[cursor] = (c.target[cursor] + TARGETS + step) % TARGETS;
        else if (cursor == ROW_LSTICK) c.left_stick = (c.left_stick + STICK_MODE_COUNT + step) % STICK_MODE_COUNT;
        else if (cursor == ROW_RSTICK) c.right_stick = (c.right_stick + STICK_MODE_COUNT + step) % STICK_MODE_COUNT;
    }
    if (act & NAV_OK) {
        if (cursor == ROW_RESET) { c = defaults(); status = "RESET TO DEFAULTS"; }
        else if (cursor == ROW_CLOSE) { open = false; return true; }
    }
    if (act & NAV_BACK) { open = false; return true; }
    return false;
}

std::vector<std::string> Menu::lines(int& highlight) const {
    const Config& c = g_config;
    std::vector<std::string> out;
    out.push_back("CONTROLS");
    out.push_back("");
    auto row = [&](const std::string& name, const std::string& value) {
        std::string s = name;
        while (s.size() < 19) s += ' ';
        out.push_back(s + value);
    };
    int first = (int)out.size();
    for (int s = 0; s < SOURCE_COUNT; s++) row(source_labels[s], targets[c.target[s]].label);
    row("LEFT STICK", stick_labels[c.left_stick]);
    row("RIGHT STICK", stick_labels[c.right_stick]);
    row("RESET TO DEFAULTS", "");
    row("SAVE AND CLOSE", "");
    highlight = first + cursor;
    out.push_back("");
    out.push_back(status.empty() ? "UP/DOWN CHOOSE   LEFT/RIGHT CHANGE" : status);
    out.push_back("CIRCLE: SAVE AND CLOSE");
    out.push_back("THE GAME KEEPS RUNNING: PAUSE IT FIRST");
    return out;
}
}
