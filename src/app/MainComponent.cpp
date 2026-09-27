#include "MainComponent.h"

#include "platform/log/Log.h"

namespace onda {

MainComponent::MainComponent(audio::AudioEngine& engine,
                             AnalysisService& analysis,
                             PerformanceEngine& perf,
                             library::MusicLibraryEngine& lib,
                             Config& config,
                             sampler::SamplerEngine* sampler,
                             rec::RecordingEngine* recorder,
                             adevice::AudioDeviceEngine* adevice,
                             stems::StemsEngine* stems)
    : look_(theme_), sampler_(sampler), recorder_(recorder),
      adevice_(adevice), engine_(&engine), stems_(stems) {
    juce::Desktop::getInstance().setDefaultLookAndFeel(&look_);
    setLookAndFeel(&look_);

    shortcuts_ = ui::ShortcutManager::defaultProfile();
    bridge_ = std::make_unique<ui::EngineBridge>(
        engine, perf, analysis, lib, commands_, notes_, sampler,
        recorder, adevice, stems);

    // ---- Phase 13: device engine ports (control thread only) ----
    if (adevice_) {
        adevice_->ports.mixerMap = [&engine](const adevice::ChannelMapping& m) {
            engine.setOutputMap(m.masterL, m.masterR,
                                m.cueL, m.cueR, m.boothL, m.boothR);
        };
        adevice_->ports.testSig = [&engine](adevice::TestSignal m) {
            engine.setTestSignal((int)m);
        };
        adevice_->ports.notify = [this](const std::string& t,
                                        const std::string& d) {
            notes_.warn(t, d);
        };
        adevice_->ports.cfgGet = [&config](const std::string& k) {
            return config.get(juce::Identifier(k), "")
                .toString().toStdString();
        };
        adevice_->ports.cfgSet = [&config](const std::string& k,
                                           const std::string& v) {
            config.set(juce::Identifier(k), juce::String(v));
        };
        adevice_->ports.micPermission = [](std::function<void(bool)> cb) {
            juce::RuntimePermissions::request(
                juce::RuntimePermissions::recordAudio, std::move(cb));
        };
        adevice_->enumerate();
    }
    shell_ = std::make_unique<ui::AppShell>(
        theme_, commands_, shortcuts_, engine, perf, analysis, lib,
        *bridge_, config, notes_);

    // Phase 23 — smart library layer (queries stay sync/indexed;
    // heavy work posts to the library worker thread).
    smartLib_ = std::make_unique<smart::SmartLibraryEngine>(lib);
    smartLib_->batch().setAnalyzeFn(
        [&analysis, &lib](int64_t trackId, bool /*force*/) {
            library::Track t;
            if (lib.tracks().getById(trackId, t) && !t.path.empty())
                analysis.analyzeTrack(t.path, (int)AnalysisPriority::High);
        });
    shell_->browser().setSmartLib(smartLib_.get());

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
        p.setParam = [&engine, &perf](uint16_t id, float v) {
            // deck params (0x8000 space) ride the perf layer; mixer
            // params go through MixerController as before
            if (ctrl::isDeckParam(id)) {
                const auto deck = (uint8_t)ctrl::deckParamDeck(id);
                if (ctrl::deckParamKind(id) == ctrl::kDeckParamPitch)
                    perf.onUserPitch(deck, v);   // §26 pitch policy
                else
                    perf.setPitchRange(deck, v); // §35 pitch engine
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
        // Phase 28 §38-40 — jog decisions live in the performance
        // layer's JogEngine (seek vs bend vs scratch vs browse),
        // slip-aware and shared with every other input path.
        p.jog = [&perf](int deck, int mode, double delta) {
            perf.jog(deck, mode, delta);
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
        lib, adevice_));
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
    if (stems_)
        stems_->poll();     // publish finished stem sets + retire views
    if (recorder_)
        recorder_->tick();  // writer health, splits, clips, setlist
    if (adevice_ && engine_) {
        const auto& as = engine_->stats();
        adevice_->tick(
            (int64_t)juce::Time::getMillisecondCounter(),
            as.maxCallbackMs.load(), as.lastCallbackMs.load(),
            as.xrunCount.load(), as.cpuLoad.load(),
            (int64_t)as.underrunCount.load());
    }
}

} // namespace onda
