#pragma once

#include <memory>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "../platform/audio/AudioEngine.h"
#include "../core/analysis/AnalysisService.h"
#include "../core/performance/PerformanceEngine.h"

namespace onda {

class DeckStrip;

/// Phase-2 functional shell: every engine feature gets a control —
/// deck transport, pitch/keylock/loops/seek, EQ/filter, faders,
/// crossfader, cue bus, master + meters. Functional layout, not final UX.
class MainComponent : public juce::Component, private juce::Timer {
public:
    MainComponent(audio::AudioEngine& engine, AnalysisService& analysis,
                  PerformanceEngine& perf);
    ~MainComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

private:
    audio::AudioEngine& engine_;
    AnalysisService& analysis_;
    PerformanceEngine& perf_;

    std::unique_ptr<juce::Component> headerBar_;
    std::vector<std::unique_ptr<DeckStrip>> decks_;

    // mixer column
    juce::Slider masterGain_, cueVol_, cueMix_, crossfader_;
    juce::ComboBox xfCurve_;
    juce::Label masterLabel_, cueVolLabel_, cueMixLabel_, xfLabel_, meterLabel_;
    juce::TextButton settingsBtn_;
    std::unique_ptr<juce::Component> devicePanel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace onda
