#include "ControllerDiagnostics.h"

#include <algorithm>
#include <cstdio>

namespace onda::ctrl {

void ControllerDiagnostics::logIn(uint32_t dev, const MidiMsg& m,
                                  double at, bool acted,
                                  std::string decoded) {
    ++in_;
    if (acted) ++acted_;
    if (!monitor_) return;
    Entry e;
    e.in = true; e.msg = m; e.atSec = at; e.device = dev;
    e.mapped = acted; e.decoded = std::move(decoded);
    ring_.push_back(std::move(e));
    if (ring_.size() > kRing) ring_.pop_front();
}

void ControllerDiagnostics::logOut(uint32_t dev, const MidiMsg& m,
                                   double at) {
    ++out_;
    if (!monitor_) return;
    Entry e;
    e.in = false; e.msg = m; e.atSec = at; e.device = dev;
    ring_.push_back(std::move(e));
    if (ring_.size() > kRing) ring_.pop_front();
}

void ControllerDiagnostics::logSysex(uint32_t dev, const SysExMsg& sx,
                                     double at, bool consumed) {
    ++in_;
    if (!monitor_) return;
    Entry e;
    e.in = true; e.atSec = at; e.device = dev;
    e.mapped = consumed; e.sysex = true; e.sx = sx;
    e.msg.type = MsgType::Unknown;
    e.msg.data1 = (uint8_t)sx.len;
    ring_.push_back(std::move(e));
    if (ring_.size() > kRing) ring_.pop_front();
}

void ControllerDiagnostics::noteLatency(int64_t us) {
    sumUs_ += us; maxUs_ = std::max(maxUs_, us); ++n_;
}

std::string ControllerDiagnostics::describeMsg(const MidiMsg& m) {
    char b[64];
    switch (m.type) {
        case MsgType::NoteOn:
            std::snprintf(b, sizeof b, "NoteOn  ch%-2d %3d v%-3d",
                          m.channel, m.data1, m.data2); break;
        case MsgType::NoteOff:
            std::snprintf(b, sizeof b, "NoteOff ch%-2d %3d",
                          m.channel, m.data1); break;
        case MsgType::ControlChange:
            std::snprintf(b, sizeof b, "CC      ch%-2d %3d =%-3d",
                          m.channel, m.data1, m.data2); break;
        case MsgType::ProgramChange:
            std::snprintf(b, sizeof b, "PgmChg  ch%-2d %3d",
                          m.channel, m.data1); break;
        case MsgType::PitchBend:
            std::snprintf(b, sizeof b, "PitchB  ch%-2d %5d",
                          m.channel, m.data2 * 128 + m.data1); break;
        case MsgType::ChannelPressure:
            std::snprintf(b, sizeof b, "CPress  ch%-2d %3d",
                          m.channel, m.data1); break;
        case MsgType::ClockTick:    return "ClockTick";
        case MsgType::ClockStart:   return "ClockStart";
        case MsgType::ClockStop:    return "ClockStop";
        case MsgType::ClockContinue:return "ClockContinue";
        default:                    return "Unknown";
    }
    return b;
}

std::string ControllerDiagnostics::describeSysex(const SysExMsg& sx) {
    std::string out = "SysEx   [" + std::to_string(sx.len) + "] ";
    const int n = sx.len < 16 ? sx.len : 16;
    char hb[8];
    for (int i = 0; i < n; ++i) {
        std::snprintf(hb, sizeof hb, "%02X ", sx.data[i]);
        out += hb;
    }
    if (sx.len > n) out += "…";
    return out;
}

std::string ControllerDiagnostics::describe(const Entry& e) {
    char b[160];
    if (e.sysex) {
        std::snprintf(b, sizeof b, "%s dev%-2u %8.3fs  %s%s",
                      e.in ? "IN " : "OUT", e.device, e.atSec,
                      describeSysex(e.sx).c_str(),
                      e.mapped ? "  →consumed" : "");
        return b;
    }
    std::snprintf(b, sizeof b, "%s dev%-2u %8.3fs  %s%s",
                  e.in ? "IN " : "OUT", e.device, e.atSec,
                  describeMsg(e.msg).c_str(),
                  e.mapped ? "  →action" : "");
    std::string s = b;
    if (!e.decoded.empty()) s += "  [" + e.decoded + "]";
    return s;
}

std::string ControllerDiagnostics::dump(size_t max) const {
    std::string out;
    const size_t n = ring_.size() < max ? ring_.size() : max;
    const size_t start = ring_.size() - n;
    for (size_t i = start; i < ring_.size(); ++i) {
        out += describe(ring_[i]);
        out += '\n';
    }
    return out;
}

// ----------------------------------------------------------------------

bool LearnMode::capture(const MidiMsg& m, Mapping& out) {
    if (stage_ != Stage::Armed || m.isRealtime()) return false;
    // only capture "press"-like / moving events, not releases
    if (m.type == MsgType::NoteOff) return false;
    if (m.type == MsgType::NoteOn && m.data2 == 0) return false;
    out = tmpl_;
    out.in = addrOf(m);
    stage_ = Stage::Idle;
    return true;
}

// ----------------------------------------------------------------------

void MidiClock::ingest(const MidiMsg& m) {
    switch (m.type) {
        case MsgType::ClockStart:
            running_ = true; tickN_ = 0; accumUs_ = 0;
            lastTickUs_ = m.timeUs; break;
        case MsgType::ClockStop:
            running_ = false; break;
        case MsgType::ClockContinue:
            running_ = true; break;
        case MsgType::ClockTick:
            if (running_ && lastTickUs_ > 0 && m.timeUs > lastTickUs_) {
                accumUs_ += m.timeUs - lastTickUs_;
                ++tickN_;
                if (tickN_ >= kPpqn) {           // one beat elapsed
                    const double beatUs = (double)accumUs_;
                    if (beatUs > 0.0)
                        bpm_ = 60000000.0 / beatUs;
                    tickN_ = 0; accumUs_ = 0;
                }
            }
            lastTickUs_ = m.timeUs; break;
        default: break;
    }
}

bool MidiClock::clockOutDue(int64_t nowUs_, double bpm) {
    if (role_ != Role::Source || bpm <= 0.0 || !running_) return false;
    const double usPerTick = 60000000.0 / bpm / kPpqn;
    if (lastEmitUs_ == 0 ||
        nowUs_ - lastEmitUs_ >= (int64_t)usPerTick) {
        lastEmitUs_ = nowUs_;
        return true;
    }
    return false;
}

} // namespace onda::ctrl
