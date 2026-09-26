#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "core/adevice/AudioDeviceTypes.h"

namespace onda::adevice {

/// Backend-independent view of the raw device list (§3). The platform
/// backend hands us RawDevice records; the catalog assigns stable ids,
/// flags defaults, and answers lookups.
struct RawDevice {
    std::string backend;   // "WASAPI", "CoreAudio", "ASIO", ...
    std::string name;
    std::string manufacturer;
    int inCh = 0, outCh = 0;
    bool isDefaultOut = false, isDefaultIn = false;
};

struct DeviceCatalog {
    static std::vector<DeviceInfo> normalize(
            const std::vector<RawDevice>& raw) {
        std::vector<DeviceInfo> out;
        out.reserve(raw.size());
        for (const auto& r : raw) {
            DeviceInfo d;
            d.backend = r.backend;
            d.name = r.name;
            d.id = DeviceInfo::makeId(r.backend, r.name);
            d.manufacturer = r.manufacturer;
            d.inCh = r.inCh; d.outCh = r.outCh;
            d.isDefaultOut = r.isDefaultOut;
            d.isDefaultIn = r.isDefaultIn;
            out.push_back(std::move(d));
        }
        return out;
    }

    static const DeviceInfo* byId(const std::vector<DeviceInfo>& v,
                                  const std::string& id) {
        const auto it = std::find_if(v.begin(), v.end(),
            [&](const DeviceInfo& d) { return d.id == id; });
        return it == v.end() ? nullptr : &*it;
    }
    static const DeviceInfo* byName(
            const std::vector<DeviceInfo>& v,
            const std::string& backend, const std::string& name) {
        return byId(v, DeviceInfo::makeId(backend, name));
    }
    static const DeviceInfo* defaultOut(
            const std::vector<DeviceInfo>& v) {
        const auto it = std::find_if(v.begin(), v.end(),
            [](const DeviceInfo& d) {
                return d.isDefaultOut && d.outCh > 0; });
        return it == v.end() ? nullptr : &*it;
    }
    static const DeviceInfo* defaultIn(
            const std::vector<DeviceInfo>& v) {
        const auto it = std::find_if(v.begin(), v.end(),
            [](const DeviceInfo& d) {
                return d.isDefaultIn && d.inCh > 0; });
        return it == v.end() ? nullptr : &*it;
    }

    /// Name-based recogniser for the FLX4 endpoint (documented in the
    /// controller profile): "ddj-flx4" / "ddj flx4" anywhere in the
    /// name. Detection, never an assumption of channels or indices.
    static bool isFlx4(const DeviceInfo& d) {
        std::string n = d.name;
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        return n.find("ddj-flx4") != std::string::npos
            || n.find("ddj flx4") != std::string::npos;
    }
    static const DeviceInfo* flx4(const std::vector<DeviceInfo>& v) {
        const auto it = std::find_if(v.begin(), v.end(), isFlx4);
        return it == v.end() ? nullptr : &*it;
    }
};

} // namespace onda::adevice
