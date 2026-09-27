#include "MappingEngine.h"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace onda::ctrl {

int encoderDelta(EncoderMode mode, uint8_t v) {
    switch (mode) {
        case EncoderMode::RelBinaryOffset:
            return (int)v - 64;
        case EncoderMode::RelTwosComplement:
            // 7-bit signed: sign-extend bit6 (0x7F = -1)
            return (int)(int8_t)((v & 0x7F) << 1) >> 1;
        case EncoderMode::RelSignMagnitude: {
            const int mag = v & 0x3F;
            return (v & 0x40) ? -mag : mag;
        }
        case EncoderMode::Absolute:
        default:
            return 0;
    }
}

float applyCurve(Curve c, float x) {
    switch (c) {
        case Curve::Inverse: return 1.0f - x;
        case Curve::Expo:    return x * x;
        case Curve::Log:     return std::sqrt(std::max(0.0f, x));
        case Curve::Linear:
        default:             return x;
    }
}

float scaleValue(const Mapping& m, float raw01) {
    const float x = applyCurve(m.curve, raw01);
    return m.outMin + x * (m.outMax - m.outMin);
}

// ----------------------------------------------------------------------

void MappingEngine::indexAddr(const Mapping& m, int idx) {
    byAddr_[m.in.pack()].push_back(idx);
    if (m.hiRes) {
        // 14-bit pair: LSB rides the CC number at MSB+0x20
        MidiAddr lsb = m.in;
        lsb.number = (uint8_t)(m.in.number + 0x20);
        byAddr_[lsb.pack()].push_back(idx);
    }
}

/// Sort an address bucket: layer-specific entries that match the
/// active layer first is unnecessary (filtering handles that), but
/// priority must order same-layer siblings — higher priority wins.
static void sortBucket(std::vector<int>& v,
                       const std::vector<Mapping>& maps) {
    std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
        return maps[(size_t)a].priority > maps[(size_t)b].priority;
    });
}

void MappingEngine::setMappings(std::vector<Mapping> maps) {
    maps_ = std::move(maps);
    run_.assign(maps_.size(), MappingRuntime{});
    byAddr_.clear();
    for (int i = 0; i < (int)maps_.size(); ++i)
        indexAddr(maps_[(size_t)i], i);
    for (auto& [k, v] : byAddr_) sortBucket(v, maps_);
}

int MappingEngine::addMapping(const Mapping& m) {
    const int idx = (int)maps_.size();
    maps_.push_back(m);
    run_.push_back(MappingRuntime{});
    indexAddr(m, idx);
    sortBucket(byAddr_[m.in.pack()], maps_);
    if (m.hiRes) {
        MidiAddr lsb = m.in;
        lsb.number = (uint8_t)(m.in.number + 0x20);
        sortBucket(byAddr_[lsb.pack()], maps_);
    }
    return idx;
}

bool MappingEngine::removeMapping(int index) {
    if (index < 0 || index >= (int)maps_.size()) return false;
    // mark disabled — cheap; full compaction happens on setMappings
    maps_[(size_t)index].enabled = false;
    run_[(size_t)index] = MappingRuntime{};
    maps_[(size_t)index].target = Target{};
    return true;
}

bool MappingEngine::setMappingEnabled(int index, bool on) {
    if (index < 0 || index >= (int)maps_.size()) return false;
    maps_[(size_t)index].enabled = on;
    if (!on) run_[(size_t)index] = MappingRuntime{};
    return true;
}

void MappingEngine::clear() {
    maps_.clear(); run_.clear(); byAddr_.clear();
}

int MappingEngine::find(const MidiAddr& a) const {
    auto it = byAddr_.find(a.pack());
    return it == byAddr_.end() || it->second.empty()
           ? -1 : it->second.front();
}

void MappingEngine::noteTargetValue(const MidiAddr& in, float v) {
    auto it = byAddr_.find(in.pack());
    if (it == byAddr_.end()) return;
    for (int i : it->second) run_[(size_t)i].current = v;
}

