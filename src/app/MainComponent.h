#pragma once

#include <memory>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "../platform/audio/AudioEngine.h"

namespace onda {

class DeckStrip;

/// F3 shell: header bar (identity + device + DSP load + xruns + clips),
/// device configuration panel, and per-deck transport strips that exercise
/// the streamer -> fifo -> RT deck -> mixer path end to end.
class MainComponent : public juce::Component {
public:
    explicit MainComponent(audio::AudioEngine& engine);
    ~MainComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    audio::AudioEngine& engine_;

    std::unique_ptr<juce::Component> headerBar_;
    juce::AudioDeviceSelectorComponent deviceSelector_;
    std::vector<std::unique_ptr<DeckStrip>> decks_;
    juce::Slider masterGain_;
    juce::Slider cueMix_;
    juce::Label masterLabel_;
    juce::Label cueLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace onda
