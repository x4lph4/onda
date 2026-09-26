#include "ControllerEngine.h"

#include <chrono>

namespace onda::ctrl {

static int64_t steadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ControllerEngine::ControllerEngine(std::string profileDir)
    : profiles_(std::move(profileDir)) {
    devMgr_.addListener(
        [this](const DeviceInfo& d, DeviceManager::Event e) {
            onDeviceEvent(d, e);
        });
    ports_.notify = [](const std::string&, const std::string&, int) {};
}

void ControllerEngine::setPorts(Ports p) {
    ports_ = std::move(p);
    if (!ports_.notify)
        ports_.notify = [](const std::string&, const std::string&, int) {};
    if (!ports_.commandState)
        ports_.commandState = [](uint32_t) { return -1; };
    if (!ports_.paramState)
        ports_.paramState = [](uint16_t) { return 0.0f; };
    // propagate to every existing device
    for (auto& [id, st] : states_) {
        st->mapping.setPorts({
            ports_.sendCommand, ports_.setParam, ports_.jog,
            ports_.commandState, ports_.paramState,
            [this, id](const MidiAddr& a, uint8_t v, int64_t us) {
                auto* s = device(id);
                return s && s->feedback.isEcho(a, v, us);
            }});
        wireFeedbackPorts(*st, id);
    }
}

// ----------------------------------------------------------------------

int ControllerEngine::attachDevice(
    const DeviceInfo& d,
    std::function<void(const MidiMsg&)> send,
    double nowSec) {
    auto st = std::make_unique<DeviceState>();
    st->info = d;
    st->connected = true;
    st->sendRaw = std::move(send);
    st->capabilities = d.capabilities;
    st->mapping.setPorts({
        ports_.sendCommand, ports_.setParam, ports_.jog,
        ports_.commandState, ports_.paramState,
        [this, id = d.id](const MidiAddr& a, uint8_t v, int64_t us) {
            auto* s = device(id);
            return s && s->feedback.isEcho(a, v, us);
        }});
    wireFeedbackPorts(*st, d.id);
    states_[d.id] = std::move(st);
    devMgr_.attach(d, nowSec);
    return (int)d.id;
}

void ControllerEngine::detachDevice(uint32_t deviceId, double nowSec) {
    auto* st = device(deviceId);
    if (st) st->connected = false;
    devMgr_.detach(deviceId, nowSec);
}

void ControllerEngine::onDeviceEvent(const DeviceInfo& d,
                                     DeviceManager::Event e) {
    using Ev = DeviceManager::Event;
    auto* st = device(d.id);
    if (e == Ev::Connected || e == Ev::Reconnected) {
        if (st) st->connected = true;
        // auto-assign a matching profile
        std::string err;
        const std::string pid =
            profiles_.profileIdFor(d.id).empty()
                ? profiles_.autoMatch(d.name, err)
                : profiles_.profileIdFor(d.id);
        if (!pid.empty()) {
            std::string aerr;
            profiles_.assignProfile(d.id, pid, aerr);
            const ControllerProfile* p = profiles_.profileFor(d.id);
            if (p && st) {
                std::string perr;
                if (applyProfile(d.id, *p, perr)) {
                    st->profileId = pid;
                    st->capabilities |= p->capabilities;
                } else if (ports_.notify) {
                    ports_.notify("Profile error",
                                  perr, 2);
                }
            }
        }
        if (e == Ev::Reconnected) {
            restoreFeedback(d.id);
            if (ports_.notify)
                ports_.notify("Controller reconnected", d.name, 1);
        } else if (ports_.notify) {
            ports_.notify("Controller connected",
                          d.name + (st && !st->profileId.empty()
                                    ? " — " + st->profileId : ""), 0);
        }
    } else if (e == Ev::Disconnected) {
        if (ports_.notify)
            ports_.notify("Controller disconnected", d.name, 2);
    }
}

// ----------------------------------------------------------------------

void ControllerEngine::wireFeedbackPorts(DeviceState& st, uint32_t id) {
    FeedbackEngine::Ports fp;
    fp.send = [this, id](const MidiAddr& a, uint8_t v) {
        auto* s = device(id);
        if (s && s->sendRaw) {
            MidiMsg m = makeOut(a, v);
            m.timeUs = steadyUs();
            diag_.logOut(id, m, nowSec_);
            s->sendRaw(m);
        }
    };
    fp.commandState = ports_.commandState;
    fp.paramState   = ports_.paramState;
    st.feedback.setPorts(std::move(fp));
}

void ControllerEngine::processInput(uint32_t deviceId,
                                    const MidiMsg& msgIn) {
    MidiMsg m = msgIn;
    if (m.timeUs == 0) m.timeUs = steadyUs();
    ++totalIn_;

    // realtime clock family — decoupled, never mapped
    if (m.isRealtime()) { clock_.ingest(m); return; }

    DeviceState* st = device(deviceId);
    if (!st || !st->connected) return;
    ++st->inCount;

    // Learn Mode capture eats the message
    if (learn_.armed() && learnDevice_ == deviceId) {
        Mapping captured;
        if (learn_.capture(m, captured)) {
            const int idx = st->mapping.addMapping(captured);
            if (onLearnedMapping) onLearnedMapping(deviceId, captured);
            if (ports_.notify)
                ports_.notify("Mapping learned",
                    addrKindName(captured.in.kind) +
                    std::string(" ch") + std::to_string(captured.in.channel) +
                    " #" + std::to_string(captured.in.number) +
                    " → slot " + std::to_string(idx), 0);
            diag_.logIn(deviceId, m, nowSec_, true);
            ++st->actedCount; ++totalActed_;
            return;
        }
    }

    const bool acted = st->mapping.process(m, nowSec_);
    if (acted) { ++st->actedCount; ++totalActed_; }
    diag_.logIn(deviceId, m, nowSec_, acted);
    diag_.noteLatency(st->mapping.lastProcessUs());
}

void ControllerEngine::tick(double nowSec) {
    nowSec_ = nowSec;
    for (auto& [id, st] : states_)
        if (st->connected) st->feedback.tick(nowSec);
}

// ----------------------------------------------------------------------

void ControllerEngine::restoreFeedback(uint32_t deviceId) {
    auto* st = device(deviceId);
    if (st) st->feedback.tick(nowSec_, /*force*/ true);
}

bool ControllerEngine::applyProfile(uint32_t deviceId,
                                    const ControllerProfile& p,
                                    std::string& err) {
    DeviceState* st = device(deviceId);
    if (!st) { err = "no such device"; return false; }
    if ((err = p.validate()), !err.empty()) return false;

    std::vector<Mapping> maps = p.mappings;
    std::vector<FeedbackEngine::Watch> watches;
    watches.reserve(maps.size());

    for (Mapping& m : maps) {
        // resolve named commands → ids via CommandManager
        if (m.target.kind == Target::Kind::Command &&
            m.target.cmd == 0 && !m.target.cmdName.empty() &&
            ports_.commandIdFor) {
            m.target.cmd = ports_.commandIdFor(m.target.cmdName);
            if (m.target.cmd == 0xFFFFFFFFu) {
                err = "unknown command '" + m.target.cmdName + "'";
                return false;
            }
        }
        if (m.out.hasCustom || m.out.addr.pack() != 0) {
            FeedbackEngine::Watch w;
            w.out = m.out.addr;
            w.onValue = m.out.onValue;
            w.offValue = m.out.offValue;
            w.followValue = m.out.followValue;
            w.srcMin = m.outMin; w.srcMax = m.outMax;
            if (m.target.kind == Target::Kind::Command)
                w.cmd = m.target.cmd;
            else if (m.target.kind == Target::Kind::Param)
                w.paramId = m.target.paramId;
            else continue;
            watches.push_back(w);
        }
    }

    st->mapping.setMappings(std::move(maps));
    st->feedback.setWatches(std::move(watches));
    st->profileId = profiles_.idFor(p);
    st->capabilities |= p.capabilities;
    return true;
}

// ----------------------------------------------------------------------

DeviceState* ControllerEngine::device(uint32_t id) {
    auto it = states_.find(id);
    return it == states_.end() ? nullptr : it->second.get();
}
const DeviceState* ControllerEngine::device(uint32_t id) const {
    auto it = states_.find(id);
    return it == states_.end() ? nullptr : it->second.get();
}
std::vector<uint32_t> ControllerEngine::deviceIds() const {
    std::vector<uint32_t> out;
    for (auto& [id, st] : states_) out.push_back(id);
    return out;
}

} // namespace onda::ctrl
