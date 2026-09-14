// --- src/radar.cpp ---
#include "radar.h"

#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

std::atomic<int>  g_port{3000};
std::atomic<bool> g_opened{false}, g_failed{false};
std::atomic<bool> g_started{false};

char g_ipv4[64] = "";

// Pick this machine's LAN IPv4: a non-loopback, non-APIPA IPv4 address. This is
// the address another device on the same network can reach.
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
        for (PIP_ADAPTER_ADDRESSES a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS ua = a->FirstUnicastAddress;
                 ua; ua = ua->Next) {
                if (!ua->Address.lpSockaddr) continue;
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;

                const auto* in4 =
                    (sockaddr_in*)ua->Address.lpSockaddr;
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
            if (ok) break;
        }
    }

    HeapFree(GetProcessHeap(), 0, addrs);
    return ok;
}

void refresh() {
    char ip[64] = "";
    if (find_lan_ipv4(ip, sizeof(ip))) {
        std::snprintf(g_ipv4, sizeof(g_ipv4), "%s", ip);
    }
}

void open_url(const char* url) {
    HINSTANCE r = ShellExecuteA(nullptr, "open", url, nullptr, nullptr,
                                SW_SHOWNORMAL);
    const bool ok = ((INT_PTR)r > 32);
    g_opened.store(ok);
    g_failed.store(!ok);
}

} // namespace

void Radar_Init() {
    if (g_started.exchange(true)) return;
    // ShellExecuteA needs no explicit COM init; the resolver needs nothing
    // either, so there is no background thread to own here.
    refresh();
}

void Radar_Shutdown() {
    g_started.store(false);
}

RadarInfo Radar_Get() {
    RadarInfo info;
    std::snprintf(info.ipv4, sizeof(info.ipv4), "%s", g_ipv4);

    const int port = g_port.load();
    info.port = port;
    if (g_ipv4[0])
        std::snprintf(info.url, sizeof(info.url), "http://%s:%d",
                      g_ipv4, port);
    else
        std::snprintf(info.url, sizeof(info.url), "http://127.0.0.1:%d", port);

    info.opened = g_opened.load();
    info.failed = g_failed.load();
    return info;
}

void Radar_Start() {
    refresh();
    const int port = g_port.load();
    char url[128];
    if (g_ipv4[0])
        std::snprintf(url, sizeof(url), "http://%s:%d", g_ipv4, port);
    else
        std::snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);

    open_url(url);

    // The flash flags are one-shot, cleared on the next read.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }).detach();
}

void Radar_SetPort(int port) {
    if (port < 1 || port > 65535) return;
    g_port.store(port);
}

int Radar_Port() { return g_port.load(); }
