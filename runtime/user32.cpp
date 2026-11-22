#include "display.h"
#include "options.h"
#include "win32.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdio>
#include <deque>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

namespace {

constexpr uint32_t kMainWindow = 0x00010010u;

constexpr uint32_t WM_CREATE = 0x0001;
constexpr uint32_t WM_DESTROY = 0x0002;
constexpr uint32_t WM_ACTIVATE = 0x0006;
constexpr uint32_t WM_SETFOCUS = 0x0007;
constexpr uint32_t WM_CLOSE = 0x0010;
constexpr uint32_t WM_QUIT = 0x0012;
constexpr uint32_t WM_ACTIVATEAPP = 0x001c;
constexpr uint32_t WM_KEYDOWN = 0x0100;
constexpr uint32_t WM_KEYUP = 0x0101;
constexpr uint32_t WM_CHAR = 0x0102;
constexpr uint32_t WM_SYSKEYDOWN = 0x0104;
constexpr uint32_t WM_SYSKEYUP = 0x0105;
constexpr uint32_t WM_MOUSEMOVE = 0x0200;

struct Msg {
    uint32_t hwnd, message, wParam, lParam;
};

struct Key {
    uint16_t scan;
    uint8_t vk;
};

struct WindowClass {
    uint32_t wndProc;
};

std::map<std::string, WindowClass> g_classes;
uint32_t g_wndProc = 0;
SDL_Window* g_window = nullptr;
std::deque<Msg> g_queue;
bool g_shown = false;
bool g_shift = false;
bool g_alt = false;
uint32_t g_mouseButtons = 0;
int g_width = 640;
int g_height = 480;

const std::map<SDL_Scancode, Key>& keyTable() {
    static const std::map<SDL_Scancode, Key> t = {
        {SDL_SCANCODE_A, {0x1e, 'A'}}, {SDL_SCANCODE_B, {0x30, 'B'}}, {SDL_SCANCODE_C, {0x2e, 'C'}}, {SDL_SCANCODE_D, {0x20, 'D'}},
        {SDL_SCANCODE_E, {0x12, 'E'}}, {SDL_SCANCODE_F, {0x21, 'F'}}, {SDL_SCANCODE_G, {0x22, 'G'}}, {SDL_SCANCODE_H, {0x23, 'H'}},
        {SDL_SCANCODE_I, {0x17, 'I'}}, {SDL_SCANCODE_J, {0x24, 'J'}}, {SDL_SCANCODE_K, {0x25, 'K'}}, {SDL_SCANCODE_L, {0x26, 'L'}},
        {SDL_SCANCODE_M, {0x32, 'M'}}, {SDL_SCANCODE_N, {0x31, 'N'}}, {SDL_SCANCODE_O, {0x18, 'O'}}, {SDL_SCANCODE_P, {0x19, 'P'}},
        {SDL_SCANCODE_Q, {0x10, 'Q'}}, {SDL_SCANCODE_R, {0x13, 'R'}}, {SDL_SCANCODE_S, {0x1f, 'S'}}, {SDL_SCANCODE_T, {0x14, 'T'}},
        {SDL_SCANCODE_U, {0x16, 'U'}}, {SDL_SCANCODE_V, {0x2f, 'V'}}, {SDL_SCANCODE_W, {0x11, 'W'}}, {SDL_SCANCODE_X, {0x2d, 'X'}},
        {SDL_SCANCODE_Y, {0x15, 'Y'}}, {SDL_SCANCODE_Z, {0x2c, 'Z'}},
        {SDL_SCANCODE_1, {0x02, '1'}}, {SDL_SCANCODE_2, {0x03, '2'}}, {SDL_SCANCODE_3, {0x04, '3'}}, {SDL_SCANCODE_4, {0x05, '4'}},
        {SDL_SCANCODE_5, {0x06, '5'}}, {SDL_SCANCODE_6, {0x07, '6'}}, {SDL_SCANCODE_7, {0x08, '7'}}, {SDL_SCANCODE_8, {0x09, '8'}},
        {SDL_SCANCODE_9, {0x0a, '9'}}, {SDL_SCANCODE_0, {0x0b, '0'}},
        {SDL_SCANCODE_RETURN, {0x1c, 0x0d}}, {SDL_SCANCODE_ESCAPE, {0x01, 0x1b}}, {SDL_SCANCODE_BACKSPACE, {0x0e, 0x08}},
        {SDL_SCANCODE_TAB, {0x0f, 0x09}}, {SDL_SCANCODE_SPACE, {0x39, 0x20}}, {SDL_SCANCODE_MINUS, {0x0c, 0xbd}},
        {SDL_SCANCODE_EQUALS, {0x0d, 0xbb}}, {SDL_SCANCODE_LEFTBRACKET, {0x1a, 0xdb}}, {SDL_SCANCODE_RIGHTBRACKET, {0x1b, 0xdd}},
        {SDL_SCANCODE_BACKSLASH, {0x2b, 0xdc}}, {SDL_SCANCODE_SEMICOLON, {0x27, 0xba}}, {SDL_SCANCODE_APOSTROPHE, {0x28, 0xde}},
        {SDL_SCANCODE_GRAVE, {0x29, 0xc0}}, {SDL_SCANCODE_COMMA, {0x33, 0xbc}}, {SDL_SCANCODE_PERIOD, {0x34, 0xbe}},
        {SDL_SCANCODE_SLASH, {0x35, 0xbf}}, {SDL_SCANCODE_CAPSLOCK, {0x3a, 0x14}},
        {SDL_SCANCODE_F1, {0x3b, 0x70}}, {SDL_SCANCODE_F2, {0x3c, 0x71}}, {SDL_SCANCODE_F3, {0x3d, 0x72}}, {SDL_SCANCODE_F4, {0x3e, 0x73}},
        {SDL_SCANCODE_F5, {0x3f, 0x74}}, {SDL_SCANCODE_F6, {0x40, 0x75}}, {SDL_SCANCODE_F7, {0x41, 0x76}}, {SDL_SCANCODE_F8, {0x42, 0x77}},
        {SDL_SCANCODE_F9, {0x43, 0x78}}, {SDL_SCANCODE_F10, {0x44, 0x79}}, {SDL_SCANCODE_F11, {0x57, 0x7a}}, {SDL_SCANCODE_F12, {0x58, 0x7b}},
        {SDL_SCANCODE_SCROLLLOCK, {0x46, 0x91}}, {SDL_SCANCODE_PAUSE, {0x45, 0x13}}, {SDL_SCANCODE_NUMLOCKCLEAR, {0x145, 0x90}},
        {SDL_SCANCODE_INSERT, {0x152, 0x2d}}, {SDL_SCANCODE_HOME, {0x147, 0x24}}, {SDL_SCANCODE_PAGEUP, {0x149, 0x21}},
        {SDL_SCANCODE_DELETE, {0x153, 0x2e}}, {SDL_SCANCODE_END, {0x14f, 0x23}}, {SDL_SCANCODE_PAGEDOWN, {0x151, 0x22}},
        {SDL_SCANCODE_RIGHT, {0x14d, 0x27}}, {SDL_SCANCODE_LEFT, {0x14b, 0x25}}, {SDL_SCANCODE_DOWN, {0x150, 0x28}}, {SDL_SCANCODE_UP, {0x148, 0x26}},
        {SDL_SCANCODE_KP_DIVIDE, {0x135, 0x6f}}, {SDL_SCANCODE_KP_MULTIPLY, {0x37, 0x6a}}, {SDL_SCANCODE_KP_MINUS, {0x4a, 0x6d}},
        {SDL_SCANCODE_KP_PLUS, {0x4e, 0x6b}}, {SDL_SCANCODE_KP_ENTER, {0x11c, 0x0d}}, {SDL_SCANCODE_KP_PERIOD, {0x53, 0x6e}},
        {SDL_SCANCODE_KP_0, {0x52, 0x60}}, {SDL_SCANCODE_KP_1, {0x4f, 0x61}}, {SDL_SCANCODE_KP_2, {0x50, 0x62}}, {SDL_SCANCODE_KP_3, {0x51, 0x63}},
        {SDL_SCANCODE_KP_4, {0x4b, 0x64}}, {SDL_SCANCODE_KP_5, {0x4c, 0x65}}, {SDL_SCANCODE_KP_6, {0x4d, 0x66}}, {SDL_SCANCODE_KP_7, {0x47, 0x67}},
        {SDL_SCANCODE_KP_8, {0x48, 0x68}}, {SDL_SCANCODE_KP_9, {0x49, 0x69}},
        {SDL_SCANCODE_LCTRL, {0x1d, 0x11}}, {SDL_SCANCODE_LSHIFT, {0x2a, 0x10}}, {SDL_SCANCODE_LALT, {0x38, 0x12}},
        {SDL_SCANCODE_RCTRL, {0x11d, 0x11}}, {SDL_SCANCODE_RSHIFT, {0x36, 0x10}}, {SDL_SCANCODE_RALT, {0x138, 0x12}},
        {SDL_SCANCODE_LGUI, {0x15b, 0x5b}}, {SDL_SCANCODE_RGUI, {0x15c, 0x5c}},
    };
    return t;
}

SDL_Scancode scancodeFromPc(uint32_t scan) {
    for (auto& [sdl, k] : keyTable()) {
        if (k.scan == scan) {
            return sdl;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

void post(uint32_t message, uint32_t wParam, uint32_t lParam) {
    g_queue.push_back({kMainWindow, message, wParam, lParam});
}

struct Letterbox {
    float scale;
    float x;
    float y;
};

Letterbox letterbox() {
    int w = 1, h = 1;
    SDL_GetWindowSize(g_window, &w, &h);
    float scale = std::min(float(w) / g_width, float(h) / g_height);
    return {scale, (w - g_width * scale) / 2, (h - g_height * scale) / 2};
}

uint32_t mouseLParam(float x, float y) {
    Letterbox lb = letterbox();
    int gx = std::clamp(int((x - lb.x) / lb.scale), 0, g_width - 1);
    int gy = std::clamp(int((y - lb.y) / lb.scale), 0, g_height - 1);
    return (uint32_t(uint16_t(gy)) << 16) | uint16_t(gx);
}

struct ScriptedKey {
    uint64_t time;
    SDL_Scancode scancode;
    bool down;
};

std::vector<ScriptedKey> loadKeyScript() {
    std::vector<ScriptedKey> keys;
    const char* spec = std::getenv("NOC_KEYS");
    if (!spec) {
        return keys;
    }
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t colon = item.find(':');
        if (colon == std::string::npos) {
            std::fprintf(stderr, "NOC_KEYS: expected <ms>:<key>, got \"%s\"\n", item.c_str());
            continue;
        }
        uint64_t t = std::stoull(item.substr(0, colon));
        std::string name = item.substr(colon + 1);
        uint64_t hold = 100;
        if (size_t second = name.find(':'); second != std::string::npos) {
            hold = std::stoull(name.substr(second + 1));
            name = name.substr(0, second);
        }
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) {
            std::fprintf(stderr, "NOC_KEYS: unknown key \"%s\"\n", name.c_str());
            continue;
        }
        keys.push_back({t, sc, true});
        keys.push_back({t + hold, sc, false});
    }
    std::stable_sort(keys.begin(), keys.end(), [](const ScriptedKey& x, const ScriptedKey& y) { return x.time < y.time; });
    return keys;
}

void runKeyScript() {
    static const std::vector<ScriptedKey> keys = loadKeyScript();
    static size_t next = 0;
    uint64_t now = SDL_GetTicks();
    for (; next < keys.size() && keys[next].time <= now; next++) {
        SDL_Event e {};
        e.type = keys[next].down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = keys[next].scancode;
        e.key.down = keys[next].down;
        SDL_PushEvent(&e);
    }
}

void runDumpScript() {
    static std::vector<uint64_t> times = [] {
        std::vector<uint64_t> t;
        if (const char* spec = std::getenv("NOC_DUMPS")) {
            std::stringstream ss(spec);
            std::string item;
            while (std::getline(ss, item, ',')) {
                t.push_back(std::stoull(item));
            }
        }
        return t;
    }();
    static size_t next = 0;
    const char* dir = std::getenv("NOC_DUMP_DIR");
    for (; dir && next < times.size() && times[next] <= SDL_GetTicks(); next++) {
        std::string path = std::string(dir) + "/dump_" + std::to_string(times[next]) + ".bin";
        guest_dump(path);
        std::fprintf(stderr, "[debug] wrote %s\n", path.c_str());
    }
}

void pumpEvents() {
    runKeyScript();
    runDumpScript();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            post(WM_CLOSE, 0, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            post(WM_ACTIVATEAPP, 1, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            post(WM_ACTIVATEAPP, 0, 0);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            if (e.key.scancode == SDL_SCANCODE_F8 || e.key.scancode == SDL_SCANCODE_F10) {
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                    if (e.key.scancode == SDL_SCANCODE_F8) {
                        options_cycle_battery_drain();
                    } else {
                        options_toggle_textured_night_vision();
                    }
                }
                break;
            }
            auto it = keyTable().find(e.key.scancode);
            if (it == keyTable().end()) {
                break;
            }
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            const Key& k = it->second;
            if (k.vk == 0x10) {
                g_shift = down;
            }
            if (k.vk == 0x12) {
                g_alt = down;
            }
            uint32_t lp = 1u | (uint32_t(k.scan & 0xff) << 16) | ((k.scan & 0x100) ? (1u << 24) : 0);
            if (g_alt) {
                lp |= 1u << 29;
            }
            if (!down) {
                lp |= (1u << 30) | (1u << 31);
            } else if (e.key.repeat) {
                lp |= 1u << 30;
            }
            bool sys = g_alt || k.vk == 0x12 || k.vk == 0x79;
            post(down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), k.vk, lp);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            post(WM_MOUSEMOVE, g_mouseButtons, mouseLParam(e.motion.x, e.motion.y));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            uint32_t bit = 0;
            uint32_t msg = 0;
            switch (e.button.button) {
            case SDL_BUTTON_LEFT: bit = 0x1; msg = down ? 0x201 : 0x202; break;
            case SDL_BUTTON_RIGHT: bit = 0x2; msg = down ? 0x204 : 0x205; break;
            case SDL_BUTTON_MIDDLE: bit = 0x10; msg = down ? 0x207 : 0x208; break;
            default: break;
            }
            if (!msg) {
                break;
            }
            g_mouseButtons = down ? (g_mouseButtons | bit) : (g_mouseButtons & ~bit);
            post(msg, g_mouseButtons, mouseLParam(e.button.x, e.button.y));
            break;
        }
        default:
            break;
        }
    }
}

