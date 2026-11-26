// SPDX-License-Identifier: GPL-3.0-only
// Exercise the application's real Wi-Fi polling code at the driver boundary.
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstring>

#define CONFIG_MT_WIFI_STATION 1
#define IS_ENABLED(setting) (setting)
using atomic_t = int;
using atomic_val_t = int;
static atomic_val_t atomic_set(atomic_t *p, atomic_val_t v) {
    const int old = *p;
    *p = v;
    return old;
}
#define NET_AF_INET AF_INET
struct net_in_addr { unsigned char bytes[4]; };
static int net_addr_pton(int family, const char *text, net_in_addr *address) {
    return inet_pton(family, text, address) == 1 ? 0 : -1;
}

#include "../src/network/WifiRuntime.cpp"

static bool timeAttemptFinished;
static const char *stationAddress;
void zc_wifi_station_start_ntp(const char *, const char *, const char *, void (*)(uint32_t)) {}
bool zc_wifi_station_is_connected() { return timeAttemptFinished; }
bool zc_wifi_station_ip(char *out, size_t capacity) {
    if (!stationAddress) return false;
    std::snprintf(out, capacity, "%s", stationAddress);
    return true;
}
int main() {
    using namespace mt::network;
    WifiStationSettings settings;
    assert(settings.set(true, "test", 4, "", 0) && wifiStart(settings, true));
    wifiPoll();
    assert(!wifiStatus().connected && !wifiStatus().mqttReady && wifiStatus().ip == 0 && !wifiStatus().ssid[0]);
    stationAddress = "192.0.2.17";
    wifiPoll();
    const auto lease = wifiStatus();
    const unsigned char expected[] = {192, 0, 2, 17};
    assert(lease.connected && std::memcmp(&lease.ip, expected, sizeof(expected)) == 0);
    assert(std::strcmp(lease.ssid, "test") == 0 && !lease.mqttReady);
    timeAttemptFinished = true;
    wifiPoll();
    assert(wifiStatus().connected && wifiStatus().mqttReady && wifiStatus().ip == lease.ip);
    stationAddress = nullptr;
    wifiPoll();
    assert(!wifiStatus().connected && !wifiStatus().mqttReady && wifiStatus().ip == 0 && !wifiStatus().ssid[0]);
    puts("PASS preferred address is visible during pending SNTP and clears on lease loss");
}