bool MappingEngine::process(const MidiMsg& msg, double nowSec) {
    const auto t0 = std::chrono::steady_clock::now();
    bool acted = false;

    const MidiAddr addr = addrOf(msg);
    // echo suppression — drop inputs our own feedback caused
    if (ports_.isEcho && ports_.isEcho(addr, msg.data2, msg.timeUs)) {
        ++dropped_;
        lastUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        processUs_ += lastUs_;
        return false;
    }

    auto it = byAddr_.find(addr.pack());
    if (it != byAddr_.end()) {
        // ---- layer resolution (§18-19): if any binding on the
        // active layer owns this input, Default-layer siblings are
        // shadowed for this message. --------------------------
        bool layerHit = false;
        if (activeLayer_ != 0)
            for (int i : it->second)
                if (maps_[(size_t)i].enabled &&
                    maps_[(size_t)i].layer == activeLayer_) {
                    layerHit = true; break;
                }

        // edge classification
        bool press = false, release = false;
        float raw01 = msgValue01(msg);
        if (addr.kind == AddrKind::Note) {
            if (msg.type == MsgType::NoteOn && msg.data2 > 0) press = true;
            else release = true; // note-off or vel-0
        } else if (addr.kind == AddrKind::CC &&
                   (it->second.size() == 1 &&
                    (maps_[(size_t)it->second[0]].mode == MappingMode::Toggle ||
                     maps_[(size_t)it->second[0]].mode == MappingMode::Momentary))) {
            // CC acting as a button: >63 = press edge
            const bool hi = msg.data2 > 63;
            auto& r = run_[(size_t)it->second[0]];
            if (hi && !r.pressed)  press = true;
            if (!hi && r.pressed)  release = true;
            r.pressed = hi;
        }
        for (int i : it->second) {
            const uint8_t ml = maps_[(size_t)i].layer;
            if (ml != 0 && ml != activeLayer_) continue;   // other layer
            if (ml == 0 && layerHit)         continue;   // shadowed
            acted |= dispatch(i, msg, raw01, press, release, nowSec);
        }
    }

    if (acted) ++processed_; else ++dropped_;
    lastUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    processUs_ += lastUs_;
    return acted;
}