char charForKey(uint8_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return g_shift ? char(vk) : char(vk - 'A' + 'a');
    }
    if (vk >= '0' && vk <= '9') {
        return g_shift ? ")!@#$%^&*("[vk - '0'] : char(vk);
    }
    switch (vk) {
    case 0x20: return ' ';
    case 0x0d: return '\r';
    case 0x08: return '\b';
    case 0x1b: return 0x1b;
    case 0x09: return '\t';
    case 0xbd: return g_shift ? '_' : '-';
    case 0xbb: return g_shift ? '+' : '=';
    case 0xbc: return g_shift ? '<' : ',';
    case 0xbe: return g_shift ? '>' : '.';
    case 0xbf: return g_shift ? '?' : '/';
    case 0xba: return g_shift ? ':' : ';';
    case 0xde: return g_shift ? '"' : '\'';
    default: return 0;
    }
}

std::string className(uint32_t p) {
    if (p < 0x10000) {
        return "#" + std::to_string(p);
    }
    return gstring(p);
}

}

SDL_Window* user32_window() {
    return g_window;
}

HLE("user32", FindWindowA, 2) { return 0; }

HLE("user32", RegisterClassA, 1) {
    uint32_t wc = a[0];
    std::string name = className(rd32(m, wc + 36));
    g_classes[name] = {rd32(m, wc + 4)};
    return 0xc000 + uint32_t(g_classes.size());
}

