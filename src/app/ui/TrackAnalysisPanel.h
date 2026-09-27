#pragma once

#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "core/analysis/AnalysisService.h"

namespace onda::ui {

/// §55 — read-only per-track analysis panel. Shows what the
/// pipeline measured plus how trustworthy each result is:
///   BPM + confidence · BeatGrid status (+needsReview flag)
///   Key (standard + Camelot) + confidence
///   Waveform status · Loudness (LUFS/peak/true-peak)
///   Energy score · analysis version · per-component states
///
/// The view pulls a snapshot from AnalysisService when the host
/// calls setPath()/refresh() — it never runs analysis and never
/// blocks the message thread with decode work.
class TrackAnalysisPanel : public juce::Component,
                           private juce::Timer {
public:
    explicit TrackAnalysisPanel(const ThemeEngine& te,
                                AnalysisService* svc = nullptr)
        : theme_(&te), svc_(svc) {
        startTimerHz(4);   // cheap refresh while a job runs
    }
    ~TrackAnalysisPanel() override { stopTimer(); }

    void bind(AnalysisService* svc) { svc_ = svc; }

    void setPath(const std::string& path) {
        if (path_ == path) return;
        path_ = path;
        refresh();
    }

    void refresh() {
        if (svc_ == nullptr || path_.empty()) { snap_ = Snap{}; }
        else {
            Snap s;
            s.state = svc_->stateFor(path_);
            if (auto b = svc_->getAnalysis(path_)) {
                s.bpm        = svc_->getBpm(path_);
                s.bpmConf    = b->result.bpm.confidence;
                s.gridOk     = !b->effectiveGrid().empty();
                s.review     = b->result.needsReview;
                s.keyText    = b->result.key.standard;
                s.camelot    = b->result.key.camelot;
                s.keyConf    = b->result.key.confidence;
                s.lufs       = b->result.loudness.integratedLufs;
                s.peak       = b->result.loudness.peak;
                s.truePeak   = b->result.loudness.truePeak;
                s.loudValid  = b->result.loudness.valid;
                s.energy     = b->result.energy.score;
                s.centroid   = b->result.spectral.centroidHz;
                for (int c = 0; c < kNumComponents; ++c)
                    s.comp[c] = b->result.components[c].state;
                s.version    = b->version;
                s.hasData    = true;
            }
            snap_ = s;
        }
        repaint();
    }

    void timerCallback() override {
        if (svc_ != nullptr && !path_.empty() &&
            snap_.state == AnalysisState::Analyzing)
            refresh();
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        g.fillAll(col(p.background));
        auto area = getLocalBounds().reduced(8);
        g.setColour(col(p.text));
        g.setFont(15.0f);
        g.drawText("Track Analysis", area.removeFromTop(22),
                   juce::Justification::centredLeft);
        if (path_.empty() || svc_ == nullptr) {
            g.setColour(col(p.textDim));
            g.drawText("no track selected", area,
                       juce::Justification::centredLeft);
            return;
        }
        auto row = [&](const juce::String& k, const juce::String& v) {
            auto r = area.removeFromTop(20);
            g.setColour(col(p.textDim));
            g.drawText(k, r.removeFromLeft(120),
                       juce::Justification::centredLeft);
            g.setColour(col(p.text));
            g.drawText(v, r, juce::Justification::centredLeft);
        };
        row("State", stateText(snap_.state));
        if (!snap_.hasData) return;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.2f  (conf %.0f%%)",
                      snap_.bpm, snap_.bpmConf * 100.0f);
        row("BPM", buf);
        row("BeatGrid", snap_.gridOk
            ? (snap_.review ? "ready — review flagged" : "ready")
            : "none");
        juce::String key = snap_.keyText.isEmpty() ? "-" : snap_.keyText;
        if (snap_.camelot.isNotEmpty()) key += "  (" + snap_.camelot + ")";
        std::snprintf(buf, sizeof buf, "%s  (conf %.0f%%)",
                      key.toRawUTF8(), snap_.keyConf * 100.0f);
        row("Key", buf);
        row("Waveform",
            snap_.comp[(int)AnalysisComponent::Waveform]
                == (int)AnalysisState::Analyzed ? "ready" : "pending");
        if (snap_.loudValid) {
            std::snprintf(buf, sizeof buf,
                          "%.1f LUFS  peak %.2f  true-peak %.2f",
                          snap_.lufs, snap_.peak, snap_.truePeak);
        } else std::snprintf(buf, sizeof buf, "-");
        row("Loudness", buf);
        std::snprintf(buf, sizeof buf,
                      snap_.energy >= 0.0f ? "%.2f" : "-", snap_.energy);
        row("Energy", buf);
        if (snap_.centroid > 0.0f) {
            std::snprintf(buf, sizeof buf, "%.0f Hz", snap_.centroid);
            row("Centroid", buf);
        }
        std::snprintf(buf, sizeof buf, "v%d", snap_.version);
        row("Analysis ver.", buf);
    }

private:
    struct Snap {
        AnalysisState state { AnalysisState::NotAnalyzed };
        bool   hasData { false };
        double bpm { 0.0 };
        float  bpmConf { 0.0f };
        bool   gridOk { false }, review { false };
        juce::String keyText, camelot;
        float  keyConf { 0.0f };
        float  lufs { -70.0f }, peak { 0.0f }, truePeak { 0.0f };
        bool   loudValid { false };
        float  energy { -1.0f }, centroid { -1.0f };
        int    comp[kNumComponents] {};
        int    version { kAnalysisVersion };
    };

    static juce::String stateText(AnalysisState s) {
        switch (s) {
            case AnalysisState::NotAnalyzed: return "not analyzed";
            case AnalysisState::Analyzing:   return "analyzing…";
            case AnalysisState::Analyzed:    return "analyzed";
            case AnalysisState::Failed:      return "failed";
            case AnalysisState::Outdated:    return "outdated";
            default:                          return "?";
        }
    }

    const ThemeEngine* theme_;
    AnalysisService*   svc_ { nullptr };
    std::string        path_;
    Snap               snap_;
};

} // namespace onda::ui
