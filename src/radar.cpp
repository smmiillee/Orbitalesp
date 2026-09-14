// --- src/radar.cpp ---
// Radar launcher.
//
// The radar is a separate app (orbitalweb) that runs its own local server on
// port 3000. This does NOT embed it -- it finds this machine's LAN IPv4 and
// opens a browser at http://<ip>:3000 so the radar is reachable from another
// device on the same network.
//
// No injection, no memory access: this is a convenience button.

// *** HEADER ORDER MATTERS ***
// winsock2.h MUST come before windows.h, otherwise windows.h pulls in the old
// winsock.h and the two conflict. And shellapi.h must be included EXPLICITLY:
// the build defines WIN32_LEAN_AND_MEAN, which makes windows.h skip it, so
// ShellExecuteA would be undeclared.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <shellapi.h>
#include <iphlpapi.h>

#include "radar.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

std::atomic<int>    g_port{3000};
std::atomic<double> g_flash_until{0.0};   // ms timestamp
std::atomic<bool>   g_flash_ok{false};
std::atomic<bool>   g_started{false};

char g_ipv4[64] = "";

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

// This machine's LAN IPv4: a non-loopback, non-APIPA address. This is what
// another device on the same network can reach.
bool find_lan_ipv4(char* out, size_t cap) {
    out[0] = '\0';

    ULONG size = 15000;
    PIP_ADAPTER_ADDRESSES addrs =
        (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, size);
    if (!addrs) return false;

    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;

    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, size);
        if (!addrs) return false;
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }

    bool ok = false;
    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a && !ok; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS ua = a->FirstUnicastAddress;
                 ua; ua = ua->Next) {
                if (!ua->Address.lpSockaddr) continue;
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;

                const sockaddr_in* in4 = (const sockaddr_in*)ua->Address.lpSockaddr;
                char buf[INET_ADDRSTRLEN] = "";
                if (!inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof(buf)))
                    continue;

                // Skip loopback and APIPA (169.254.x.x).
                if (std::strncmp(buf, "127.", 4) == 0) continue;
                if (std::strncmp(buf, "169.254.", 8) == 0) continue;

                std::snprintf(out, cap, "%s", buf);
                ok = true;
                break;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, addrs);
    return ok;
}

void refresh() {
    char ip[64] = "";
    if (find_lan_ipv4(ip, sizeof(ip)))
        std::snprintf(g_ipv4, sizeof(g_ipv4), "%s", ip);
}

void build_url(char* out, size_t cap) {
    const int port = g_port.load();
    if (g_ipv4[0]) std::snprintf(out, cap, "http://%s:%d", g_ipv4, port);
    else           std::snprintf(out, cap, "http://127.0.0.1:%d", port);
}

void open_url(const char* url) {
    HINSTANCE r = ShellExecuteA(nullptr, "open", url, nullptr, nullptr,
                                SW_SHOWNORMAL);
    const bool ok = ((INT_PTR)r > 32);

    // Sticky for 2 seconds rather than one frame, so the message is readable.
    g_flash_ok.store(ok);
    g_flash_until.store(now_ms() + 2000.0);
}

} // namespace

void Radar_Init() {
    if (g_started.exchange(true)) return;
    refresh();
}

void Radar_Shutdown() {
    g_started.store(false);
}

RadarInfo Radar_Get() {
    RadarInfo info;
    std::snprintf(info.ipv4, sizeof(info.ipv4), "%s", g_ipv4);
    info.port = g_port.load();
    build_url(info.url, sizeof(info.url));

    const bool live = now_ms() < g_flash_until.load();
    info.opened = live && g_flash_ok.load();
    info.failed = live && !g_flash_ok.load();
    return info;
}

void Radar_Start() {
    refresh();
    char url[128];
    build_url(url, sizeof(url));
    open_url(url);
}

void Radar_SetPort(int port) {
    if (port < 1 || port > 65535) return;
    g_port.store(port);
}

int Radar_Port() { return g_port.load(); }
