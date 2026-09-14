// --- src/radar.h ---
#pragma once
#include <cstdint>

// Radar launcher.
//
// IMPORTANT: the radar is its OWN program -- cs2_dashboard.exe, built from the
// orbitalweb repo. It reads CS2 memory itself and serves a WebSocket dashboard
// on port 3000. NOTHING here embeds it; this file only builds a URL and opens a
// browser.
//
// There is no Node.js server and no package.json in that repo -- it is C++ and
// CMake, so "run server.js" is not a thing. You build cs2_dashboard.exe and run
// it on the PC playing CS2.
//
// ADDRESS SELECTION
// The old version guessed: it took the first up adapter's non-loopback IPv4.
// On any machine with a VPN, WSL, Hyper-V or Docker that is frequently a
// virtual adapter, and nothing can reach the URL it produced.
//
// So instead of guessing we ENUMERATE every usable IPv4 and let you pick, with
// loopback first -- because if you are viewing the radar on the same PC as the
// server, 127.0.0.1 is correct and always works.
struct RadarInfo {
    // Detected addresses. Entry 0 is always 127.0.0.1.
    static constexpr int kMaxAddr = 10;
    char addr[kMaxAddr][64] = {};
    char label[kMaxAddr][80] = {};   // "127.0.0.1  (this PC)" etc.
    int  count = 0;
    int  selected = 0;

    char url[128] = "";
    int  port = 3000;
    bool opened = false;   // browser opened, shown briefly
    bool failed = false;
};

void Radar_Init();
void Radar_Shutdown();

// Re-enumerates adapters on every call, so plugging in a network shows up.
RadarInfo Radar_Get();

void Radar_Start();

void Radar_SetSelection(int index);
int  Radar_Selection();

void Radar_SetPort(int port);
int  Radar_Port();
