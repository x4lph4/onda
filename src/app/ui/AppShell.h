#pragma once

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "BrowserView.h"
#include "DeckView.h"
#include "EngineBridge.h"
#include "FXView.h"
#include "MixerView.h"
#include "SettingsView.h"
#include "TopBar.h"
#include "WaveformView.h"
#include "controls/MeterBar.h"
#include "core/library/MusicLibraryEngine.h"
#include "platform/config/Config.h"
#include "ui/CommandManager.h"
#include "ui/LayoutSpec.h"
#include "ui/NotificationCenter.h"
#include "ui/ShortcutManager.h"
#include "ui/UndoManager.h"
#include "ui/pads/PadFeedbackEngine.h"

namespace onda::ui {

/// Application shell — owns every top-level view, runs the 30 Hz
/// snapshot tick, dispatches keyboard shortcuts, manages panels and
/// overlays. Layout comes from LayoutSpec (responsive, tested).
///
///   ┌─ TopBar ────────────────────────────────────────────────┐
///   │ WaveA ────────────┬──────────────┬──────────── WaveB    │
///   │ Deck A            │   MIXER      │            Deck B    │
///   │ ─────── FX strip (optional) ─────────────────────────── │
///   │ ─────── Browser / Library (optional) ────────────────── │
///   └──────────── toasts (overlay) ── settings (overlay) ─────┘
class AppShell : public juce::Component, private juce::Timer {
public:
    AppShell(const ThemeEngine& te, CommandManager& cm,
             ShortcutManager& shortcuts,
             audio::AudioEngine& engine,
             PerformanceEngine& perf,
             AnalysisService& analysis,
             library::MusicLibraryEngine& lib,
             EngineBridge& bridge,
             Config& config,
             NotificationCenter& notes)
        : theme_(&te), cmds_(&cm), shortcuts_(&shortcuts),
          bridge_(&bridge), engine_(&engine), perf_(&perf),
          analysis_(&analysis), lib_(&lib), notes_(&notes),
          topbar_(te, cm), mixer_(te, engine.mixerCtl()),
          browser_(te, cm, lib),
          notifications_(te, notes) {
        for (int d = 0; d < 4; ++d) {           // §45: 4-deck capable
            decks_[(size_t)d] =
                std::make_unique<DeckView>(te, cm, d);
            waves_[(size_t)d] =
                std::make_unique<WaveformView>(te, d);
            waves_[(size_t)d]->setDeckColor(
                theme_->theme().waveColor(d));
            addAndMakeVisible(*decks_[(size_t)d]);
            addAndMakeVisible(*waves_[(size_t)d]);

            // seek + pitch wiring (view → rt commands)
            auto* q = &engine.commands();
            waves_[(size_t)d]->onSeek = [q, d](double sec) {
                q->tryPush(rt::DeckSeek{(uint8_t)d, sec});
            };
            // §42 — every other waveform interaction arrives as a
            // WaveCmd and is routed to the matching engine surface.
            waves_[(size_t)d]->onWaveCmd = [this, q, d](
                    const WaveCmd& c) { routeWaveCmd(d, q, c); };
            // Phase 15 §26: user fader moves go through the sync
            // pitch policy (Override/Disable/ManualAsBend), never
            // raw rt::SetPitch — hardware and software can't fight.
            decks_[(size_t)d]->onPitch = [this, d](float v) {
                perf_->onUserPitch(d, v);
            };
            decks_[(size_t)d]->onPitchRange = [q, d](float v) {
                q->tryPush(rt::SetPitchRange{(uint8_t)d, v});
            };
            decks_[(size_t)d]->bindPitch();
        }
        // Phase 20 §41 — shared performance undo stack for the
        // perf.undo/perf.redo commands and every cue/loop/grid edit.
        bridge_->mutablePorts().undo = &undo_;
        // Phase 19: deck.*.wave.* / deck.*.deck.timeMode commands land
        // here — view-local presentation state, no engine calls.
        bridge_->mutablePorts().viewAction =
            [this](int d, int action, double) {
                if (d < 0 || d >= 4) return;
                switch (action) {
                    case cmd::WaveZoomIn:
                        waves_[(size_t)d]->zoomIn(); break;
                    case cmd::WaveZoomOut:
                        waves_[(size_t)d]->zoomOut(); break;
                    case cmd::WaveZoomFit:
                        waves_[(size_t)d]->zoomFit(); break;
                    case cmd::WaveAutoScroll:
                        waves_[(size_t)d]->cycleAutoScroll(); break;
                    case cmd::WaveGridEditTgl:
                        waves_[(size_t)d]->setGridEdit(
                            !waves_[(size_t)d]->gridEdit()); break;
                    case cmd::DeckTimeMode:
                        decks_[(size_t)d]->cycleTimeMode(); break;
                    default: break;
                }
            };
        fx_ = std::make_unique<FXView>(te, engine.mixerCtl());

        addAndMakeVisible(topbar_);
        addAndMakeVisible(mixer_);
        addAndMakeVisible(*fx_);
        addAndMakeVisible(browser_);
        addChildComponent(notifications_);
        notifications_.setAlwaysOnTop(true);

        setWantsKeyboardFocus(true);
        startTimerHz(30);
    }

