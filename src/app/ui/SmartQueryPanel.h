#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "core/smart/SmartLibraryEngine.h"
#include "core/smart/SmartQueryValidator.h"

namespace onda::ui {

/// Phase 23 §46-48 — visual rule editor for smart queries.
/// Field | Operator | Value rows map 1:1 onto QueryBuilder → the
/// preview count and the saved playlist evaluate the SAME rules —
/// the visual model is the stored model.
///
/// The panel never writes anything until Save; Preview is a dry run.
class SmartQueryPanel : public juce::Component {
public:
    SmartQueryPanel(const ThemeEngine& te,
                    smart::SmartLibraryEngine& smart)
        : theme_(&te), smart_(&smart) {
        // field/operator lists — subset that covers the real columns
        static const char* kFields[] = {
            "bpm", "key_index", "key_mode", "keyconf", "genre",
            "artist", "album", "title", "rating", "year", "duration",
            "play_count", "date_added", "energy_score", "tags",
            "autotag", "unplayed", "analyzed",
        };
        static const char* kOps[] = {
            "eq", "neq", "gt", "gte", "lt", "lte", "between",
            "contains", "not_contains", "starts_with", "ends_with",
            "in", "not_in", "isnull", "isnotnull",
        };
        for (auto* f : kFields) fields_.add(juce::String(f));
        for (auto* o : kOps)    ops_.add(juce::String(o));

        matchAll_.setButtonText("ALL (AND)");
        matchAny_.setButtonText("ANY (OR)");
        matchAll_.setRadioGroupId(9101);
        matchAny_.setRadioGroupId(9101);
        matchAll_.setToggleState(true, juce::dontSendNotification);
        matchAll_.onClick = [this] { refreshPreview(); };
        matchAny_.onClick = [this] { refreshPreview(); };
        addAndMakeVisible(matchAll_);
        addAndMakeVisible(matchAny_);

        addRule_.setButtonText("+ rule");
        addRule_.onClick = [this] { addRow(); };
        addAndMakeVisible(addRule_);
        addGroup_.setButtonText("+ group (OR)");
        addGroup_.onClick = [this] { addRow(true); };
        addAndMakeVisible(addGroup_);

        savePl_.setButtonText("save playlist");
        savePl_.onClick = [this] { save(false); };
        addAndMakeVisible(savePl_);
        saveCrate_.setButtonText("save crate");
        saveCrate_.onClick = [this] { save(true); };
        addAndMakeVisible(saveCrate_);

        name_.setTextToShowWhenEmpty("query name…",
            col(theme_->theme().palette.textFaint));
        name_.onTextChange = [this] {};
        addAndMakeVisible(name_);

        addRow();   // start with one empty rule
    }

    std::function<void()> onSaved;      // notify rail refresh

    void setVisible(bool v) override {
        juce::Component::setVisible(v);
        if (v) refreshPreview();
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        const Metrics m = theme_->metrics();
        g.setColour(col(p.surface));
        g.fillRect(getLocalBounds());
        g.setColour(col(p.borderSoft));
        g.drawRect(getLocalBounds());
        g.setColour(col(p.textFaint));
        g.setFont(juce::Font(juce::FontOptions(
            (float)m.fontSm, juce::Font::bold)));
        g.drawText("SMART QUERY", 10, 8, 160, 16,
                   juce::Justification::centredLeft);
        g.setColour(col(p.text));
        g.drawText("match:", 10, 30, 50, 20,
                   juce::Justification::centredLeft);
        // preview line
        g.setFont(juce::Font(juce::FontOptions((float)m.fontSm)));
        g.setColour(col(p.accent));
        g.drawText(previewText_, 10, getHeight() - 62,
                   getWidth() - 20, 18,
                   juce::Justification::centredLeft);
        g.setColour(col(p.textDim));
        g.drawText(statusText_, 10, getHeight() - 42,
                   getWidth() - 20, 18,
                   juce::Justification::centredLeft);
    }

    void resized() override {
        auto b = getLocalBounds().reduced(10);
        b.removeFromTop(24);
        auto match = b.removeFromTop(22);
        match.removeFromLeft(52);
        matchAll_.setBounds(match.removeFromLeft(96));
        matchAny_.setBounds(match.removeFromLeft(96));
        b.removeFromTop(4);

        for (auto& rp : rows_) {
            auto& r = *rp;
            auto line = b.removeFromTop(24);
            r.group.setBounds(line.removeFromLeft(56));
            r.field.setBounds(line.removeFromLeft(120).reduced(0, 1));
            r.op.setBounds(line.removeFromLeft(120).reduced(0, 1));
            r.value.setBounds(line.removeFromLeft(
                juce::jmax(60, line.getWidth() - 56)).reduced(0, 1));
            r.remove.setBounds(line.removeFromRight(24));
        }

        auto ctl = b.removeFromBottom(24);
        auto nm = ctl.removeFromLeft(juce::jmin(160, ctl.getWidth() / 3));
        name_.setBounds(nm.reduced(0, 2));
        savePl_.setBounds(ctl.removeFromLeft(96).reduced(2));
        saveCrate_.setBounds(ctl.removeFromLeft(90).reduced(2));

        auto add = b.removeFromBottom(24);
        addRule_.setBounds(add.removeFromLeft(70));
        addGroup_.setBounds(add.removeFromLeft(110));
        b.removeFromBottom(18);   // status line
        b.removeFromBottom(20);   // preview line
    }

private:
    struct RuleRow {
        juce::ComboBox group, field, op;
        juce::TextEditor value;
        juce::TextButton remove{ "×" };
        bool isGroup { false };
    };

