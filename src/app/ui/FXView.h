#pragma once

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "controls/Knob.h"
#include "controls/StateButton.h"
#include "core/fx/FXFactory.h"
#include "core/mixer/MixerController.h"
#include "ui/UiState.h"

namespace onda::ui {

/// One FX slot — type selector, ON, MIX knob, SYNC toggle, division
/// selector, three param knobs (P1..P3 native-normalized). Generic:
/// works for any FXKind in any rack target.
class FxSlotView : public juce::Component {
public:
    FxSlotView(const ThemeEngine& te, mixer::MixerController& mc,
               int target, int slot)
        : theme_(&te), t_(target), s_(slot),
          on_(te, "FX"), sync_(te, "SYNC"),
          mix_(te, "MIX", 0.0, 1.0, 0.0),
          p1_(te, "P1", 0.0, 1.0, 0.0),
          p2_(te, "P2", 0.0, 1.0, 0.0),
          p3_(te, "P3", 0.0, 1.0, 0.0) {
        using R = mixer::ParameterRegistry;
        // list generated from the engine's kind table — new FXKinds
        // appear here automatically
        for (int k = 0; k < (int)fx::FXKind::kCount; ++k) {
            juce::String nm = k == 0 ? "—"
                : juce::String(fx::fxKindName((fx::FXKind)k)).toUpperCase();
            type_.addItem(nm, k + 1);
        }
        type_.onChange = [this, &mc] {
            mc.setFxType(t_, s_, type_.getSelectedId() - 1);
        };
        on_.onInvoke = [this, &mc] {
            mc.fxEnabled(t_, s_,
                mc.value(R::fxEnabled(t_, s_)) < 0.5f);
        };
        sync_.onInvoke = [this, &mc] {
            mc.fxSyncMode(t_, s_,
                mc.value(R::fxSyncMode(t_, s_)) < 0.5f);
        };
        for (int d = 0; d < fx::beatDivCount(); ++d)
            div_.addItem(fx::beatDivName(d), d + 1);
        div_.setSelectedId(4, juce::dontSendNotification);
        div_.onChange = [this, &mc] {
            mc.fxDivision(t_, s_, div_.getSelectedId() - 1);
        };
        mix_.onValue = [&mc, this](double v) {
            mc.fxMix(t_, s_, (float)v);
        };
        p1_.onValue = [&mc, this](double v) {
            mc.fxParam(t_, s_, 0, (float)v);
        };
        p2_.onValue = [&mc, this](double v) {
            mc.fxParam(t_, s_, 1, (float)v);
        };
        p3_.onValue = [&mc, this](double v) {
            mc.fxParam(t_, s_, 2, (float)v);
        };
        for (auto* c : { (juce::Component*)&type_, (juce::Component*)&on_,
                         (juce::Component*)&sync_, (juce::Component*)&div_,
                         (juce::Component*)&mix_, (juce::Component*)&p1_,
                         (juce::Component*)&p2_, (juce::Component*)&p3_ })
            addAndMakeVisible(*c);
    }

    void setSnapshot(const FxSlotSnapshot& f) {
        const int wantId = f.kind + 1;
        if (type_.getSelectedId() != wantId)
            type_.setSelectedId(wantId, juce::dontSendNotification);
        on_.setState(f.enabled ? ControlState::On : ControlState::Off);
        sync_.setState(f.synced ? ControlState::On : ControlState::Off);
        const int wantDiv = juce::jlimit(1, fx::beatDivCount(),
                                         f.division + 1);
        if (div_.getSelectedId() != wantDiv)
            div_.setSelectedId(wantDiv, juce::dontSendNotification);
        auto pull = [](juce::Slider& sl, double v) {
            if (!sl.isMouseButtonDown()
                && std::abs(sl.getValue() - v) > 0.02)
                sl.setValue(v, juce::dontSendNotification);
        };
        pull(mix_.slider(), f.mix);
        pull(p1_.slider(), f.params[0]);
        pull(p2_.slider(), f.params[1]);
        pull(p3_.slider(), f.params[2]);
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        g.setColour(col(p.surface));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setColour(col(p.borderSoft));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f),
                               4.0f, 1.0f);
    }

    void resized() override {
        auto b = getLocalBounds().reduced(3);
        auto top = b.removeFromTop(20);
        on_.setBounds(top.removeFromLeft(30).reduced(1));
        type_.setBounds(top.removeFromLeft(
            juce::jmin(76, top.getWidth() / 2)).reduced(1));
        sync_.setBounds(top.removeFromLeft(38).reduced(1));
        div_.setBounds(top.reduced(1));
        auto row = b;
        const int kw = row.getWidth() / 4;
        mix_.setBounds(row.removeFromLeft(kw).reduced(1));
        p1_.setBounds(row.removeFromLeft(kw).reduced(1));
        p2_.setBounds(row.removeFromLeft(kw).reduced(1));
        p3_.setBounds(row.reduced(1));
    }