HLE("user32", CreateWindowExA, 12) {
    std::string cls = className(a[1]);
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) {
        std::fprintf(stderr, "[user32] CreateWindowExA: unknown class \"%s\"\n", cls.c_str());
        return 0;
    }
    if (g_window) {
        std::fprintf(stderr, "[user32] CreateWindowExA: only one window is supported\n");
        return 0;
    }
    int w = int32_t(a[6]) > 0 ? int32_t(a[6]) : 640;
    int h = int32_t(a[7]) > 0 ? int32_t(a[7]) : 480;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }
    std::string title = a[2] ? gstring(a[2]) : "Nocturne";
    display_prepare_window_attributes();
    g_window = SDL_CreateWindow(title.c_str(), w, h, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (!g_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    g_width = w;
    g_height = h;
    g_wndProc = it->second.wndProc;
    std::fprintf(stderr, "[user32] window \"%s\" %dx%d style 0x%08x ex 0x%08x\n", title.c_str(), w, h, a[3], a[0]);
    uint32_t cs = guest_alloc(48, 4);
    wr32(m, cs + 0, a[11]);
    wr32(m, cs + 4, a[10]);
    wr32(m, cs + 8, a[9]);
    wr32(m, cs + 12, a[8]);
    wr32(m, cs + 16, a[7]);
    wr32(m, cs + 20, a[6]);
    wr32(m, cs + 24, a[5]);
    wr32(m, cs + 28, a[4]);
    wr32(m, cs + 32, a[3]);
    wr32(m, cs + 36, a[2]);
    wr32(m, cs + 40, a[1]);
    wr32(m, cs + 44, a[0]);
    uint32_t r = call_guest(c, m, g_wndProc, {kMainWindow, WM_CREATE, 0, cs});
    guest_free(cs);
    if (int32_t(r) == -1) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return 0;
    }
    return kMainWindow;
}

