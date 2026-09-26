#pragma once

#include <array>
#include <cstdint>

#include "core/fx/FXMacro.h"
#include "core/fx/FXPresetManager.h"
#include "core/fx/FXTypes.h"
#include "core/mixer/MixerController.h"

namespace onda::ui {

/// Control-side FX surface (§6/§28/§30): the single address space
/// every input source talks to — UI knobs, keyboard, MIDI, HID and
/// future control surfaces. Holds the *focus* (target + chain slot)
/// plus an 8-macro bank and preset navigation state; every action is
/// resolved into MixerController calls, which sanitize and enqueue.
/// Nothing here touches the audio thread directly.
class FXSurface {
public:
    static constexpr int kMacros = 8;

    void bind(mixer::MixerController* c, fx::FXPresetManager* pm) noexcept {
        ctl_ = c; presets_ = pm;
    }

    // ---- focus ----
    void setFocus(int target, int slot) noexcept {
        focusT_ = target < 0 ? 0
                : target >= (int)fx::FXTarget::kCount
                    ? (int)fx::FXTarget::kCount - 1 : target;
        focusS_ = slot & 3;
    }
    int focusTarget() const noexcept { return focusT_; }
    int focusSlot()   const noexcept { return focusS_; }

    // ---- slot ops on the focus ----
    void toggleEnabled() {
        if (!ctl_) return;
        const float cur = ctl_->value(reg::fxEnabled(focusT_, focusS_));
        ctl_->fxEnabled(focusT_, focusS_, cur < 0.5f);
    }
    void setEnabled(bool on) {
        if (ctl_) ctl_->fxEnabled(focusT_, focusS_, on);
    }
    void setMix(float v) {
        if (ctl_) ctl_->fxMix(focusT_, focusS_, v);
    }
    void toggleSync() {
        if (!ctl_) return;
        const float cur = ctl_->value(reg::fxSyncMode(focusT_, focusS_));
        ctl_->fxSyncMode(focusT_, focusS_, cur < 0.5f);
    }
    void setType(int kind) {
        if (ctl_) ctl_->setFxType(focusT_, focusS_, kind);
    }
    /// Cycle effect kind (skips None + kCount bounds).
    void typeStep(int dir) {
        if (!ctl_) return;
        const int cur = (int)ctl_->value(reg::fxType(focusT_, focusS_));
        int k = cur + (dir >= 0 ? 1 : -1);
        if (k >= (int)fx::FXKind::kCount) k = (int)fx::FXKind::None;
        if (k < 0) k = (int)fx::FXKind::kCount - 1;
        ctl_->setFxType(focusT_, focusS_, k);
    }
    void setDivision(int d) {
        if (ctl_) ctl_->fxDivision(focusT_, focusS_, d);
    }
    void divisionStep(int dir) {
        if (!ctl_) return;
        const int cur =
            (int)ctl_->value(reg::fxDivision(focusT_, focusS_));
        int d = cur + (dir >= 0 ? 1 : -1);
        if (d >= fx::beatDivCount()) d = 0;
        if (d < 0) d = fx::beatDivCount() - 1;
        ctl_->fxDivision(focusT_, focusS_, d);
    }
    void setParam(int p, float v) {
        if (ctl_) ctl_->fxParam(focusT_, focusS_, p, v);
    }
    void nudgeParam(int p, float dv) {
        if (!ctl_) return;
        const float cur = ctl_->value(reg::fxParam(focusT_, focusS_, p));
        ctl_->fxParam(focusT_, focusS_, p, cur + dv);
    }
    /// Move focus slot one position left/right in the chain and keep
    /// focus on the moved content (§7).
    void moveSlot(int dir) {
        if (!ctl_) return;
        const int to = dir >= 0 ? (focusS_ + 1) & 3
                                : (focusS_ + 3) & 3;
        ctl_->moveFxSlot(focusT_, focusS_, to);
        focusS_ = to;
    }

