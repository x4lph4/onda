#include "JuceMidi.h"

#include <cstring>

namespace onda::ctrl {

MidiManager::MidiManager(ControllerEngine& engine, int pollMs)
    : eng_(&engine), pollMs_(pollMs) {}

MidiManager::~MidiManager() { stop(); }

void MidiManager::start() {
    scan();                 // initial attach
    startTimer(pollMs_);
}

void MidiManager::stop() {
    stopTimer();
    ins_.clear();
    outs_.clear();
}

uint32_t MidiManager::deviceKey(const juce::String& osId) {
    // stable-ish id from the OS identifier string
    uint32_t h = 2166136261u;
    for (int i = 0; i < osId.length(); ++i) {
        h ^= (uint32_t)osId[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/// SysEx conversion — bounded copy into the fixed SysExMsg buffer
/// (§13): longer dumps are truncated at the adapter, never queued.
static SysExMsg toSysEx(const juce::MidiMessage& m) {
    SysExMsg sx;
    sx.timeUs = (int64_t)(m.getTimeStamp() * 1e6);
    const int n = std::min<int>(m.getSysExDataSize() + 1,
                                (int)SysExMsg::kMax);
    sx.data[0] = 0xF0;
    if (n > 1)
        std::memcpy(sx.data + 1, m.getSysExData(), (size_t)(n - 1));
    sx.len = (uint16_t)n;
    return sx;
}

MidiMsg MidiManager::toMsg(const juce::MidiMessage& m) {
    MidiMsg o;
    o.timeUs = (int64_t)(m.getTimeStamp() * 1e6);
    o.channel = (uint8_t)juce::jlimit(0, 15, m.getChannel() - 1);
    if (m.isNoteOn())            { o.type = MsgType::NoteOn;
        o.data1 = (uint8_t)m.getNoteNumber();
        o.data2 = (uint8_t)m.getVelocity(); }
    else if (m.isNoteOff())      { o.type = MsgType::NoteOff;
        o.data1 = (uint8_t)m.getNoteNumber(); }
    else if (m.isController())   { o.type = MsgType::ControlChange;
        o.data1 = (uint8_t)m.getControllerNumber();
        o.data2 = (uint8_t)m.getControllerValue(); }
    else if (m.isProgramChange()){ o.type = MsgType::ProgramChange;
        o.data1 = (uint8_t)m.getProgramChangeNumber(); }
    else if (m.isPitchWheel())   { o.type = MsgType::PitchBend;
        const int v = m.getPitchWheelValue();
        o.data1 = (uint8_t)(v & 0x7F); o.data2 = (uint8_t)(v >> 7); }
    else if (m.isChannelPressure()){ o.type = MsgType::ChannelPressure;
        o.data1 = (uint8_t)m.getChannelPressureValue(); }
    else if (m.isMidiClock())      o.type = MsgType::ClockTick;
    else if (m.isMidiStart())      o.type = MsgType::ClockStart;
    else if (m.isMidiStop())       o.type = MsgType::ClockStop;
    else if (m.isMidiContinue())   o.type = MsgType::ClockContinue;
    else                           o.type = MsgType::Unknown;
    return o;
}

juce::MidiMessage MidiManager::toJuce(const MidiMsg& m) {
    switch (m.type) {
        case MsgType::NoteOn:
            return juce::MidiMessage::noteOn(m.channel + 1, m.data1,
                                             m.data2);
        case MsgType::NoteOff:
            return juce::MidiMessage::noteOff(m.channel + 1, m.data1);
        case MsgType::ControlChange:
            return juce::MidiMessage::controllerEvent(
                m.channel + 1, m.data1, m.data2);
        case MsgType::ProgramChange:
            return juce::MidiMessage::programChange(m.channel + 1,
                                                  m.data1);
        case MsgType::PitchBend:
            return juce::MidiMessage::pitchWheel(
                m.channel + 1, m.data1 | (m.data2 << 7));
        case MsgType::ChannelPressure:
            return juce::MidiMessage::channelPressureChange(
                m.channel + 1, m.data1);
        case MsgType::ClockTick:     return juce::MidiMessage::midiClock();
        case MsgType::ClockStart:    return juce::MidiMessage::midiStart();
        case MsgType::ClockStop:     return juce::MidiMessage::midiStop();
        case MsgType::ClockContinue: return juce::MidiMessage::midiContinue();
        default:                     return juce::MidiMessage();
    }
}

void MidiManager::scan() {
    const auto avail = juce::MidiInput::getAvailableDevices();

    // detach inputs that vanished
    for (auto it = ins_.begin(); it != ins_.end();) {
        bool found = false;
        for (auto& d : avail)
            if (d.identifier == it->second.juceId) found = true;
        if (!found) {
            eng_->detachDevice(it->first,
                               juce::Time::getMillisecondCounterHiRes()
                                   / 1000.0);
            outs_.erase(it->first);
            it = ins_.erase(it);
        } else ++it;
    }

    // attach new inputs
    for (auto& d : avail) {
        const uint32_t id = deviceKey(d.identifier);
        if (ins_.count(id)) continue;
        auto in = juce::MidiInput::openDevice(d.identifier, this);
        if (!in) continue;
        in->start();
        DeviceInfo di;
        di.id = id;
        di.name = d.name.toStdString();
        di.transport = Transport::MidiUsb;
        di.hasInput = true;
        // find the matching output device by name (JUCE lists them
        // separately; same-name pairing is the standard convention)
        for (auto& od : juce::MidiOutput::getAvailableDevices())
            if (od.name == d.name) {
                auto out = juce::MidiOutput::openDevice(od.identifier);
                if (out) {
                    outs_[id].dev = std::move(out);
                    outs_[id].juceId = od.identifier;
                    di.hasOutput = true;
                }
                break;
            }
        ins_[id] = In{ std::move(in), id, d.identifier };
        eng_->attachDevice(di,
            [this, id](const MidiMsg& m) {
                auto it = outs_.find(id);
                if (it != outs_.end() && it->second.dev)
                    it->second.dev->sendMessageNow(toJuce(m));
            },
            juce::Time::getMillisecondCounterHiRes() / 1000.0);
        // §13 outbound SysEx path — vendor handshakes, position dumps
        if (auto* st = eng_->device(id))
            st->sendSysex = [this, id](const SysExMsg& sx) {
                auto it = outs_.find(id);
                if (it != outs_.end() && it->second.dev && sx.len > 1)
                    it->second.dev->sendMessageNow(
                        juce::MidiMessage::createSysExMessage(
                            sx.data + 1, sx.len - 1));
            };
    }
    firstScan_ = false;
}

void MidiManager::handleIncomingMidiMessage(
    juce::MidiInput* src, const juce::MidiMessage& m) {
    for (auto& [id, in] : ins_)
        if (in.dev.get() == src) {
            if (m.isSysEx()) eng_->processSysex(id, toSysEx(m));
            else             eng_->processInput(id, toMsg(m));
            return;
        }
}

} // namespace onda::ctrl
