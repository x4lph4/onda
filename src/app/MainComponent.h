#pragma once

#include <memory>

#include <juce_gui_extra/juce_gui_extra.h>

#include "ui/AppShell.h"
#include "ui/EngineBridge.h"
#include "ui/OndaTheme.h"
#include "ui/SettingsView.h"
#include "core/analysis/AnalysisService.h"
#include "core/library/MusicLibraryEngine.h"
#include "core/performance/PerformanceEngine.h"
#include "controller/ControllerEngine.h"
#include "platform/audio/AudioEngine.h"
#include "platform/config/Config.h"
#include "platform/midi/JuceMidi.h"
#include "ui/CommandManager.h"
#include "ui/ShortcutManager.h"
#include "ui/ThemeEngine.h"

namespace onda {

/// Phase-8 shell host: owns the UI framework pieces (theme, commands,
/// shortcuts, notifications, engine bridge) and mounts the AppShell.
/// All engine access inside views flows through EngineBridge +
/// CommandManager — widgets never see AudioEngine.
class MainComponent : public juce::Component, private juce::Timer {
public:
    MainComponent(audio::AudioEngine& engine,
                  AnalysisService& analysis,
                  PerformanceEngine& perf,
                  library::MusicLibraryEngine& lib,
                  Config& config);
    ~MainComponent() override {
        stopTimer();
        juce::Desktop::getInstance().setDefaultLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    ui::ThemeEngine      theme_;
    ui::OndaLookAndFeel  look_;
    ui::CommandManager   commands_;
    ui::ShortcutManager  shortcuts_;
    ui::NotificationCenter notes_;
    std::unique_ptr<ui::EngineBridge> bridge_;
    std::unique_ptr<ui::AppShell>     shell_;

    // Phase 9 — controller subsystem (MIDI now, HID-ready)
    std::unique_ptr<ctrl::ControllerEngine> ctrl_;
    std::unique_ptr<ctrl::MidiManager>      midi_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace onda
