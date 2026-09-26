#include "MainComponent.h"

#include "platform/log/Log.h"

namespace onda {

MainComponent::MainComponent(audio::AudioEngine& engine,
                             AnalysisService& analysis,
                             PerformanceEngine& perf,
                             library::MusicLibraryEngine& lib,
                             Config& config,
                             sampler::SamplerEngine* sampler,
                             rec::RecordingEngine* recorder)
    : look_(theme_), sampler_(sampler), recorder_(recorder) {
    juce::Desktop::getInstance().setDefaultLookAndFeel(&look_);
    setLookAndFeel(&look_);

    shortcuts_ = ui::ShortcutManager::defaultProfile();
    bridge_ = std::make_unique<ui::EngineBridge>(
        engine, perf, analysis, lib, commands_, notes_, sampler,
        recorder);
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

    // ---- Phase 9: controller engine — same ports the UI uses ----
    const auto profileDir =
        config.file().getSiblingFile("controllers")
            .getFullPathName().toStdString();
    ctrl_ = std::make_unique<ctrl::ControllerEngine>(profileDir);
    {
        ctrl::ControllerEngine::Ports p;
        p.sendCommand = [this](uint32_t id, float v) {
            commands_.invoke(id, ui::CommandSource::Midi, v);
        };
        p.commandState = [this](uint32_t id) {
            return commands_.state(id);
        };
        p.commandIdFor = [this](const std::string& n) {
            return commands_.idFor(n);
        };
        p.setParam = [&engine](uint16_t id, float v) {
            // deck params (0x8000 space) ride the lock-free rt queue;
            // mixer params go through MixerController as before
            if (ctrl::isDeckParam(id)) {
                const auto deck = (uint8_t)ctrl::deckParamDeck(id);
                if (ctrl::deckParamKind(id) == ctrl::kDeckParamPitch)
                    engine.commands().tryPush(
                        rt::SetPitch{deck, v});
                else
                    engine.commands().tryPush(
                        rt::SetPitchRange{deck, v});
                return;
            }
            engine.mixerCtl().setParam(id, v);
        };
        p.paramState = [&engine](uint16_t id) {
            if (ctrl::isDeckParam(id)) {
                const auto& ds =
                    engine.deckStats(ctrl::deckParamDeck(id));
                return ctrl::deckParamKind(id) == ctrl::kDeckParamPitch
                       ? ds.pitchPct.load() : ds.pitchRange.load();
            }
            return engine.mixerCtl().value(id);
        };
        // jog → same rt queue the UI waveforms use
        p.jog = [&engine](int deck, int mode, double delta) {
            auto* q = &engine.commands();
            if (mode == (int)ctrl::JogMode::JogPitchBend) {
                q->tryPush(rt::SetPitchBend{
                    (uint8_t)deck,
                    (float)juce::jlimit(-8.0, 8.0, delta * 0.25)});
            } else { // JogSeek / JogScratch: nudge position
                const auto& ds = engine.deckStats(deck);
                const double cur = ds.positionSec.load();
                const double scale =
                    mode == (int)ctrl::JogMode::JogScratch ? 0.10 : 0.04;
                q->tryPush(rt::DeckSeek{
                    (uint8_t)deck, cur + delta * scale});
            }
        };
        p.notify = [this](const std::string& t,
                          const std::string& d, int lvl) {
            if (lvl == 2)      notes_.warn(t, d);
            else if (lvl == 1) notes_.info(t, d);
            else               notes_.info(t, d);
        };
        ctrl_->setPorts(std::move(p));
        // Phase 10: DDJ-FLX4 semantic plugin (pad modes, shift,
        // jog touch/vinyl, browse, tempo-range cycle) — attaches on
        // device-connect via the preDispatch/onDevice hooks.
        flx4_.attach(*ctrl_);
    }
    midi_ = std::make_unique<ctrl::MidiManager>(*ctrl_);
    midi_->start();
    startTimerHz(20);   // feedback poll + engine housekeeping

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

    // Phase 12: report recordings interrupted by a previous crash —
    // journal scan happens on the app thread before the timer starts.
    if (recorder_) {
        const auto items = recorder_->scanRecovery();
        if (!items.empty()) {
            notes_.warn("Unfinished recordings",
                std::to_string(items.size()) +
                " interrupted session(s) found in " +
                recorder_->outputDir() +
                " — recover from the session store");
            for (const auto& it : items)
                log::warn("Recoverable recording: " +
                          juce::String(it.wavPath));
        }
    }

    addAndMakeVisible(*shell_);
    setSize(1440, 860);
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(ui::col(theme_.theme().palette.background));
}

void MainComponent::resized() {
    if (shell_) shell_->setBounds(getLocalBounds());
}

void MainComponent::timerCallback() {
    if (ctrl_)
        ctrl_->tick(juce::Time::getMillisecondCounterHiRes() / 1000.0);
    if (sampler_)
        sampler_->tick();   // drain loader results + retire RT trash
    if (recorder_)
        recorder_->tick();  // writer health, splits, clips, setlist
}

} // namespace onda