HLE("user32", ShowWindow, 2) {
    if (a[1] != 0 && !g_shown) {
        SDL_ShowWindow(g_window);
        g_shown = true;
        post(WM_ACTIVATEAPP, 1, 0);
        post(WM_ACTIVATE, 1, 0);
        post(WM_SETFOCUS, 0, 0);
    }
    return 0;
}

HLE("user32", UpdateWindow, 1) { return 1; }

HLE("user32", PeekMessageA, 5) {
    pumpEvents();
    uint32_t lo = a[2];
    uint32_t hi = a[3];
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        if ((lo || hi) && (it->message < lo || it->message > hi)) {
            continue;
        }
        uint32_t p = a[0];
        wr32(m, p + 0, it->hwnd);
        wr32(m, p + 4, it->message);
        wr32(m, p + 8, it->wParam);
        wr32(m, p + 12, it->lParam);
        wr32(m, p + 16, uint32_t(SDL_GetTicks()));
        wr32(m, p + 20, 0);
        wr32(m, p + 24, 0);
        if (a[4] & 1) {
            g_queue.erase(it);
        }
        return 1;
    }
    return 0;
}

HLE("user32", TranslateMessage, 1) {
    uint32_t msg = rd32(m, a[0] + 4);
    if (msg != WM_KEYDOWN) {
        return 0;
    }
    char ch = charForKey(uint8_t(rd32(m, a[0] + 8)));
    if (!ch) {
        return 0;
    }
    g_queue.push_front({rd32(m, a[0]), WM_CHAR, uint32_t(uint8_t(ch)), rd32(m, a[0] + 12)});
    return 1;
}

