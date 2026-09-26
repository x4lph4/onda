#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "core/library/MusicLibraryEngine.h"
#include "ui/CommandManager.h"
#include "ui/TrackTableModel.h"

namespace onda::ui {

/// Browser panel: search field, left rail (sources + playlists/crates),
/// virtualized track table, column header. Drag source: rows expose
/// the track path for deck/playlist targets.
///
/// Virtualization: juce::ListBox paints only visible rows; the model
/// fetches kPage-row pages — a 100k library costs like a 1k one.
class BrowserView : public juce::Component {
public:
    BrowserView(const ThemeEngine& te, CommandManager& cm,
                library::MusicLibraryEngine& lib)
        : theme_(&te), cmds_(&cm), lib_(&lib),
          railModel_(*this), tableModel_(*this) {
        model_.setDefaultColumns();
        applyLibrarySource();

        search_.setTextToShowWhenEmpty("search — title, artist, "
                                       "album, tag…",
            col(theme_->theme().palette.textFaint));
        search_.onTextChange = [this] { searchDebounce_ = 8; };
        search_.setJustification(juce::Justification::centredLeft);
        addAndMakeVisible(search_);

        rail_.setModel(&railModel_);
        rail_.setRowHeight(24);
        rail_.setColour(juce::ListBox::backgroundColourId,
                        col(theme_->theme().palette.surfaceSunk));
        addAndMakeVisible(rail_);

        table_.setModel(&tableModel_);
        table_.setRowHeight(theme_->metrics().rowH);
        table_.setMultipleSelectionEnabled(true);
        table_.setColour(juce::ListBox::backgroundColourId,
                         col(theme_->theme().palette.surfaceSunk));
        addAndMakeVisible(table_);

        reloadRail();
        applySpec();
    }

    // ---- wiring outward ----
    std::function<void(const library::Track&)> onLoadDeck; // dbl-click

    library::Track selected() {
        const int r = table_.getSelectedRow();
        const library::Track* t = r >= 0 ? model_.row(r) : nullptr;
        return t ? *t : library::Track{};
    }
    int selectedRow() const { return table_.getSelectedRow(); }
    void moveSelection(int delta) {
        const int n = model_.total();
        if (n <= 0) return;
        const int r = juce::jlimit(
            0, n - 1,
            (table_.getSelectedRow() < 0 ? 0
                                         : table_.getSelectedRow()) + delta);
        table_.selectRow(r);
        table_.scrollToEnsureRowIsOnscreen(r);
    }
    void focusSearch() { search_.grabKeyboardFocus(); }
    TrackTableModel& model() { return model_; }

    // ---- sources / filters ----
    void applyLibrarySource() {
        model_.setPageSource(
            [this](const library::QuerySpec& q, int off, int lim) {
                auto qq = q; qq.offset = off; qq.limit = lim;
                return lib_->tracks().query(qq);
            });
    }
    void applyPlaylistSource(int playlistId) {
        model_.setPageSource(
            [this, playlistId]
            (const library::QuerySpec&, int off, int lim) {
                const auto ids = lib_->playlists().trackIds(playlistId);
                library::TrackPage page;
                page.total = (int)ids.size();
                for (int i = off;
                     i < (int)ids.size() && i < off + lim; ++i) {
                    library::Track t;
                    if (lib_->tracks().getById(ids[(size_t)i], t))
                        page.items.push_back(std::move(t));
                }
                return page;
            });
    }

    void applySpec() {
        // §13 — the search box accepts field tokens (artist:x bpm:120-128
        // key:Am rating:4+ tag:warmup …); the rest goes to FTS.
        library::QuerySpec q = library::SearchQuery::toSpec(
            search_.getText().toStdString());
        if (historyMode_) {
            // history view: recent plays as a page source
            model_.setPageSource(
                [this](const library::QuerySpec&, int off, int lim) {
                    const auto ents = lib_->history().entries(-1, lim, off);
                    library::TrackPage page;
                    for (auto& e : ents) {
                        library::Track t;
                        if (lib_->tracks().getById(e.trackId, t))
                            page.items.push_back(std::move(t));
                    }
                    page.total = page.items.size() < (size_t)lim
                               ? off + (int)page.items.size()
                               : off + lim + 1; // unknown tail
                    return page;
                });
        } else if (playlistFilter_ >= 0) {
            applyPlaylistSource(playlistFilter_);
        } else {
            applyLibrarySource();
        }
        model_.setSpec(q);
        table_.updateContent();
        table_.deselectAllRows();
    }

    void reloadRail() {
        railItems_.clear();
        railItems_.push_back({ "ALL TRACKS", -1 });
        railItems_.push_back({ "HISTORY", -3 });
        railItems_.push_back({ "PLAYLISTS", -2 });   // section header
        for (auto& pl : lib_->playlists().playlists(-1))
            railItems_.push_back({ pl.name, (int)pl.id });
        rail_.updateContent();
    }

    /// Called by AppShell's tick — debounce search typing.
    void tick() {
        if (searchDebounce_ > 0 && --searchDebounce_ == 0)
            applySpec();
    }

