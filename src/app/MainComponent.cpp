#include "MainComponent.h"

namespace onda {

MainComponent::MainComponent(audio::AudioEngine& engine,
                             AnalysisService& analysis,
                             PerformanceEngine& perf,
                             library::MusicLibraryEngine& lib,
                             Config& config)
    : look_(theme_) {
    juce::Desktop::getInstance().setDefaultLookAndFeel(&look_);
    setLookAndFeel(&look_);

    shortcuts_ = ui::ShortcutManager::defaultProfile();
    bridge_ = std::make_unique<ui::EngineBridge>(
        engine, perf, analysis, lib, commands_, notes_);
    shell_ = std::make_unique<ui::AppShell>(
        theme_, commands_, shortcuts_, engine, perf, analysis, lib,
        *bridge_, config, notes_);

    // app-level command actions → shell panels
    bridge_->mutablePorts().appAction = [this](uint32_t id) {
        shell_->appAction(id);
    };
    // browser selection → deck load commands
    bridge_->mutablePorts().browserSelection = [this]() -> std::string {
        const auto t = shell_->browser().selected();
        return t.path;
    };
    // double-click loads to deck A (Shift+click for B later via DnD)
    shell_->browser().onLoadDeck = [this](const library::Track& t) {
        if (!t.path.empty())
            bridge_->ports().loadTrack(0, t.path);
    };

    shell_->attachSettings(std::make_unique<ui::SettingsView>(
        theme_, commands_, shortcuts_, engine.deviceManager(), config,
        lib));
    shell_->onThemeChange = [this](int id) {
        theme_.set((ui::ThemeId)id);
        look_.syncColors();
        repaint();
    };
    shell_->onDensityChange = [this](int d) {
        theme_.setDensity((ui::Density)d);
        resized();
        repaint();
    };

    addAndMakeVisible(*shell_);
    setSize(1440, 860);
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(ui::col(theme_.theme().palette.background));
}

void MainComponent::resized() {
    if (shell_) shell_->setBounds(getLocalBounds());
}

} // namespace onda