HLE("user32", DispatchMessageA, 1) {
    uint32_t p = a[0];
    uint32_t hwnd = rd32(m, p);
    if (hwnd != kMainWindow || !g_wndProc) {
        return 0;
    }
    return call_guest(c, m, g_wndProc, {hwnd, rd32(m, p + 4), rd32(m, p + 8), rd32(m, p + 12)});
}

HLE("user32", DefWindowProcA, 4) {
    if (a[1] == WM_CLOSE) {
        call_guest(c, m, g_wndProc, {a[0], WM_DESTROY, 0, 0});
    }
    return 0;
}

HLE("user32", PostQuitMessage, 1) {
    post(WM_QUIT, a[0], 0);
    return 0;
}

HLE("user32", MessageBoxA, 4) {
    std::string text = gstring(a[1]);
    std::string caption = gstring(a[2]);
    std::fprintf(stderr, "[user32] MessageBox \"%s\": %s\n", caption.c_str(), text.c_str());
    if (text == "Unable to open .AVI!") {
        return 1;
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || std::strcmp(driver, "offscreen") != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, caption.c_str(), text.c_str(), g_window);
    }
    return 1;
}

HLE("user32", LoadIconA, 2) { return 0x20001; }
HLE("user32", LoadCursorA, 2) { return 0x20002; }