bool MappingEngine::dispatch(int idx, const MidiMsg& msg, float raw01,
                             bool press, bool release, double nowSec) {
    Mapping& m = maps_[(size_t)idx];
    if (!m.enabled || m.target.kind == Target::Kind::None) return false;
    MappingRuntime& r = run_[(size_t)idx];

    if (m.hiRes) {
        // 14-bit merge: cache whichever half arrived, then combine
        if (msg.data1 == (uint8_t)(m.in.number + 0x20)) r.hiLsb = msg.data2;
        else                                        r.hiMsb = msg.data2;
        r.hiSeen = true;
        raw01 = (float)(((uint16_t)r.hiMsb << 7) | r.hiLsb) / 16383.0f;
    }

    const bool continuous =
        (m.in.kind == AddrKind::CC ||
         m.in.kind == AddrKind::PitchBend ||
         m.in.kind == AddrKind::ChannelPressure) &&
        m.mode != MappingMode::Toggle && m.mode != MappingMode::Momentary;
    const bool edge = press || release;

    switch (m.mode) {
        case MappingMode::Toggle:
            if (!press) return false;
            if (m.target.kind == Target::Kind::Command && ports_.sendCommand) {
                // flip from current state if known; else push 1
                const int cur = ports_.commandState
                                ? ports_.commandState(m.target.cmd) : -1;
                ports_.sendCommand(m.target.cmd,
                                   cur == 1 ? 0.0f : 1.0f);
                return true;
            }
            if (m.target.kind == Target::Kind::Param && ports_.setParam) {
                const float cur = ports_.paramState
                                  ? ports_.paramState(m.target.paramId) : 0.0f;
                ports_.setParam(m.target.paramId,
                                cur > (m.outMin + m.outMax) * 0.5f
                                    ? m.outMin : m.outMax);
                return true;
            }
            return false;

        case MappingMode::Momentary:
            if (!edge) return false;
            // long-press bookkeeping (§17): press stamps the hold
            // time; release clears it (and releases longCmd if fired)
            if (m.longMs > 0 && m.target.kind == Target::Kind::Command) {
                if (press) { r.pressAtSec = nowSec; r.longFired = false; }
                else {
                    if (r.longFired && ports_.sendCommand)
                        ports_.sendCommand(m.longCmd, 0.0f);
                    r.pressAtSec = -1.0; r.longFired = false;
                }
            }
            if (m.target.kind == Target::Kind::Command && ports_.sendCommand) {
                ports_.sendCommand(m.target.cmd, press ? 1.0f : 0.0f);
                return true;
            }
            if (m.target.kind == Target::Kind::Param && ports_.setParam) {
                ports_.setParam(m.target.paramId,
                                press ? m.outMax : m.outMin);
                return true;
            }
            return false;

        case MappingMode::Absolute:
        case MappingMode::Pickup:
            if (m.mode == MappingMode::Pickup && r.armed) {
                // swallow until raw crosses the *live* target value —
                // prefer the paramState port, else last known current
                const float t =
                    m.target.kind == Target::Kind::Param && ports_.paramState
                        ? ports_.paramState(m.target.paramId)
                        : r.current;
                const float tol = (m.outMax - m.outMin) * 0.02f + 0.005f;
                const float v = scaleValue(m, raw01);
                const float prev = r.lastRaw;
                r.lastRaw = v;
                bool crossed = std::fabs(v - t) < tol;
                if (prev >= 0.0f && !crossed)
                    crossed = (prev < t && v >= t) ||
                              (prev > t && v <= t);
                if (!crossed) return true; // consumed, silently
                r.armed = false;
            }
            if (m.target.kind == Target::Kind::Param && ports_.setParam) {
                const float v = scaleValue(m, raw01);
                if (v == r.lastOut) return true;   // no spam
                r.lastOut = v; r.current = v;
                ports_.setParam(m.target.paramId, v);
                return true;
            }
            if (m.target.kind == Target::Kind::Command && ports_.sendCommand) {
                const float v = scaleValue(m, raw01);
                if (v == r.lastOut) return true;
                r.lastOut = v;
                ports_.sendCommand(m.target.cmd, v);
                return true;
            }
            return false;

        case MappingMode::Relative: {
            const int d = decodeDelta(m, msg.data2);
            if (d == 0) return true;   // consumed (dead zone)
            if (m.target.kind == Target::Kind::Jog && ports_.jog) {
                ports_.jog(m.target.jogDeck, m.target.jogMode,
                           (double)d * m.step);
                return true;
            }
            if (m.target.kind == Target::Kind::Param && ports_.setParam) {
                const float cur = ports_.paramState
                                  ? ports_.paramState(m.target.paramId)
                                  : r.current;
                const float nv = std::clamp(cur + d * m.step,
                                            m.outMin, m.outMax);
                r.current = nv;
                ports_.setParam(m.target.paramId, nv);
                return true;
            }
            if (m.target.kind == Target::Kind::Command && ports_.sendCommand) {
                ports_.sendCommand(m.target.cmd, (float)d);
                return true;
            }
            return false;
        }

        case MappingMode::IncDec: {
            const int d = decodeDelta(m, msg.data2);
            if (d == 0) return true;
            if (m.target.kind == Target::Kind::Command && ports_.sendCommand) {
                // one invoke per tick sign — command decides meaning
                for (int k = 0; k < (d > 0 ? d : -d); ++k)
                    ports_.sendCommand(m.target.cmd, d > 0 ? 1.0f : -1.0f);
                return true;
            }
            if (m.target.kind == Target::Kind::Param && ports_.setParam) {
                const float cur = ports_.paramState
                                  ? ports_.paramState(m.target.paramId)
                                  : r.current;
                const float nv = std::clamp(cur + d * m.step,
                                            m.outMin, m.outMax);
                r.current = nv;
                ports_.setParam(m.target.paramId, nv);
                return true;
            }
            return false;
        }
    }
    return false;
}

