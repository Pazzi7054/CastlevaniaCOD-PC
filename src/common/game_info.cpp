#include "app/app.h"
#include "core/input.h"

void KeyboardWindow(void* hwnd);                // keyboard.cpp
void KeyboardPad(int port, input::Pad& pad);  // keyboard.cpp

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Castlevania: Curse of Darkness",  // window and splash
    .short_title = L"Castlevania",        // message boxes
    .name = L"Castlevania",               // window classes, crash dumps, handover event
    .log = "castlevania",                 // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Castlevania"}},  // each recompiled xbe and its exe, for title launches
    .options = {},                        // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = KeyboardWindow,             // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = nullptr,                        // ImGui drawing each frame
    .pad = KeyboardPad,                   // changes a polled pad: keyboard play and CV_PAD_SCRIPT
    .rumble = nullptr,                    // changes a rumble before it is set
};