    void paint(juce::Graphics& g) override {
        const Palette& p = theme_->theme().palette;
        g.setColour(col(p.surfaceSunk));
        g.fillRect(getLocalBounds());
        const auto hb = juce::Rectangle<int>(
            railW_, 34, getWidth() - railW_, 20);
        g.setColour(col(p.surface));
        g.fillRect(hb);
        g.setFont(juce::Font(juce::FontOptions(
            (float)theme_->metrics().fontSm, juce::Font::bold)));
        int x = hb.getX();
        for (const auto& c : model_.columns()) {
            if (!c.visible) continue;
            g.setColour(col(p.textDim));
            g.drawText(juce::String(c.id).toUpperCase(),
                       x + 6, hb.getY(), c.widthPx - 8, hb.getHeight(),
                       juce::Justification::centredLeft);
            x += c.widthPx;
            g.setColour(col(p.borderSoft));
            g.drawVerticalLine(x, (float)hb.getY(),
                               (float)hb.getBottom());
        }
        // playlist rail header
        g.setColour(col(p.textFaint));
        g.drawText("SOURCES", 8, 34, railW_ - 8, 20,
                   juce::Justification::centredLeft);
    }

    void resized() override {
        auto b = getLocalBounds();
        search_.setBounds(b.removeFromTop(30).reduced(4));
        auto rail = b.removeFromLeft(railW_);
        b.removeFromTop(20);           // header strip
        rail_.setBounds(rail.withTrimmedTop(20));
        table_.setBounds(b);
    }

private:
    struct RailItem { std::string name; int id; };

    // -------- rail model: sources + playlists --------
    struct RailModel : juce::ListBoxModel {
        explicit RailModel(BrowserView& o) : o_(o) {}
        int getNumRows() override { return (int)o_.railItems_.size(); }
        void paintListBoxItem(int row, juce::Graphics& g,
                              int w, int h, bool sel) override {
            const Palette& p = o_.theme_->theme().palette;
            const Metrics m = o_.theme_->metrics();
            if (row < 0 || row >= (int)o_.railItems_.size()) return;
            const auto& it = o_.railItems_[(size_t)row];
            if (it.id == -2) {
                g.setColour(col(p.textFaint));
                g.setFont(juce::Font(juce::FontOptions(
                    (float)m.fontSm, juce::Font::bold)));
                g.drawText("PLAYLISTS", 8, 0, w - 8, h,
                           juce::Justification::centredLeft);
                return;
            }
            const bool active =
                (it.id == -1 && o_.playlistFilter_ == -1 &&
                 !o_.historyMode_) ||
                (it.id == -3 && o_.historyMode_) ||
                it.id == o_.playlistFilter_;
            if (sel || active) {
                g.setColour(col(p.selection));
                g.fillRect(0, 0, w, h);
            }
            g.setColour(col(it.id <= -1 ? p.text : p.textDim));
            g.setFont(juce::Font(juce::FontOptions((float)m.fontMd)));
            g.drawText(it.name, 10, 0, w - 10, h,
                       juce::Justification::centredLeft);
        }
        void listBoxItemClicked(int row,
                                const juce::MouseEvent&) override {
            if (row < 0 || row >= (int)o_.railItems_.size()) return;
            const auto& it = o_.railItems_[(size_t)row];
            if (it.id == -2) return;
            o_.historyMode_ = (it.id == -3);
            o_.playlistFilter_ = it.id > 0 ? it.id : -1;
            o_.applySpec();
        }
        BrowserView& o_;
    };

    // -------- table model: virtual rows over TrackTableModel --------
    struct TableModel : juce::ListBoxModel {
        explicit TableModel(BrowserView& o) : o_(o) {}
        int getNumRows() override { return o_.model_.total(); }
        void paintListBoxItem(int row, juce::Graphics& g,
                              int w, int h, bool sel) override {
            const Palette& p = o_.theme_->theme().palette;
            const Metrics m = o_.theme_->metrics();
            const library::Track* t = o_.model_.row(row);
            if (!t) return;
            if (sel) {
                g.setColour(col(p.selection));
                g.fillRect(0, 0, w, h);
            } else if (t->status ==
                       (int)library::TrackStatus::Missing) {
                g.setColour(col(p.missingRow));
                g.fillRect(0, 0, w, h);
            } else if (row & 1) {
                g.setColour(col(p.rowAlt));
                g.fillRect(0, 0, w, h);
            }
            int x = 0;
            g.setFont(juce::Font(juce::FontOptions((float)m.fontMd)));
            for (const auto& c : o_.model_.columns()) {
                if (!c.visible) continue;
                if (c.id == "color" && t->color != 0) {
                    g.setColour(juce::Colour((uint32_t)t->color));
                    g.fillRect(x + 4, 4, 8, h - 8);
                } else {
                    const auto txt =
                        TrackTableModel::cellText(*t, c.id);
                    const bool dim = c.id == "album" || c.id == "genre"
                                     || c.id == "dateAdded";
                    g.setColour(col(c.id == "key"
                                    ? o_.theme_->theme().accentColor()
                                    : dim ? p.textDim : p.text));
                    g.drawText(txt, x + 6, 0, c.widthPx - 8, h,
                               juce::Justification::centredLeft, true);
                }
                x += c.widthPx;
            }
        }
        void listBoxItemDoubleClicked(int row,
                                      const juce::MouseEvent&) override {
            const library::Track* t = o_.model_.row(row);
            if (t && o_.onLoadDeck) o_.onLoadDeck(*t);
        }
        BrowserView& o_;
    };

    const ThemeEngine* theme_;
    CommandManager*    cmds_;
    library::MusicLibraryEngine* lib_;
    TrackTableModel    model_;
    RailModel          railModel_;
    TableModel         tableModel_;
    juce::TextEditor   search_;
    juce::ListBox      rail_{ "rail", nullptr },
                     table_{ "tracks", nullptr };
    std::vector<RailItem> railItems_;
    int playlistFilter_ = -1;
    bool historyMode_   = false;
    int railW_ = 150;
    int searchDebounce_ = 0;
};

} // namespace onda::ui