private:
    const ThemeEngine* theme_;
    int t_, s_;
    juce::ComboBox type_, div_;
    StateButton on_, sync_;
    Knob mix_, p1_, p2_, p3_;
};

/// FX strip — deck A insert rack, deck B insert rack, master rack.
/// Send racks get a compact pair of return-level knobs. The number of
/// slots per rack comes from the engine (kSlotsPerRack).
class FXView : public juce::Component {
public:
    FXView(const ThemeEngine& te, mixer::MixerController& mc)
        : theme_(&te),
          retA_(te, "RET A", 0.0, 1.0, 1.0),
          retB_(te, "RET B", 0.0, 1.0, 1.0) {
        struct R { int target; const char* name; };
        const R racks[] = {
            { (int)fx::FXTarget::DeckA, "DECK A" },
            { (int)fx::FXTarget::DeckB, "DECK B" },
            { (int)fx::FXTarget::SendA, "SEND A" },
            { (int)fx::FXTarget::SendB, "SEND B" },
            { (int)fx::FXTarget::Master, "MASTER" },
        };
        for (int r = 0; r < 5; ++r) {
            for (int s = 0; s < kSlots; ++s) {
                auto v = std::make_unique<FxSlotView>(te, mc,
                                                      racks[r].target, s);
                addAndMakeVisible(*v);
                slots_.push_back({ racks[r].target, std::move(v) });
            }
            rackNames_.push_back(racks[r].name);
        }
        retA_.onValue = [&mc](double v) { mc.fxReturn(0, (float)v); };
        retB_.onValue = [&mc](double v) { mc.fxReturn(1, (float)v); };
        addAndMakeVisible(retA_);
        addAndMakeVisible(retB_);
    }

    void setSnapshot(const UiSnapshot& s) {
        // slots_ order = racks × slots — map by construction order
        int idx = 0;
        const FxSlotSnapshot* byRack[5] = {
            s.fxDeck[0], s.fxDeck[1], s.fxSend[0], s.fxSend[1],
            s.fxMaster
        };
        for (int r = 0; r < 5; ++r)
            for (int sl = 0; sl < kSlots; ++sl)
                slots_[(size_t)idx++].view->setSnapshot(byRack[r][sl]);
        retA_.setValue(s.fxReturn[0]);
        retB_.setValue(s.fxReturn[1]);
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        g.setColour(col(p.background));
        g.fillRect(getLocalBounds());
        g.setColour(col(p.textFaint));
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        for (size_t r = 0; r < rackNames_.size() && r < rackX_.size(); ++r)
            g.drawText(rackNames_[r], rackX_[r], 2, 80, 11,
                       juce::Justification::left);
    }

    void resized() override {
        auto b = getLocalBounds().reduced(3);
        b.removeFromTop(12); // rack label strip
        rackX_.clear();
        const int groups = 5;
        const int retsW = 96;
        auto rets = b.removeFromRight(retsW);
        const int gw = b.getWidth() / groups;
        int idx = 0;
        for (int r = 0; r < groups; ++r) {
            auto gr = b.removeFromLeft(gw).reduced(2);
            rackX_.push_back(gr.getX());
            const int sw = gr.getWidth() / kSlots;
            for (int s = 0; s < kSlots; ++s)
                slots_[(size_t)idx++].view->setBounds(
                    gr.removeFromLeft(sw).reduced(1));
        }
        retA_.setBounds(rets.removeFromLeft(retsW / 2).reduced(1));
        retB_.setBounds(rets.reduced(1));
    }

private:
    static constexpr int kSlots = 4;
    struct SlotRef { int target; std::unique_ptr<FxSlotView> view; };

    const ThemeEngine* theme_;
    std::vector<SlotRef> slots_;
    std::vector<std::string> rackNames_;
    std::vector<int> rackX_;
    Knob retA_, retB_;
};

} // namespace onda::ui