    void attachSettings(std::unique_ptr<SettingsView> s) {
        settings_ = std::move(s);
        addChildComponent(*settings_);
        settings_->setAlwaysOnTop(true);
        settings_->onTheme = [this](int id) {
            if (onThemeChange) onThemeChange(id);
        };
        settings_->onClose = [this] { setSettingsVisible(false); };
        settings_->onDensity = [this](int d) {
            if (onDensityChange) onDensityChange(d);
        };
    }

    std::function<void(int)> onThemeChange, onDensityChange;

    NotificationCenter& notes() { return *notes_; }
    BrowserView&        browser() { return browser_; }
    void setSettingsVisible(bool v) {
        if (settings_) settings_->setVisible(v);
        topbar_.settingsButton().setState(v ? ControlState::On
                                            : ControlState::Off);
        if (v) grabKeyboardFocus();
    }
    bool settingsVisible() const {
        return settings_ && settings_->isVisible();
    }
    void setBrowserVisible(bool v) {
        prefs_.browserOpen = v;
        browser_.setVisible(v);
        topbar_.browserButton().setState(v ? ControlState::On
                                           : ControlState::Off);
        resized();
    }
    void setFxVisible(bool v) {
        prefs_.fxOpen = v;
        fx_->setVisible(v);
        topbar_.fxButton().setState(v ? ControlState::On
                                      : ControlState::Off);
        resized();
    }
    void setBrowserSelectionPort(std::function<std::string()> fn) {
        selPort_ = std::move(fn);
    }

    bool keyPressed(const juce::KeyPress& kp) override {
        const KeyDesc k = toKeyDesc(kp);
        const uint32_t id = shortcuts_->lookup(k);
        if (id == cmd::Invalid) return false;
        return cmds_->invoke(id, CommandSource::Keyboard,
                             kp.isCurrentlyDown() ? 1.0f : 0.0f);
    }

    /// JUCE KeyPress → platform-neutral KeyDesc (single conversion
    /// point — MIDI/HID never needs this).
    static KeyDesc toKeyDesc(const juce::KeyPress& kp) {
        KeyDesc k;
        const auto& m = kp.getModifiers();
        if (m.isCtrlDown() || m.isCommandDown()) k.mods |= key::Ctrl;
        if (m.isShiftDown())                     k.mods |= key::Shift;
        if (m.isAltDown())                       k.mods |= key::Alt;
        const int code = kp.getKeyCode();
        const juce::juce_wchar tc = kp.getTextCharacter();
        if (code >= 'a' && code <= 'z') k.code = (uint32_t)(code - 'a' + 'A');
        else if (code >= 'A' && code <= 'Z') k.code = (uint32_t)code;
        else if (code >= '0' && code <= '9') k.code = (uint32_t)code;
        else if (code >= juce::KeyPress::F1Key &&
                 code <= juce::KeyPress::F16Key)
            k.code = key::F1 + (uint32_t)(code - juce::KeyPress::F1Key);
        else if (code == juce::KeyPress::spaceKey) k.code = key::Space;
        else if (code == juce::KeyPress::returnKey) k.code = key::Return;
        else if (code == juce::KeyPress::escapeKey) k.code = key::Escape;
        else if (code == juce::KeyPress::tabKey) k.code = key::Tab;
        else if (code == juce::KeyPress::leftKey) k.code = key::Left;
        else if (code == juce::KeyPress::rightKey) k.code = key::Right;
        else if (code == juce::KeyPress::upKey) k.code = key::Up;
        else if (code == juce::KeyPress::downKey) k.code = key::Down;
        else if (code == juce::KeyPress::deleteKey) k.code = key::Delete;
        else if (code == juce::KeyPress::backspaceKey) k.code = key::Backspace;
        else if (code == juce::KeyPress::homeKey) k.code = key::Home;
        else if (code == juce::KeyPress::endKey) k.code = key::End;
        else if (code == juce::KeyPress::pageUpKey) k.code = key::PageUp;
        else if (code == juce::KeyPress::pageDownKey) k.code = key::PageDown;
        else if (code == ',') k.code = ',';
        else if (code == '.') k.code = '.';
        else if (code == '/') k.code = '/';
        else if (code == '-') k.code = '-';
        else if (code == '=' || code == '+') k.code = '+';
        else k.code = 0;
        (void)tc;
        return k;
    }

