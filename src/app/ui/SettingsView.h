#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "OndaTheme.h"
#include "core/adevice/AudioDeviceEngine.h"
#include "core/library/MusicLibraryEngine.h"
#include "platform/config/Config.h"
#include "ui/CommandManager.h"
#include "ui/ShortcutManager.h"

namespace onda::ui {

/// Settings overlay — sectioned left rail + content pages.
/// Sections: Audio, Appearance, Shortcuts, Library, Controllers
/// (foundation), Diagnostics. Device changes go through
/// AudioDeviceSelectorComponent (proven JUCE path), never raw
/// deviceManager calls scattered through widgets.
class SettingsView : public juce::Component, private juce::Timer {
public:
    SettingsView(const ThemeEngine& te, CommandManager& cm,
                 ShortcutManager& shortcuts,
                 juce::AudioDeviceManager& deviceManager,
                 Config& config,
                 library::MusicLibraryEngine& lib,
                 adevice::AudioDeviceEngine* adev = nullptr)
        : theme_(&te), cmds_(&cm), shortcuts_(&shortcuts),
          config_(&config), lib_(&lib), adev_(adev) {
        rail_.setModel(&railModel_);
        rail_.setRowHeight(28);
        addAndMakeVisible(rail_);

        // ---- audio section ----
        deviceSel_ = std::make_unique<juce::AudioDeviceSelectorComponent>(
            deviceManager, 0, 2, 2, 8, false, false, true, true);
        addChildComponent(*deviceSel_);

        // ---- Phase 13 device panel (below the JUCE selector) ----
        latencyBox_.addItemList(
            { "Low latency", "Balanced", "Stable" }, 1);
        latencyBox_.setSelectedId(2, juce::dontSendNotification);
        latencyBox_.onChange = [this] {
            if (!adev_) return;
            adevice::DeviceSettings s = adev_->settings();
            s.pref = (adevice::LatencyPref)(latencyBox_.getSelectedId()-1);
            s.bufferSize = 0;   // let the preset pick
            const auto err = adev_->select(s);
            if (!err.empty()) audioInfo_.setText(
                "APPLY FAILED: " + juce::String(err),
                juce::dontSendNotification);
        };
        addChildComponent(latencyBox_);
        testSigBox_.addItem("Signal: off", 1);
        for (int i = 1; i <= 7; ++i)
            testSigBox_.addItem("Signal: " + juce::String(
                adevice::testSignalName((adevice::TestSignal)i)), i + 1);
        testSigBox_.setSelectedId(1, juce::dontSendNotification);
        testSigBox_.onChange = [this] {
            if (adev_)
                adev_->setTestSignal(
                    (adevice::TestSignal)(testSigBox_.getSelectedId()-1));
        };
        addChildComponent(testSigBox_);
        copyDiag_.setButtonText("COPY DIAGNOSTICS");
        copyDiag_.onClick = [this] {
            if (adev_)
                juce::SystemClipboard::copyTextToClipboard(
                    adev_->diagnostics());
        };
        addChildComponent(copyDiag_);
        audioInfo_.setJustificationType(juce::Justification::topLeft);
        audioInfo_.setColour(juce::Label::textColourId,
                             col(theme_->theme().palette.textDim));
        addChildComponent(audioInfo_);
        if (adev_) startTimerHz(2);

        // ---- appearance section ----
        themeBox_.addItemList({ "Dark", "Dark (high contrast)" }, 1);
        themeBox_.setSelectedId(1, juce::dontSendNotification);
        themeBox_.onChange = [this] {
            if (onTheme) onTheme(themeBox_.getSelectedId() - 1);
        };
        addChildComponent(themeBox_);
        densityBox_.addItemList({ "Compact", "Comfortable", "Expanded" },
                                1);
        densityBox_.setSelectedId(2, juce::dontSendNotification);
        densityBox_.onChange = [this] {
            if (onDensity) onDensity(densityBox_.getSelectedId() - 1);
        };
        addChildComponent(densityBox_);

        // ---- library section ----
        libInfo_.setJustificationType(juce::Justification::topLeft);
        libInfo_.setColour(juce::Label::textColourId,
                           col(theme_->theme().palette.textDim));
        addChildComponent(libInfo_);

        // ---- shortcuts section (read-only table for now) ----
        shortcutList_.setModel(&scModel_);
        shortcutList_.setRowHeight(20);
        shortcutList_.setColour(juce::ListBox::backgroundColourId,
                                col(theme_->theme().palette.surfaceSunk));
        addChildComponent(shortcutList_);

        // ---- controllers section (foundation) ----
        ctrlInfo_.setJustificationType(juce::Justification::topLeft);
        ctrlInfo_.setColour(juce::Label::textColourId,
                            col(theme_->theme().palette.textDim));
        ctrlInfo_.setText(
            "CONTROLLER SURFACES\n\n"
            "No controllers configured yet.\n\n"
            "The command layer already accepts input from MIDI / HID:\n"
            "a mapping table will route controller events to the same\n"
            "CommandManager the keyboard uses — no duplicate logic.\n\n"
            "Planned: Pioneer DJ DDJ-FLX4 profile (Phase 9).",
            juce::dontSendNotification);
        addChildComponent(ctrlInfo_);

        // ---- diagnostics section ----
        diag_.setJustificationType(juce::Justification::topLeft);
        diag_.setColour(juce::Label::textColourId,
                        col(theme_->theme().palette.textDim));
        addChildComponent(diag_);

        select(0);
    }

