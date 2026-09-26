#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "controller/ControllerEngine.h"

namespace onda::ctrl {

/// JUCE-backed MIDI manager — owns juce::MidiInput/MidiOutput
/// instances, converts messages both ways, polls the OS device list
/// for hot-plug. The core engine never sees JUCE types.
///
/// One instance per app; call start() after wiring the
/// ControllerEngine ports, stop() on shutdown.
class MidiManager : private juce::MidiInputCallback,
                    private juce::Timer {
public:
    explicit MidiManager(ControllerEngine& engine,
                         int pollMs = 800);
    ~MidiManager() override;

    void start();
    void stop();

    /// Number of open inputs/outputs (diagnostics).
    int openInputs()  const { return (int)ins_.size(); }
    int openOutputs() const { return (int)outs_.size(); }

private:
    struct In {
        std::unique_ptr<juce::MidiInput> dev;
        uint32_t id = 0;
        juce::String juceId;
    };
    struct Out {
        std::unique_ptr<juce::MidiOutput> dev;
        juce::String juceId;
    };

    void timerCallback() override { scan(); }
    void scan();                      // diff OS lists → attach/detach

    void handleIncomingMidiMessage(juce::MidiInput* src,
                                   const juce::MidiMessage& m) override;

    static MidiMsg  toMsg(const juce::MidiMessage& m);
    static juce::MidiMessage toJuce(const MidiMsg& m);
    static uint32_t deviceKey(const juce::String& osId);

    ControllerEngine* eng_;
    int pollMs_;
    std::unordered_map<uint32_t, In>  ins_;   // key = ctrl device id
    std::unordered_map<uint32_t, Out> outs_;
    bool firstScan_ = true;
};

} // namespace onda::ctrl
