// Keyboard play on port 1, and a scripted pad for unattended test runs.
//
// The keyboard is read only while the game window is in front, and adds to a real controller on the same port
// rather than replacing it. Port 1 always reports a controller, so the game can be played with the keyboard alone.
//
//   WASD left stick         arrows right stick (camera)    numpad 8 2 4 6 d-pad
//   K or Space  A (jump)    J  X (attack)                  L  B         I  Y
//   Q  left trigger         E  right trigger               Z  white     C  black
//   Enter  Start            Backspace  Back
//
// CV_PAD_SCRIPT="<ms>:<button>[+<button>...][:<hold ms>],..." presses buttons at times measured from the game's
// first read of the pad (default hold 150 ms). Buttons: up down left right start back a b x y black white lt rt
// lsu lsd lsl lsr (left stick). "@file" reads the same entries from a file, one per line or comma separated.
#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/input.h"

namespace {

HWND g_hwnd = nullptr;

enum : uint16_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kStart = 0x10, kBack = 0x20 };
enum { kA, kB, kX, kY, kBlack, kWhite, kLT, kRT };

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool Focused() {
    HWND fg = GetForegroundWindow();
    if (!fg || !g_hwnd) return false;
    DWORD a = 0, b = 0;
    GetWindowThreadProcessId(fg, &a);
    GetWindowThreadProcessId(g_hwnd, &b);
    return a == b;
}

// one stick axis from two keys, full deflection
int16_t Axis(int neg, int pos) {
    int v = (Down(pos) ? 32767 : 0) - (Down(neg) ? 32767 : 0);
    return int16_t(v);
}

void Keyboard(input::Pad& p) {
    if (!Focused()) return;
    if (Down(VK_NUMPAD8)) p.buttons |= kUp;
    if (Down(VK_NUMPAD2)) p.buttons |= kDown;
    if (Down(VK_NUMPAD4)) p.buttons |= kLeft;
    if (Down(VK_NUMPAD6)) p.buttons |= kRight;
    if (Down(VK_RETURN)) p.buttons |= kStart;
    if (Down(VK_BACK)) p.buttons |= kBack;
    auto press = [&](int slot, bool on) { if (on) p.analog[slot] = 0xFF; };
    press(kA, Down('K') || Down(VK_SPACE));
    press(kX, Down('J'));
    press(kB, Down('L'));
    press(kY, Down('I'));
    press(kWhite, Down('Z'));
    press(kBlack, Down('C'));
    press(kLT, Down('Q'));
    press(kRT, Down('E'));
    // the keyboard wins over a centred stick, a moved stick keeps its value
    int16_t lx = Axis('A', 'D'), ly = Axis('S', 'W'), rx = Axis(VK_LEFT, VK_RIGHT), ry = Axis(VK_DOWN, VK_UP);
    if (lx) p.lx = lx;
    if (ly) p.ly = ly;
    if (rx) p.rx = rx;
    if (ry) p.ry = ry;
}

struct Step {
    int64_t at, until;
    uint16_t buttons;
    uint32_t analog;  // bit per analog slot
    int16_t lx, ly;
};

std::vector<Step> ParseScript(std::string text) {
    std::vector<Step> steps;
    for (char& c : text)
        if (c == '\n' || c == '\r') c = ',';
    std::stringstream all(text);
    std::string entry;
    while (std::getline(all, entry, ',')) {
        if (entry.empty() || entry[0] == '#') continue;
        std::stringstream e(entry);
        std::string at, names, hold;
        std::getline(e, at, ':');
        std::getline(e, names, ':');
        std::getline(e, hold, ':');
        if (at.empty() || names.empty()) continue;
        Step s{std::atoll(at.c_str()), 0, 0, 0, 0, 0};
        s.until = s.at + (hold.empty() ? 150 : std::atoll(hold.c_str()));
        std::stringstream n(names);
        std::string b;
        while (std::getline(n, b, '+')) {
            if (b == "up") s.buttons |= kUp;
            else if (b == "down") s.buttons |= kDown;
            else if (b == "left") s.buttons |= kLeft;
            else if (b == "right") s.buttons |= kRight;
            else if (b == "start") s.buttons |= kStart;
            else if (b == "back") s.buttons |= kBack;
            else if (b == "a") s.analog |= 1u << kA;
            else if (b == "b") s.analog |= 1u << kB;
            else if (b == "x") s.analog |= 1u << kX;
            else if (b == "y") s.analog |= 1u << kY;
            else if (b == "black") s.analog |= 1u << kBlack;
            else if (b == "white") s.analog |= 1u << kWhite;
            else if (b == "lt") s.analog |= 1u << kLT;
            else if (b == "rt") s.analog |= 1u << kRT;
            else if (b == "lsu") s.ly = 32767;
            else if (b == "lsd") s.ly = -32767;
            else if (b == "lsl") s.lx = -32767;
            else if (b == "lsr") s.lx = 32767;
        }
        steps.push_back(s);
    }
    return steps;
}

const std::vector<Step>& Script() {
    static const std::vector<Step> steps = [] {
        const char* v = std::getenv("CV_PAD_SCRIPT");
        if (!v || !*v) return std::vector<Step>{};
        std::string text = v;
        if (text[0] == '@') {
            std::ifstream f(text.substr(1));
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        }
        return ParseScript(text);
    }();
    return steps;
}

void Scripted(input::Pad& p) {
    const auto& steps = Script();
    if (steps.empty()) return;
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
    for (const Step& s : steps) {
        if (now < s.at || now >= s.until) continue;
        p.buttons |= s.buttons;
        for (int i = 0; i < 8; i++)
            if (s.analog >> i & 1) p.analog[i] = 0xFF;
        if (s.lx) p.lx = s.lx;
        if (s.ly) p.ly = s.ly;
    }
}

}  // namespace

void KeyboardWindow(void* hwnd) { g_hwnd = static_cast<HWND>(hwnd); }

void KeyboardPad(int port, input::Pad& p) {
    if (port != 0) return;
    p.connected = true;
    Keyboard(p);
    Scripted(p);
}
