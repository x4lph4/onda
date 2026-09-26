#include "DeviceManager.h"

namespace onda::ctrl {

void DeviceManager::attach(const DeviceInfo& d, double nowSec) {
    auto it = devices_.find(d.id);
    if (it != devices_.end()) {
        Entry& e = it->second;
        const bool wasGone = e.gone;
        e.info = d; e.seenSec = nowSec; e.gone = false;
        if (wasGone) {
            ++reconnects_;
            emit(e.info, Event::Reconnected);
        }
        return;                                  // already present
    }
    devices_.emplace(d.id, Entry{ d, nowSec, false, 0.0 });
    ++connects_;
    emit(d, Event::Connected);
}

void DeviceManager::detach(uint32_t id, double nowSec) {
    auto it = devices_.find(id);
    if (it == devices_.end() || it->second.gone) return;
    it->second.gone = true;
    it->second.goneAtSec = nowSec;
    ++disconnects_;
    emit(it->second.info, Event::Disconnected);
}

const DeviceInfo* DeviceManager::find(uint32_t id) const {
    auto it = devices_.find(id);
    return it != devices_.end() && !it->second.gone
           ? &it->second.info : nullptr;
}

bool DeviceManager::connected(uint32_t id) const {
    return find(id) != nullptr;
}

std::vector<DeviceInfo> DeviceManager::list() const {
    std::vector<DeviceInfo> out;
    out.reserve(devices_.size());
    for (auto& [id, e] : devices_)
        if (!e.gone) out.push_back(e.info);
    return out;
}

} // namespace onda::ctrl