    std::function<void(int)> onTheme;
    std::function<void(int)> onDensity;
    std::function<void()>    onClose;

    void select(int section) {
        section_ = section;
        rail_.selectRow(section);
        for (auto* c : { (juce::Component*)deviceSel_.get(),
                         (juce::Component*)&themeBox_,
                         (juce::Component*)&densityBox_,
                         (juce::Component*)&libInfo_,
                         (juce::Component*)&shortcutList_,
                         (juce::Component*)&ctrlInfo_,
                         (juce::Component*)&diag_,
                         (juce::Component*)&latencyBox_,
                         (juce::Component*)&testSigBox_,
                         (juce::Component*)&copyDiag_,
                         (juce::Component*)&audioInfo_ })
            c->setVisible(false);
        auto show = [&](juce::Component* c) {
            c->setVisible(true); c->setBounds(contentBounds());
        };
        switch (section) {
            case 0: {
                auto b = contentBounds();
                const bool panel = adev_ != nullptr;
                deviceSel_->setVisible(true);
                deviceSel_->setBounds(b.removeFromTop(
                    panel ? (int)(b.getHeight() * 0.62)
                          : b.getHeight()));
                if (panel) {
                    b.removeFromTop(8);
                    auto row = b.removeFromTop(24);
                    latencyBox_.setBounds(row.removeFromLeft(150));
                    row.removeFromLeft(8);
                    testSigBox_.setBounds(row.removeFromLeft(170));
                    row.removeFromLeft(8);
                    copyDiag_.setBounds(row.removeFromLeft(160));
                    latencyBox_.setVisible(true);
                    testSigBox_.setVisible(true);
                    copyDiag_.setVisible(true);
                    audioInfo_.setBounds(b.withTrimmedTop(6));
                    audioInfo_.setVisible(true);
                    refreshAudioInfo();
                }
                break;
            }
            case 1:
                themeBox_.setVisible(true);
                densityBox_.setVisible(true);
                themeBox_.setBounds(contentBounds().removeFromTop(26)
                                        .removeFromLeft(220));
                densityBox_.setBounds(
                    contentBounds().withTrimmedTop(30)
                        .removeFromTop(26).removeFromLeft(220));
                break;
            case 2:
                scModel_.reload();
                shortcutList_.setVisible(true);
                shortcutList_.setBounds(contentBounds());
                break;
            case 3:
                libInfo_.setText(
                    "LIBRARY\n\nTracks: " +
                        juce::String(lib_->trackCount()) +
                    "\nDatabase: live\n\n"
                    "Scan roots are managed from the browser panel;\n"
                    "watch/fingerprint checks run on the library worker.",
                    juce::dontSendNotification);
                show(&libInfo_);
                break;
            case 4: show(&ctrlInfo_); break;
            case 5: show(&diag_); break;
            default: break;
        }
        repaint();
    }

    void setDiag(const juce::String& text) {
        diag_.setText(text, juce::dontSendNotification);
    }

    /// 2 Hz — keep the audio-panel readout live while section 0 shows.
    void timerCallback() override {
        if (section_ == 0 && isShowing()) refreshAudioInfo();
    }
    void refreshAudioInfo() {
        if (!adev_) return;
        juce::String t = juce::String(adev_->diagnostics());
        if (const auto* f = adev_->flx4())
            t += "DDJ-FLX4: " + juce::String(f->name) +
                 " detected\n";
        audioInfo_.setText(t, juce::dontSendNotification);
    }

