#pragma once

#include <cstdio>
#include <cstdlib>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// DeviceProfile JSON — single-level object, escaped strings.
/// Keys: id, rate, buffer, pref, mL, mR, cL, cR, iL, iR.
inline std::string DeviceProfile::toJson() const {
    char b[512];
    std::snprintf(b, sizeof b,
        "{\"id\":\"%s\",\"rate\":%.0f,\"buffer\":%d,\"pref\":%d,"
        "\"mL\":%d,\"mR\":%d,\"cL\":%d,\"cR\":%d,\"iL\":%d,\"iR\":%d}",
        deviceId.c_str(), sampleRate, bufferSize, (int)pref,
        map.masterL, map.masterR, map.cueL, map.cueR,
        map.inL, map.inR);
    return b;
}
inline bool DeviceProfile::fromJson(const std::string& j,
                                    DeviceProfile& p) {
    auto num = [&](const char* k, double& v) {
        const std::string key = std::string("\"") + k + "\":";
        auto i = j.find(key); if (i == std::string::npos) return false;
        v = std::strtod(j.c_str() + i + key.size(), nullptr);
        return true;
    };
    auto str = [&](const char* k, std::string& v) {
        const std::string key = std::string("\"") + k + "\":\"";
        auto i = j.find(key); if (i == std::string::npos) return false;
        const auto e = j.find('"', i + key.size());
        if (e == std::string::npos) return false;
        v = j.substr(i + key.size(), e - i - key.size());
        return true;
    };
    double v = 0; int iv = 0; std::string s;
    if (!str("id", s)) return false;
    p.deviceId = s;
    if (num("rate", v))   p.sampleRate = v;
    if (num("buffer", v)) p.bufferSize = (int)v;
    if (num("pref", v))   p.pref = (LatencyPref)(int)v;
    if (num("mL", v)) p.map.masterL = (int)v;
    if (num("mR", v)) p.map.masterR = (int)v;
    if (num("cL", v)) p.map.cueL = (int)v;
    if (num("cR", v)) p.map.cueR = (int)v;
    if (num("iL", v)) p.map.inL = (int)v;
    if (num("iR", v)) p.map.inR = (int)v;
    (void)iv;
    return true;
}

} // namespace onda::adevice
