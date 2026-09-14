// --- src/radar.h ---
#pragma once

// Radar launcher.
//
// The radar is a separate app (orbitalweb) that runs its own local server on
// port 3000. This does NOT embed it -- it finds this machine's LAN IPv4 and
// opens a browser at http://<ip>:3000 so the radar is reachable from another
// device on the same network (a phone, a second PC).
//
// Deliberately no injection and no memory access: this is a convenience button.
struct RadarInfo {
    char ipv4[64] = "";     // best LAN IPv4, empty if none found
    char url[128] = "";     // http://<ipv4>:3000
    int  port = 3000;
    bool opened = false;    // set for one frame after a successful open
    bool failed = false;    // set for one frame if it could not open
};

void Radar_Init();
void Radar_Shutdown();

RadarInfo Radar_Get();

// Opens the radar URL in the default browser.
void Radar_Start();

void Radar_SetPort(int port);
int  Radar_Port();