    juce::Rectangle<int> contentBounds() const {
        auto b = getLocalBounds().reduced(16);
        b.removeFromTop(28);
        return b.withTrimmedLeft(railW_ + 12);
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        g.setColour(col(p.surface).withAlpha(0.98f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(col(p.border));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f),
                               8.0f, 1.0f);
        g.setColour(col(p.text));
        g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        g.drawText("SETTINGS", getLocalBounds().reduced(16)
                               .removeFromTop(26),
                   juce::Justification::centredLeft);
    }

    void resized() override {
        auto b = getLocalBounds().reduced(16);
        b.removeFromTop(28);
        rail_.setBounds(b.removeFromLeft(railW_).withTrimmedTop(2));
        for (auto* c : { (juce::Component*)deviceSel_.get(),
                         (juce::Component*)&libInfo_,
                         (juce::Component*)&shortcutList_,
                         (juce::Component*)&ctrlInfo_,
                         (juce::Component*)&diag_ })
            c->setBounds(contentBounds());
        select(section_);   // re-lay out the visible section
    }

    bool keyPressed(const juce::KeyPress&) override {
        if (onClose) { onClose(); return true; }
        return false;
    }

private:
    static constexpr int kSections = 6;
    const int railW_ = 150;

    struct RailModel : juce::ListBoxModel {
        explicit RailModel(SettingsView& o) : o_(o) {}
        int getNumRows() override { return kSections; }
        void paintListBoxItem(int row, juce::Graphics& g,
                              int w, int h, bool sel) override {
            static const char* kNames[kSections] = {
                "AUDIO", "APPEARANCE", "SHORTCUTS",
                "LIBRARY", "CONTROLLERS", "DIAGNOSTICS"
            };
            const Palette& p = o_.theme_->theme().palette;
            if (sel || row == o_.section_) {
                g.setColour(col(p.selection));
                g.fillRect(0, 0, w, h);
            }
            g.setColour(col(row == o_.section_ ? p.text : p.textDim));
            g.setFont(juce::Font(juce::FontOptions(13.0f)));
            g.drawText(kNames[row], 10, 0, w - 10, h,
                       juce::Justification::centredLeft);
        }
        void listBoxItemClicked(int row,
                                const juce::MouseEvent&) override {
            o_.select(row);
        }
        SettingsView& o_;
    };

    struct ShortcutModel : juce::ListBoxModel {
        explicit ShortcutModel(SettingsView& o) : o_(o) { reload(); }
        void reload() { rows_ = o_.shortcuts_->entries(); }
        int getNumRows() override { return (int)rows_.size(); }
        void paintListBoxItem(int row, juce::Graphics& g,
                              int w, int h, bool) override {
            if (row < 0 || row >= (int)rows_.size()) return;
            const Palette& p = o_.theme_->theme().palette;
            const Command* c = o_.cmds_->find(rows_[(size_t)row].second);
            g.setColour(col(row & 1 ? p.rowAlt : p.surfaceSunk));
            g.fillRect(0, 0, w, h);
            g.setColour(col(p.textDim));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText(formatKey(rows_[(size_t)row].first),
                       8, 0, 110, h, juce::Justification::centredLeft);
            g.setColour(col(p.text));
            g.drawText(c ? c->name : "?",
                       126, 0, w - 130, h,
                       juce::Justification::centredLeft);
        }
        std::vector<std::pair<KeyDesc, uint32_t>> rows_;
        SettingsView& o_;
    };

    const ThemeEngine* theme_;
    CommandManager*    cmds_;
    ShortcutManager*   shortcuts_;
    Config*            config_;
    library::MusicLibraryEngine* lib_;

    RailModel     railModel_{ *this };
    ShortcutModel scModel_{ *this };
    juce::ListBox rail_{ "settingsRail", nullptr },
                  shortcutList_{ "shortcuts", nullptr };
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSel_;
    juce::ComboBox themeBox_, densityBox_;
    juce::Label libInfo_, ctrlInfo_, diag_;
    // Phase 13 device panel
    adevice::AudioDeviceEngine* adev_ = nullptr;
    juce::ComboBox   latencyBox_, testSigBox_;
    juce::TextButton copyDiag_;
    juce::Label      audioInfo_;
    int section_ = 0;
};

} // namespace onda::ui
