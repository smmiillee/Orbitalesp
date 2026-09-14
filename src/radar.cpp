// --- src/radar.cpp ---
#include <winsock2.h>      // MUST precede windows.h, or winsock.h conflicts
#include <ws2tcpip.h>
#include <Windows.h>
#include <shellapi.h>      // ShellExecuteA; WIN32_LEAN_AND_MEAN omits this
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
std::atomic<int>    g_sel{0};
std::atomic<double> g_flash_until{0.0};
std::atomic<bool>   g_flash_ok{false};
std::atomic<bool>   g_started{false};

// Cached address list, refreshed on Radar_Get().
char g_addr[RadarInfo::kMaxAddr][64];
char g_label[RadarInfo::kMaxAddr][80];
int  g_count = 0;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

void add_addr(const char* ip, const char* why) {
    if (g_count >= RadarInfo::kMaxAddr) return;
    for (int i = 0; i < g_count; ++i)
        if (std::strcmp(g_addr[i], ip) == 0) return;

    std::snprintf(g_addr[g_count], sizeof(g_addr[0]), "%s", ip);
    std::snprintf(g_label[g_count], sizeof(g_label[0]), "%s  (%s)", ip, why);
    ++g_count;
}

// Every usable IPv4, loopback first. Adapters come back in whatever order the
// OS likes, which is exactly why the old "first one" approach was unreliable.
void enumerate_addrs() {
    g_count = 0;

    add_addr("127.0.0.1", "this PC");

    ULONG size = 15000;
    PIP_ADAPTER_ADDRESSES addrs =
        (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, size);
    if (!addrs) return;

    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;

    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, size);
        if (!addrs) return;
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }

    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;

            // Skip known-virtual adapters so they don't clutter the list.
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (a->IfType == IF_TYPE_TUNNEL) continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS ua = a->FirstUnicastAddress;
                 ua; ua = ua->Next) {
                if (!ua->Address.lpSockaddr) continue;
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;

                const sockaddr_in* in4 =
                    (const sockaddr_in*)ua->Address.lpSockaddr;
                char buf[INET_ADDRSTRLEN] = "";
                if (!inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof(buf)))
                    continue;

                if (std::strncmp(buf, "169.254.", 8) == 0) continue;  // APIPA

                char why[48] = "adapter";
                if (a->FriendlyName[0]) {
                    char narrow[40] = "";
                    WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1,
                                        narrow, sizeof(narrow), nullptr, nullptr);
                    std::snprintf(why, sizeof(why), "%s",
                                  narrow[0] ? narrow : "adapter");
                }
                add_addr(buf, why);
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, addrs);
}

void build_url(char* out, size_t cap) {
    const int sel = g_sel.load();
    const char* ip = (sel >= 0 && sel < g_count) ? g_addr[sel] : "127.0.0.1";
    std::snprintf(out, cap, "http://%s:%d", ip, g_port.load());
}

} // namespace

void Radar_Init() {
    if (g_started.exchange(true)) return;
    enumerate_addrs();
}

void Radar_Shutdown() {
    g_started.store(false);
}

RadarInfo Radar_Get() {
    enumerate_addrs();

    RadarInfo info;
    info.count = g_count;
    for (int i = 0; i < g_count; ++i) {
        std::snprintf(info.addr[i],  sizeof(info.addr[i]),  "%s", g_addr[i]);
        std::snprintf(info.label[i], sizeof(info.label[i]), "%s", g_label[i]);
    }

    int sel = g_sel.load();
    if (sel < 0 || sel >= g_count) { sel = 0; g_sel.store(0); }
    info.selected = sel;
    info.port = g_port.load();

    build_url(info.url, sizeof(info.url));

    const bool live = now_ms() < g_flash_until.load();
    info.opened = live && g_flash_ok.load();
    info.failed = live && !g_flash_ok.load();
    return info;
}

void Radar_Start() {
    char url[128];
    build_url(url, sizeof(url));

    HINSTANCE r = ShellExecuteA(nullptr, "open", url, nullptr, nullptr,
                                SW_SHOWNORMAL);
    const bool ok = ((INT_PTR)r > 32);

    g_flash_ok.store(ok);
    g_flash_until.store(now_ms() + 2500.0);
}

void Radar_SetSelection(int index) {
    if (index < 0) index = 0;
    g_sel.store(index);
}
int Radar_Selection() { return g_sel.load(); }

void Radar_SetPort(int port) {
    if (port < 1 || port > 65535) return;
    g_port.store(port);
}
int Radar_Port() { return g_port.load(); }
