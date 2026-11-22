#include "app/app.h"

#include <atomic>
#include <chrono>

#include "core/input.h"

namespace {

// [TCSOLA patch, after the Red Dead Revolver port's keyboard port that always reads as connected]
// A controller blip (Bluetooth or wireless reconnects, Windows re-enumerating the pad) reached the game as an unplug.
// True Crime then drops into its "reconnect the controller" loop, which draws a frame and sleeps 200 ms: the
// 5 fps stutter. Port 1 now always reads as connected (still, with no buttons, while no pad is there), and a pad on
// another port has to be gone for 3 seconds before the game is told it was removed.
void TcPad(int port, input::Pad& p) {
    static std::atomic<int64_t> last_seen[4] = {};
    if (port < 0 || port > 3) return;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
    if (p.connected) {
        last_seen[port] = now;
        return;
    }
    int64_t seen = last_seen[port];
    if (port == 0 || (seen && now - seen < 3000)) {
        p = input::Pad{};
        p.connected = true;
    }
}

}  // namespace

// [TCSOLA patch] the boss fight song caption (src/game/hooks.cpp)
void TcWindow(void* hwnd);
void TcUi(double fps);
// [TCSOLA patch] the setup window: the game, PC content and Makoto (src/common/tc_setup.cpp)
void TcSetup();
extern bool g_tc_setup_done;

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"True Crime: Streets of LA",               // window and splash
    .short_title = L"True Crime",               // message boxes
    .name = L"TrueCrimeLA",                      // window classes, crash dumps, handover event
    .log = "truecrime",                        // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put True Crime: Streets of LA (Xbox) in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "TrueCrimeLA"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"setup_done", &g_tc_setup_done, nullptr}},  // the setup window has been through once (it still opens at every start)
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = TcSetup,                      // the setup window, at every start of the release (tc_setup.cpp)
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = TcWindow,                   // the game window is up: the song caption's font
    .message = nullptr,                   // window messages first; true takes one
    .ui = TcUi,                           // ImGui drawing each frame: the song caption
    .pad = TcPad,                         // port 1 always connected, blips held 3 s (see TcPad)
    .rumble = nullptr,                    // changes a rumble before it is set
};