    void addRow(bool grouped = false) {
        if (rows_.size() >= 8) return;
        rows_.push_back(std::make_unique<RuleRow>());
        auto& r = *rows_.back();
        for (const auto& s : fields_) r.field.addItem(s, r.field.getNumItems() + 1);
        for (const auto& s : ops_)    r.op.addItem(s, r.op.getNumItems() + 1);
        r.field.setSelectedId(1);
        r.op.setSelectedId(1);
        r.group.addItem(grouped ? "group" : "top", 1);
        r.group.setSelectedId(1);
        r.value.setTextToShowWhenEmpty("value…",
            col(theme_->theme().palette.textFaint));
        r.isGroup = grouped;
        auto refresh = [this] { refreshPreview(); };
        r.field.onChange = refresh; r.op.onChange = refresh;
        r.value.onTextChange = refresh;
        r.remove.onClick = [this, i = (int)rows_.size() - 1] {
            removeRow(i);
        };
        addAndMakeVisible(r.group); addAndMakeVisible(r.field);
        addAndMakeVisible(r.op);    addAndMakeVisible(r.value);
        addAndMakeVisible(r.remove);
        resized();
        refreshPreview();
    }

    void removeRow(int idx) {
        if (idx < 0 || idx >= (int)rows_.size() || rows_.size() <= 1)
            return;
        rows_.erase(rows_.begin() + idx);
        // rebind remove buttons to new indexes
        for (size_t i = 0; i < rows_.size(); ++i)
            rows_[i]->remove.onClick = [this, k = (int)i] {
                removeRow(k);
            };
        resized();
        refreshPreview();
    }

    library::SmartRules build() {
        smart::QueryBuilder qb;
        qb.setMatchAny(matchAny_.getToggleState());
        groupIdx_ = -1;
        for (const auto& rp : rows_) {
            const auto& r = *rp;
            library::SmartRule sr;
            sr.field = r.field.getText().toStdString();
            sr.op    = r.op.getText().toStdString();
            sr.text  = r.value.getText().toStdString();
            sr.a     = sr.text.empty() ? 0.0
                       : std::strtod(sr.text.c_str(), nullptr);
            if (sr.op == "between") {
                const auto dash = sr.text.find('-');
                if (dash != std::string::npos) {
                    sr.a = std::strtod(
                        sr.text.substr(0, dash).c_str(), nullptr);
                    sr.b = std::strtod(
                        sr.text.substr(dash + 1).c_str(), nullptr);
                }
            }
            if (r.isGroup) {
                if (groupIdx_ < 0) groupIdx_ = qb.addGroup(true);
                qb.addRuleToGroup(groupIdx_, sr);
            } else {
                qb.addRule(sr);
            }
        }
        return qb.rules();
    }

    void refreshPreview() {
        const auto rules = build();
        const auto issues = smart_->validate(rules);
        if (!issues.empty()
            && !(issues.size() == 1 && issues[0].field.empty())) {
            statusText_ = issues[0].message;
            previewText_.clear();
        } else {
            const int n = smart_->previewCount(rules);
            previewText_ = std::to_string(n) + " tracks match";
            statusText_ = issues.empty() ? "" : issues[0].message;
        }
        repaint();
    }

    void save(bool crate) {
        const auto rules = build();
        const auto issues = smart_->validate(rules);
        if (!issues.empty()
            && !(issues.size() == 1 && issues[0].field.empty())) {
            statusText_ = "can't save: " + issues[0].message;
            repaint();
            return;
        }
        std::string name = name_.getText().toStdString();
        if (name.empty()) name = "Smart Query";
        if (crate) smart_->saveAsCrate(name, rules);
        else       smart_->saveAsPlaylist(name, rules);
        statusText_ = "saved '" + name + "'";
        repaint();
        if (onSaved) onSaved();
    }

    const ThemeEngine* theme_;
    smart::SmartLibraryEngine* smart_;
    juce::StringArray fields_, ops_;
    std::vector<std::unique_ptr<RuleRow>> rows_;
    int groupIdx_ { -1 };
    juce::ToggleButton matchAll_, matchAny_;
    juce::TextButton addRule_, addGroup_, savePl_, saveCrate_;
    juce::TextEditor name_;
    std::string previewText_, statusText_;
};

} // namespace onda::ui
