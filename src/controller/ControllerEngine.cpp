#include "ControllerEngine.h"

#include <chrono>

#include "ControlRegistry.h"

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

    // generic surfaces share the same outbound ports (§20-29)
    jogWheel_.setPorts({
        [this](int deck, int mode, double delta) {
            if (ports_.jog) ports_.jog(deck, mode, delta);
        },
        [this](int dir) {
            if (ports_.sendCommand)
                ports_.sendCommand(dir > 0 ? 0x3001 /*BrowserNextTrack*/
                                         : 0x3002 /*BrowserPrevTrack*/,
                                   1.0f);
        }});
    pads_.setPorts({
        [this](uint32_t cmd, float v) {
            if (ports_.sendCommand) ports_.sendCommand(cmd, v);
        }});
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
    st->lifecycle = DeviceLifecycle::Detected;
    st->lastError.clear();
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
    if (st) {
        st->connected = false;
        st->lifecycle = DeviceLifecycle::Disconnecting;
    }
    devMgr_.detach(deviceId, nowSec);
}

void ControllerEngine::onDeviceEvent(const DeviceInfo& d,
                                     DeviceManager::Event e) {
    using Ev = DeviceManager::Event;
    auto* st = device(d.id);
    if (e == Ev::Connected || e == Ev::Reconnected) {
        if (st) {
            st->connected = true;
            st->lifecycle = DeviceLifecycle::Connecting;
        }
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
                    st->lifecycle = DeviceLifecycle::Ready;
                } else {
                    st->lifecycle = DeviceLifecycle::Error;
                    st->lastError = perr;
                    if (ports_.notify)
                        ports_.notify("Profile error", perr, 2);
                }
            }
        } else if (st) {
            // no profile matched — transport open, unmapped device
            st->lifecycle = DeviceLifecycle::Connected;
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
        if (st) st->lifecycle = DeviceLifecycle::Disconnected;
        if (ports_.notify)
            ports_.notify("Controller disconnected", d.name, 2);
    }
    if (onDevice) onDevice(d, (int)e);
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

    // device plugin seam — modeful hardware semantics consume first
    if (preDispatch && preDispatch(deviceId, m)) {
        ++st->actedCount; ++totalActed_;
        diag_.logIn(deviceId, m, nowSec_, true);
        return;
    }

    const bool acted = st->mapping.process(m, nowSec_);
    if (acted) { ++st->actedCount; ++totalActed_; }
    // §50: decode which control produced the action (monitor column)
    diag_.logIn(deviceId, m, nowSec_, acted,
                acted ? decodeControl(*st, addrOf(m)) : "");
    diag_.noteLatency(st->mapping.lastProcessUs());
}

void ControllerEngine::processSysex(uint32_t deviceId,
                                    const SysExMsg& sx) {
    DeviceState* st = device(deviceId);
    if (!st || !st->connected) return;
    ++st->inCount; ++totalIn_;
    bool consumed = false;
    if (onSysex) consumed = onSysex(deviceId, sx);
    diag_.logSysex(deviceId, sx, nowSec_, consumed);
    if (consumed) { ++st->actedCount; ++totalActed_; }
}

void ControllerEngine::setLayer(uint32_t deviceId, uint8_t layer) {
    DeviceState* st = device(deviceId);
    if (st) st->mapping.setLayer(layer);
}
uint8_t ControllerEngine::layer(uint32_t deviceId) const {
    const DeviceState* st = device(deviceId);
    return st ? st->mapping.layer() : 0;
}

ControllerState ControllerEngine::state(uint32_t deviceId) const {
    ControllerState s;
    const DeviceState* st = device(deviceId);
    if (!st) return s;
    s.id             = st->info.id;
    s.name           = st->info.name;
    s.manufacturer   = st->info.manufacturer;
    s.model          = st->info.model;
    s.transport      = st->info.transport;
    s.connected      = st->connected;
    s.lifecycle      = st->lifecycle;
    s.initialized    = !st->profileId.empty() &&
                       st->lifecycle == DeviceLifecycle::Ready;
    s.profileId      = st->profileId;
    s.activeLayer    = st->mapping.layer();
    s.mappingCount   = (int)st->mapping.mappings().size();
    s.feedbackWatches= (int)st->feedback.watches().size();
    s.inCount        = st->inCount;
    s.actedCount     = st->actedCount;
    s.lastError      = st->lastError;
    return s;
}

/// §50 monitor decode: which logical/mapped control fired.
std::string ControllerEngine::decodeControl(const DeviceState& st,
                                            const MidiAddr& a) const {
    const int idx = st.mapping.find(a);
    if (idx < 0) return {};
    const Mapping& m = st.mapping.mappings()[(size_t)idx];
    if (!m.target.control.empty()) return m.target.control;
    switch (m.target.kind) {
        case Target::Kind::Command:
            return !m.target.cmdName.empty()
                   ? m.target.cmdName
                   : "cmd#" + std::to_string(m.target.cmd);
        case Target::Kind::Param:
            return "param#" + std::to_string(m.target.paramId);
        case Target::Kind::Jog:
            return "jog.deck" + std::to_string(m.target.jogDeck);
        default:
            return {};
    }
}

void ControllerEngine::tick(double nowSec) {
    nowSec_ = nowSec;
    for (auto& [id, st] : states_) {
        if (!st->connected) continue;
        // §17 long-press housekeeping rides the same UI tick
        if (st->mapping.poll(nowSec) > 0) {
            ++st->actedCount; ++totalActed_;
        }
        st->feedback.tick(nowSec);
    }
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
    if ((err = p.validate()), !err.empty()) {
        st->lifecycle = DeviceLifecycle::Error;
        st->lastError = err;
        return false;
    }

    std::vector<Mapping> maps = p.mappings;
    std::vector<FeedbackEngine::Watch> watches;
    watches.reserve(maps.size());

    for (Mapping& m : maps) {
        // §10 logical control ids resolve first — "decka.play" →
        // the canonical Target (command/param/jog) from the registry.
        if (!m.target.control.empty()) {
            const LogicalControl* lc =
                controlRegistry().find(m.target.control);
            if (!lc) {
                err = "unknown control '" + m.target.control + "'";
                return false;
            }
            const std::string keepName = m.target.cmdName;
            m.target = lc->target;
            m.target.control = lc->id;
            if (!keepName.empty() && m.target.cmdName.empty())
                m.target.cmdName = keepName;
        }
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
    st->lifecycle = DeviceLifecycle::Ready;
    st->lastError.clear();
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