    // ---- presets ----
    int presetCount() const noexcept {
        return presets_ ? presets_->count() : 0;
    }
    int presetSel() const noexcept { return presetSel_; }
    void presetStep(int dir) {
        const int n = presetCount();
        if (n <= 0) return;
        presetSel_ = (presetSel_ + (dir >= 0 ? 1 : n - 1)) % n;
    }
    /// Apply preset i (or the current selection when i < 0) to the
    /// focus slot — control thread only.
    void applyPreset(int i) {
        if (!ctl_ || !presets_) return;
        const int idx = i >= 0 ? i : presetSel_;
        if (idx < 0 || idx >= presets_->count()) return;
        presetSel_ = idx;
        ctl_->applyFxPreset(focusT_, focusS_, presets_->at(idx));
    }

    // ---- macros ----
    fx::FXMacro& macro(int i) noexcept {
        return macros_[(size_t)(i & (kMacros - 1))];
    }
    /// Set macro i's value and push all resolved writes.
    int macroSet(int i, float v) {
        if (!ctl_) return 0;
        auto& m = macros_[(size_t)(i & (kMacros - 1))];
        m.setValue(v);
        return ctl_->applyFxMacro(m, focusT_);
    }

    /// Stock macro bank — targets are kFocusTarget so the same bank
    /// drives whichever rack is focused. Slot params (Mix/Enabled)
    /// and processor params (P1..P8) are both addressable.
    void seedDefaultMacros() noexcept {
        using M = fx::FXMacro;
        using K = fx::FXSlotKind;
        auto mkMap = [](int slot, int param, float a, float b,
                        int curve = 0) {
            return M::Mapping{ M::kFocusTarget, slot, param, a, b, curve };
        };

        macros_[0].name = "Build";        // open everything up
        macros_[0].clear();
        macros_[0].addMapping(mkMap(0, (int)K::Mix, 0.0f, 1.0f));
        macros_[0].addMapping(mkMap(0, (int)K::P1,  0.2f, 1.0f));
        macros_[0].addMapping(mkMap(1, (int)K::Mix, 0.0f, 0.8f));
        macros_[0].addMapping(mkMap(1, (int)K::P1,  0.0f, 0.8f));
        macros_[0].addMapping(mkMap(2, (int)K::Mix, 0.0f, 0.6f));

        macros_[1].name = "Wash";         // drench in the wet path
        macros_[1].clear();
        macros_[1].addMapping(mkMap(0, (int)K::Mix, 0.5f, 1.0f));
        macros_[1].addMapping(mkMap(1, (int)K::Mix, 0.3f, 1.0f));
        macros_[1].addMapping(mkMap(2, (int)K::Mix, 0.3f, 1.0f));

        macros_[2].name = "Crush";        // pull P1 down hard
        macros_[2].clear();
        macros_[2].addMapping(mkMap(0, (int)K::P1, 1.0f, 0.1f));
        macros_[2].addMapping(mkMap(0, (int)K::Mix, 0.3f, 1.0f));

        macros_[3].name = "Echo Out";     // feedback swells as mix opens
        macros_[3].clear();
        macros_[3].addMapping(mkMap(0, (int)K::Mix, 0.0f, 0.9f));
        macros_[3].addMapping(mkMap(0, (int)K::P1,  0.2f, 0.95f,
                                    (int)M::Curve::Sqrt));

        for (int i = 4; i < kMacros; ++i) macros_[(size_t)i].clear();
    }

private:
    using reg = mixer::ParameterRegistry;

    mixer::MixerController* ctl_ { nullptr };
    fx::FXPresetManager*    presets_ { nullptr };
    std::array<fx::FXMacro, kMacros> macros_ {};
    int focusT_ { (int)fx::FXTarget::DeckA };
    int focusS_ { 0 };
    int presetSel_ { 0 };
};

} // namespace onda::ui