HLE("user32", SetCursor, 1) {
    if (a[0]) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
    return 0x20002;
}

HLE("user32", SetCursorPos, 2) {
    if (g_window) {
        Letterbox lb = letterbox();
        SDL_WarpMouseInWindow(g_window, lb.x + float(int32_t(a[0])) * lb.scale, lb.y + float(int32_t(a[1])) * lb.scale);
    }
    return 1;
}

HLE("user32", GetClientRect, 2) {
    wr32(m, a[1] + 0, 0);
    wr32(m, a[1] + 4, 0);
    wr32(m, a[1] + 8, uint32_t(g_width));
    wr32(m, a[1] + 12, uint32_t(g_height));
    return 1;
}

HLE("user32", MoveWindow, 6) {
    int w = int32_t(a[3]);
    int h = int32_t(a[4]);
    if (g_window && w > 0 && h > 0) {
        g_width = w;
        g_height = h;
        SDL_SetWindowSize(g_window, w, h);
    }
    return 1;
}

HLE("user32", SetFocus, 1) { return a[0]; }
HLE("user32", SetForegroundWindow, 1) { return 1; }
HLE("user32", BringWindowToTop, 1) { return 1; }
HLE("user32", IsIconic, 1) { return 0; }
HLE("user32", GetLastActivePopup, 1) { return a[0]; }
HLE("user32", InvalidateRect, 3) { return 1; }

HLE("user32", SetRectEmpty, 1) {
    std::memset(g_mem + a[0], 0, 16);
    return 1;
}

HLE("user32", GetKeyNameTextA, 3) {
    uint32_t scan = ((a[0] >> 16) & 0xff) | ((a[0] & (1u << 24)) ? 0x100 : 0);
    SDL_Scancode sc = scancodeFromPc(scan);
    const char* name = sc == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(sc);
    return copy_string_out(a[1], a[2], name);
}

HLE("user32", CharUpperBuffA, 2) {
    for (uint32_t i = 0; i < a[1]; i++) {
        m[a[0] + i] = uint8_t(std::toupper(m[a[0] + i]));
    }
    return a[1];
}

HLE("user32", OpenClipboard, 1) { return 0; }
HLE("user32", CloseClipboard, 0) { return 1; }
HLE("user32", GetClipboardData, 1) { return 0; }
HLE("user32", SetClipboardData, 2) { return 0; }