int MappingEngine::decodeDelta(const Mapping& m, uint8_t v) const {
    return encoderDelta(m.encoder, v);
}

int MappingEngine::poll(double nowSec) {
    int fired = 0;
    for (size_t i = 0; i < maps_.size(); ++i) {
        const Mapping& m = maps_[i];
        MappingRuntime& r = run_[i];
        if (m.longMs == 0 || r.pressAtSec < 0.0 || r.longFired ||
            !m.enabled || m.longCmd == 0) continue;
        // a held binding on a non-active layer never fires
        if (m.layer != 0 && m.layer != activeLayer_) continue;
        if ((nowSec - r.pressAtSec) * 1000.0 < (double)m.longMs) continue;
        r.longFired = true;
        if (ports_.sendCommand) ports_.sendCommand(m.longCmd, 1.0f);
        ++fired;
    }
    return fired;
}

std::vector<std::string> MappingEngine::findIssues(
    const std::vector<Mapping>& maps) {
    std::vector<std::string> out;
    std::unordered_map<uint32_t, std::vector<int>> byAddr;
    for (int i = 0; i < (int)maps.size(); ++i) {
        const Mapping& m = maps[(size_t)i];
        if (!m.enabled) continue;
        byAddr[m.in.pack()].push_back(i);

        if (m.in.channel > 15)
            out.push_back("mapping " + std::to_string(i) +
                          ": channel > 15");
        if (m.hiRes && m.in.kind != AddrKind::CC)
            out.push_back("mapping " + std::to_string(i) +
                          ": hiRes requires a CC input");
        if (m.hiRes && m.in.number > 0x5F)
            out.push_back("mapping " + std::to_string(i) +
                          ": hiRes pair exceeds CC space (LSB wraps)");
        if ((m.mode == MappingMode::Relative ||
             m.mode == MappingMode::IncDec) &&
            m.in.kind != AddrKind::CC)
            out.push_back("mapping " + std::to_string(i) +
                          ": encoder modes expect a CC input");
        if (m.encoder != EncoderMode::Absolute &&
            m.mode != MappingMode::Relative &&
            m.mode != MappingMode::IncDec)
            out.push_back("mapping " + std::to_string(i) +
                          ": relative encoding on a non-encoder mode");
        if (m.longMs > 0 && m.longCmd == 0)
            out.push_back("mapping " + std::to_string(i) +
                          ": longMs set but longCmd is empty");
        if (m.layer > 3)
            out.push_back("mapping " + std::to_string(i) +
                          ": layer id > Custom");
        if (m.target.kind == Target::Kind::Param &&
            m.target.paramId == 0xFFFF)
            out.push_back("mapping " + std::to_string(i) +
                          ": invalid param id");
        if (!m.verified)
            out.push_back("mapping " + std::to_string(i) +
                          ": UNVERIFIED protocol entry");
    }

    // duplicate / conflicting bindings on the same layer (§49/§53)
    for (auto& [pack, idxs] : byAddr) {
        std::unordered_map<uint8_t, int> layers;
        for (int i : idxs) {
            const uint8_t l = maps[(size_t)i].layer;
            if (layers.count(l)) {
                const int j = layers[l];
                const Mapping& a = maps[(size_t)j];
                const Mapping& b = maps[(size_t)i];
                const bool sameTarget =
                    a.target.kind == b.target.kind &&
                    a.target.cmd == b.target.cmd &&
                    a.target.paramId == b.target.paramId;
                out.push_back(
                    std::string("conflict: input ch") +
                    std::to_string(maps[(size_t)i].in.channel) + " #" +
                    std::to_string(maps[(size_t)i].in.number) +
                    " bound twice on layer " + std::to_string(l) +
                    (sameTarget ? " (same target — redundant)"
                     : a.priority == b.priority
                        ? " (equal priority — order is declaration)"
                        : " (priority resolves order)"));
            } else layers[l] = i;
        }
    }
    return out;
}

} // namespace onda::ctrl
