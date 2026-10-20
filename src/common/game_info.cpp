#include "app/app.h"
#include "core/input.h"

void KeyboardWindow(void* hwnd);              // keyboard.cpp
void KeyboardPad(int port, input::Pad& pad);  // keyboard.cpp
bool MenuMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);  // menu.cpp
void MenuUi(double fps);                      // menu.cpp
extern bool g_fps_counter;                    // menu.cpp
extern bool g_pause_on_focus_loss;            // menu.cpp
void WidescreenInit();                        // widescreen.cpp

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Castlevania: Curse of Darkness",  // window and splash
    .short_title = L"Castlevania",        // message boxes
    .name = L"Castlevania",               // window classes, crash dumps, handover event
    .log = "castlevania",                 // log file prefix
    .settings = "castlevania.toml",       // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Castlevania"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"fps_counter", &g_fps_counter, nullptr},  // game settings kept in the settings file
                {"pause_on_focus_loss", &g_pause_on_focus_loss, nullptr}},
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = WidescreenInit,               // after the settings load, before the window and boot: the renderer extension
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = KeyboardWindow,             // the game window is up: keyboard focus and the menu's font
    .message = MenuMessage,               // window messages first; true takes one: F10 for the menu
    .ui = MenuUi,                         // ImGui drawing each frame: the settings menu and FPS counter
    .pad = KeyboardPad,                   // changes a polled pad: keyboard play, CV_PAD_SCRIPT, pause, menu
    .rumble = nullptr,                    // changes a rumble before it is set
};