    void appAction(uint32_t id) {
        switch (id) {
            case cmd::BrowserTogglePanel:
                setBrowserVisible(!prefs_.browserOpen); break;
            case cmd::AppToggleSettings:
                setSettingsVisible(!settingsVisible()); break;
            case cmd::AppToggleFx:
                setFxVisible(!prefs_.fxOpen); break;
            case cmd::BrowserFocusSearch:
                if (!prefs_.browserOpen) setBrowserVisible(true);
                browser_.focusSearch(); break;
            case cmd::BrowserNextTrack: browser_.moveSelection(1); break;
            case cmd::BrowserPrevTrack: browser_.moveSelection(-1); break;
            default: break;
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(col(theme_->theme().palette.background));
    }

    void resized() override {
        const Metrics m = theme_->metrics();
        const auto L = LayoutSpec::compute(
            getWidth(), getHeight(), m.topBarH, prefs_);
        topbar_.setBounds(L.topBar.x, L.topBar.y, L.topBar.w, L.topBar.h);
        if (L.fxVisible)
            fx_->setBounds(L.fx.x, L.fx.y, L.fx.w, L.fx.h);
        if (L.browserVisible)
            browser_.setBounds(L.browser.x, L.browser.y,
                               L.browser.w, L.browser.h);
        for (int d = 0; d < 4; ++d) {          // §45: per-deck regions
            const bool vis = d < L.deckCount;
            waves_[(size_t)d]->setVisible(vis);
            decks_[(size_t)d]->setVisible(vis);
            if (vis) {
                waves_[(size_t)d]->setBounds(L.waves[d].x, L.waves[d].y,
                                             L.waves[d].w, L.waves[d].h);
                decks_[(size_t)d]->setBounds(L.decks[d].x, L.decks[d].y,
                                             L.decks[d].w, L.decks[d].h);
            }
        }
        mixer_.setBounds(L.mixer.x, L.mixer.y, L.mixer.w, L.mixer.h);
        if (settings_ && settings_->isVisible())
            settings_->setBounds(getLocalBounds().reduced(
                juce::jmin(120, getWidth() / 6),
                juce::jmin(80, getHeight() / 8)));
        notifications_.setBounds(getLocalBounds());
    }

    // ---- 30 Hz tick: snapshot → views; cheap atomic reads only ----
    void timerCallback() override {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        bridge_->snapshot(snap_);
        // measure real UI update cadence + work time (§35 perf budget)
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (lastTick_ > 0.0) {
            const double dt = now - lastTick_;
            tickAcc_ += dt; ++tickN_;
            if (tickAcc_ >= 1000.0) {
                uiFps_ = (float)(tickN_ * 1000.0 / tickAcc_);
                tickAcc_ = 0.0; tickN_ = 0;
            }
        }
        lastTick_ = now;
        snap_.audio.uiFps = uiFps_;
        snap_.audio.uiFrameMs =
            (float)(now - t0);
        topbar_.setSnapshot(snap_);
        const int deckN = std::clamp(snap_.master.deckMode, 2, 4);
        if (prefs_.deckMode != deckN) {        // 2↔4 deck layout (§45)
            prefs_.deckMode = deckN;
            resized();
        }
        for (int d = 0; d < 4; ++d) {
            decks_[(size_t)d]->setSnapshot(snap_.deck[d]);
            waves_[(size_t)d]->setSnapshot(snap_.deck[d]);
            if (d < deckN) feedDeckVisuals(d);
            mixer_.setChannelSnapshot(d, snap_.channel[d],
                                      1.0f / 30.0f);
        }
        if (mixer_.channelCount() != snap_.master.deckMode)
            mixer_.setChannelCount(snap_.master.deckMode);
        mixer_.setSnapshot(snap_.master, 1.0f / 30.0f);
        fx_->setSnapshot(snap_);
        browser_.tick();
        notes_->tick(1.0 / 30.0);
        if (notes_->size() > 0) notifications_.repaint();
    }

private:
    /// Waveform/grid/cue feeds that need engine (not snapshot) access —
    /// cached per loaded path; only refreshed when the track changes.
    void feedDeckVisuals(int d) {
        const auto& ds = snap_.deck[d];
        if (ds.path != wfPath_[(size_t)d]) {
            wfPath_[(size_t)d] = ds.path;
            bundles_[(size_t)d] = ds.path.empty()
                ? nullptr : analysis_->getAnalysis(ds.path);
            const WaveformData* wf =
                bundles_[(size_t)d] ? &bundles_[(size_t)d]->waveform
                                    : nullptr;
            wfRate_[(size_t)d] =
                wf && wf->sampleRate > 0.0 ? wf->sampleRate : 0.0;
            waves_[(size_t)d]->setWaveform(wf);
            waves_[(size_t)d]->setGrid(
                bundles_[(size_t)d] && !bundles_[(size_t)d]->grid.empty()
                    ? &bundles_[(size_t)d]->grid : nullptr);
        }
        // analysis may complete after load — refresh the bundle when
        // the current one lacks a waveform but the track keeps playing
        if (bundles_[(size_t)d] &&
            bundles_[(size_t)d]->waveform.empty()
            && !ds.path.empty() && (snap_.frame & 0x3F) == 0) {
            bundles_[(size_t)d] = analysis_->getAnalysis(ds.path);
            if (bundles_[(size_t)d] &&
                !bundles_[(size_t)d]->waveform.empty()) {
                wfRate_[(size_t)d] =
                    bundles_[(size_t)d]->waveform.sampleRate;
                waves_[(size_t)d]->setWaveform(
                    &bundles_[(size_t)d]->waveform);
                if (!bundles_[(size_t)d]->grid.empty())
                    waves_[(size_t)d]->setGrid(
                        &bundles_[(size_t)d]->grid);
            }
        }
        // user-edited grid via perf engine takes precedence
        if (perf_->hasGrid(d))
            waves_[(size_t)d]->setGrid(perf_->grid(d));
        // hot-cue pads + waveform markers from perf engine
        const auto& cues = perf_->cues(d);
        std::array<uint32_t, 8> cols {};
        std::array<bool, 8>     set {};
        std::vector<CueMarker>  marks;
        std::vector<LoopRegion> loops;
        for (const Cue& c : cues) {
            const double sec =
                wfRate_[(size_t)d] > 0.0
                    ? c.frame / wfRate_[(size_t)d] : 0.0;
            if (c.type == CueType::Hot && c.slot >= 0 && c.slot < 8) {
                cols[(size_t)c.slot] = c.argb;
                set[(size_t)c.slot]  = true;
            }
            if (c.type == CueType::Loop && c.frame2 > c.frame) {
                loops.push_back({ sec,
                    wfRate_[(size_t)d] > 0.0
                        ? c.frame2 / wfRate_[(size_t)d] : sec,
                    true, true, c.slot });   // saved cue-loop
            } else {
                marks.push_back({ sec, c.argb,
                                  c.label.empty()
                                      ? std::to_string(c.slot + 1)
                                      : c.label,
                                  c.slot,
                                  c.type == CueType::Memory ? 1 : 0 });
            }
        }
        decks_[(size_t)d]->setCuePads(cols, set);
        waves_[(size_t)d]->setMarkers(marks, loops);
        // §8/§36-37 — memory-cue strip + pad lights + size readouts.
        {
            pads::PadDeckState ps;
            ps.mode       = bridge_->pads().mode(d);
            ps.playing    = ds.state == (int)DeckState::Playing;
            ps.loopOn     = ds.loopOn;
            ps.rolling    = perf_->rollActive(d);
            ps.loopSizeIdx = perf_->loopSizeIndex(d);
            ps.jumpSizeIdx = perf_->jumpSizeIndex(d);
            ps.pendingLoopIn = perf_->loopInArmed(d) ? 0 : -1;
            std::array<uint32_t, 8> mcol {};
            std::array<bool, 8>     mset {};
            for (const Cue& c : cues) {
                if (c.type != CueType::Memory) {
                    if (c.slot >= 0 && c.slot < 8)
                        ps.hot[(size_t)c.slot] = &c;
                } else if (c.slot >= 0 && c.slot < 8) {
                    mcol[(size_t)c.slot] = c.argb;
                    mset[(size_t)c.slot] = true;
                    ps.mem[(size_t)c.slot] = &c;
                }
            }
            auto& pp = decks_[(size_t)d]->perfPanel();
            pp.setMemCues(mcol, mset);
            pp.setPadLights(padFb_.lights(ps));
            pp.setLoopBeats(perf_->loopSizeBeats(d));
            pp.setJumpBeats(perf_->jumpSizeBeats(d));
            pp.setLoopPending(ps.pendingLoopIn >= 0);
        }
        // active loop from deck stats
        if (ds.loopOn)
            loops.push_back({ ds.loopInSec, ds.loopOutSec, true,
                              false, -1 });
        // §37 artwork — cached image per track; loaded once per
        // trackId (UI thread, small thumb, cache hit afterwards).
        if (ds.trackId != artTrack_[(size_t)d]) {
            artTrack_[(size_t)d] = ds.trackId;
            juce::Image img;
            if (ds.trackId >= 0) {
                const std::string ap = lib_->artworkPath(ds.trackId);
                if (!ap.empty())
                    img = juce::ImageFileFormat::loadFrom(
                        juce::File(juce::String::fromUTF8(ap.c_str())));
            }
            decks_[(size_t)d]->setArtwork(img);
        }
    }

    /// §42 routing — WaveCmd values from WaveformView to the right
    /// engine surface. No musical logic in the component.
    void routeWaveCmd(int d, rt::CommandQueue* q, const WaveCmd& c) {
        switch (c.kind) {
            case WaveCmdKind::CueTrigger:
                perf_->triggerCue(d, c.slot); break;
            case WaveCmdKind::CueSet:
                if (c.slot >= 0)
                    perf_->setCue(d, c.slot, CueType::Hot,
                                  (int64_t)(c.a * wfRate_[(size_t)d]));
                break;
            case WaveCmdKind::CueDelete:
                if (c.slot >= 0) perf_->deleteCue(d, c.slot); break;
            case WaveCmdKind::LoopSet:
                q->tryPush(rt::SetLoop{ (uint8_t)d, c.a, c.b });
                q->tryPush(rt::SetLoopOn{ (uint8_t)d, true });
                break;
            case WaveCmdKind::GridNudgeMs:
                bridge_->mutablePorts().analysisEdit(
                    d, (int)AnalysisEdit::NudgeGrid, c.a);
                break;
            case WaveCmdKind::GridSetFirstBeat:
                bridge_->mutablePorts().analysisEdit(
                    d, (int)AnalysisEdit::SetFirstBeat,
                    c.a * wfRate_[(size_t)d]);
                break;
            default: break;   // Seek/ScrubSeek/Zoom/Scroll handled
        }                     // by the view itself / onSeek
    }

    const ThemeEngine* theme_;
    CommandManager*    cmds_;
    ShortcutManager*   shortcuts_;
    EngineBridge*      bridge_;
    audio::AudioEngine* engine_;
    PerformanceEngine* perf_;
    AnalysisService*   analysis_;
    library::MusicLibraryEngine* lib_;

    UiSnapshot snap_;
    LayoutPrefs prefs_;
    UndoManager            undo_;     // §41 — perf edit undo stack
    pads::PadFeedbackEngine padFb_;   // §36 — pad light model
    NotificationCenter* notes_;
    std::function<std::string()> selPort_;

    TopBar topbar_;
    std::array<std::unique_ptr<DeckView>, 4>     decks_;
    std::array<std::unique_ptr<WaveformView>, 4> waves_;
    MixerView mixer_;
    std::unique_ptr<FXView>      fx_;
    BrowserView browser_;
    std::unique_ptr<SettingsView> settings_;
    NotificationOverlay notifications_;

    std::string wfPath_[4];
    std::shared_ptr<AnalysisBundle> bundles_[4];   // keep wave alive
    double wfRate_[4] = { 0.0, 0.0, 0.0, 0.0 };
    int64_t artTrack_[4] = { -1, -1, -1, -1 };
    double lastTick_ = 0.0, tickAcc_ = 0.0;
    int    tickN_ = 0;
    float  uiFps_ = 0.0f;
};

} // namespace onda::ui
