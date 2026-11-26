// mt_only/src/network/WifiStationSettings.h
// 108-byte record: [0..3] 'ZWF1' [4] flags(bit0=enabled) [5] ssidLen [6] pskLen
// [7] reserved(0) [8..40] ssid(33) [41..105] psk(65) [106..107] crc16 le.
#ifndef MT_ONLY_NETWORK_WIFISTATIONSETTINGS_H
#define MT_ONLY_NETWORK_WIFISTATIONSETTINGS_H

#include "../Config.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mt {
namespace network {

struct WifiStationSettings {
    static constexpr size_t Size = 108;
#if defined(CONFIG_MT_WIFI_STATION)
    static constexpr size_t NtpSize=33;
#else
    static constexpr size_t NtpSize=0;
#endif
    static constexpr size_t MaxSize=Size+NtpSize;
    uint8_t bytes[MaxSize];
    size_t encodedSize() const {return NtpSize&&*ntpServer()?MaxSize:Size;}
    const char *ntpServer() const {return NtpSize?reinterpret_cast<const char *>(bytes+Size):"";}
    bool same(const WifiStationSettings &other) const {return memcmp(bytes,other.bytes,MaxSize)==0;}
    bool setNtp(const char *value,size_t n) {
        if(!n){if(NtpSize)memset(bytes+Size,0,NtpSize);return true;}
        if(!value||n>=NtpSize||!validHost(value,n))return false;
        memset(bytes+Size,0,NtpSize);memcpy(bytes+Size,value,n);return true;
    }
    static bool decode(const uint8_t *src,size_t n,WifiStationSettings &out) {
        if(!src||(n!=Size&&n!=MaxSize))return false;
        WifiStationSettings v;memcpy(v.bytes,src,n);
        if(!v.valid()||v.encodedSize()!=n)return false;
        out=v;return true;
    }
private:
    static bool validHost(const char *p,size_t n){
        for(size_t i=0;i<n;++i){
            const char c=p[i];
            if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))continue;
            if((c!='.'&&c!='-')||i==0||i+1==n)return false;
            if(c=='.'&&(p[i-1]=='.'||p[i-1]=='-'||p[i+1]=='-'))return false;
        }
        return true;
    }
public:

    WifiStationSettings() {
        memset(bytes, 0, MaxSize);
        writeMagic(bytes);
        seal();
    }

    bool valid() const {
        if (bytes[0] != 'Z' || bytes[1] != 'W' || bytes[2] != 'F' || bytes[3] != '1') return false;
        if (bytes[4] & ~0x01u) return false;
        if (bytes[7] != 0) return false;
        const uint8_t sl = bytes[5];
        const uint8_t pl = bytes[6];
        if (sl > 32u || pl > 64u) return false;
        if (prefixNul(bytes + 8, sl) || bytes[8 + sl] != 0) return false;
        if (prefixNul(bytes + 41, pl) || bytes[41 + pl] != 0) return false;
        if (tailNz(bytes + 9 + sl, 32u - sl)) return false;
        if (tailNz(bytes + 42 + pl, 64u - pl)) return false;
        if(NtpSize){
            const char *server=ntpServer();const size_t n=strnlen(server,NtpSize);
            if(n==NtpSize||!validHost(server,n)||tailNz(bytes+Size+n+1,NtpSize-n-1))return false;
        }
        const uint16_t crc = (uint16_t)(bytes[106] | ((uint16_t)bytes[107] << 8));
        if (crc != Config::checksum(bytes, 106u)) return false;
        if ((bytes[4] & 0x01u) != 0 && sl == 0) return false;
        return true;
    }

    bool enabled() const { return valid() && (bytes[4] & 0x01u) != 0; }
    uint8_t ssidLength() const { return valid() ? bytes[5] : 0; }
    uint8_t pskLength() const { return valid() ? bytes[6] : 0; }

    bool set(bool enabled, const char *ssid, size_t ssidLen, const char *psk, size_t pskLen) {
        if (!valid()) return false;
        if (ssidLen > 32u || pskLen > 64u) return false;
        if (ssidLen > 0 && ssid == nullptr) return false;
        if (pskLen > 0 && psk == nullptr) return false;
        if (prefixNul((const uint8_t *)ssid, ssidLen)) return false;
        if (prefixNul((const uint8_t *)psk, pskLen)) return false;
        if (enabled && ssidLen == 0) return false;
        WifiStationSettings next = *this;
        memset(next.bytes, 0, Size);
        writeMagic(next.bytes);
        next.bytes[4] = enabled ? 0x01u : 0x00u;
        next.bytes[5] = (uint8_t)ssidLen;
        next.bytes[6] = (uint8_t)pskLen;
        if (ssidLen > 0) memcpy(next.bytes + 8, ssid, ssidLen);
        if (pskLen > 0) memcpy(next.bytes + 41, psk, pskLen);
        next.seal();
        if (!next.valid()) return false;
        *this = next;
        return true;
    }

    bool setEnabled(bool enabled) {
        if (!valid()) return false;
        if (enabled && bytes[5] == 0) return false;
        WifiStationSettings next = *this;
        next.bytes[4] = enabled ? 0x01u : 0x00u;
        next.seal();
        if (!next.valid()) return false;
        *this = next;
        return true;
    }

private:
    static void writeMagic(uint8_t *b) { b[0] = 'Z'; b[1] = 'W'; b[2] = 'F'; b[3] = '1'; }
    static bool prefixNul(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i] == 0) return true; return false; }
    static bool tailNz(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i] != 0) return true; return false; }
    void seal() { const uint16_t c = Config::checksum(bytes, 106u); bytes[106] = (uint8_t)(c & 0xFFu); bytes[107] = (uint8_t)(c >> 8); }
};

} // namespace network
} // namespace mt

#endif // MT_ONLY_NETWORK_WIFISTATIONSETTINGS_H
