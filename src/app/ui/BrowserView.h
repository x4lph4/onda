#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "OndaTheme.h"
#include "HarmonicPanel.h"
#include "SmartQueryPanel.h"
#include "core/harmonic/Compatibility.h"
#include "core/harmonic/KeyNormalizer.h"
#include "core/library/MusicLibraryEngine.h"
#include "core/smart/LibraryPresets.h"
#include "core/smart/SmartLibraryEngine.h"
#include "ui/CommandManager.h"
#include "ui/TrackTableModel.h"
#include "ui/UiState.h"

namespace onda::ui {

/// Browser panel: search field, left rail (sources + playlists/crates),
/// virtualized track table, column header. Drag source: rows expose
/// the track path for deck/playlist targets.
/// File drop target: audio files import directly, folders become
/// watched roots (recursive + autoImport) and trigger a scan.
///
/// Virtualization: juce::ListBox paints only visible rows; the model
/// fetches kPage-row pages — a 100k library costs like a 1k one.
class BrowserView : public juce::Component,
                    public juce::FileDragAndDropTarget {
public:
    BrowserView(const ThemeEngine& te, CommandManager& cm,
                library::MusicLibraryEngine& lib)
        : theme_(&te), cmds_(&cm), lib_(&lib),
          railModel_(*this), tableModel_(*this), harmonic_(te) {
        model_.setDefaultColumns();
        applyLibrarySource();

        search_.setTextToShowWhenEmpty("search — title, artist, "
                                       "album, tag…",
            col(theme_->theme().palette.textFaint));
        search_.onTextChange = [this] { searchDebounce_ = 8; };
        search_.setJustification(juce::Justification::centredLeft);
        addAndMakeVisible(search_);

        // import entry points — files via picker, folders become
        // watched roots; drag & drop onto the panel does both.
        auto setupImport = [this](juce::TextButton& b,
                                  const char* tip) {
            b.setColour(juce::TextButton::buttonColourId,
                        col(theme_->theme().palette.surfaceRaised));
            b.setColour(juce::TextButton::textColourOffId,
                        col(theme_->theme().palette.accent));
            b.setTooltip(tip);
            addAndMakeVisible(b);
        };
        setupImport(importFiles_, "Import audio files");
        setupImport(importDir_, "Watch a music folder (recursive)");
        importFiles_.onClick = [this] { openImportChooser(false); };
        importDir_.onClick  = [this] { openImportChooser(true); };

        rail_.setModel(&railModel_);
        rail_.setRowHeight(24);
        rail_.setColour(juce::ListBox::backgroundColourId,
                        col(theme_->theme().palette.surfaceSunk));
        addAndMakeVisible(rail_);

        table_.setModel(&tableModel_);
        table_.setRowHeight(theme_->metrics().rowH);
        table_.setMultipleSelectionEnabled(true);
        // row drag → decks/playlists: payload is "ondatrack:<path>"
        table_.onRowDrag = [this](int row, const juce::MouseEvent& e) {
            startTrackDrag(row, e);
        };
        table_.setColour(juce::ListBox::backgroundColourId,
                         col(theme_->theme().palette.surfaceSunk));
        addAndMakeVisible(table_);

        // Phase 22 §49 — harmonic panel: wheel pick writes a
        // camelot_compatible: search token (advisory filter; the DJ
        // still picks the track).
        harmonic_.wheel().onPick = [this](int keyIndex) {
            if (keyIndex < 0) return;
            const auto cam = harmonic::CamelotConverter::fromIndex(keyIndex);
            if (!cam.valid()) return;
            search_.setText("camelot_compatible:" + cam.str(), false);
            applySpec();
        };
        addAndMakeVisible(harmonic_);

        reloadRail();
        applySpec();
    }

    // ---- wiring outward ----
    std::function<void(const library::Track&)> onLoadDeck; // dbl-click
    /// right-click context menu → assign to a specific deck (0-3)
    std::function<void(int deck, const library::Track&)> onLoadDeckTo;

    /// Right-click row menu — "Load to Deck A-D".
    void showTrackMenu(int row, int screenX, int screenY) {
        const library::Track* t = model_.row(row);
        if (!t || t->path.empty()) return;
        const library::Track track = *t;   // copy — menu is async
        juce::PopupMenu m;
        const juce::String title =
            juce::String(track.artist.empty() ? track.filename
                                            : track.artist)
            + " - " + juce::String(track.title);
        m.addSectionHeader(title);
        m.addSeparator();
        for (int d = 0; d < 4; ++d)
            m.addItem(1 + d,
                      "Load to Deck " + juce::String(
                          (char)('A' + d)));
        m.showMenuAsync(juce::PopupMenu::Options()
                .withTargetScreenArea(juce::Rectangle<int>(
                    screenX, screenY, 1, 1)),
            [this, track](int picked) {
                if (picked >= 1 && picked <= 4 && onLoadDeckTo)
                    onLoadDeckTo(picked - 1, track);
            });
    }

    /// Phase 23 — attach the smart-library facade (lazy panel).
    void setSmartLib(smart::SmartLibraryEngine* e) {
        smart_ = e;
        if (e && !smartPanel_) {
            smartPanel_ = std::make_unique<SmartQueryPanel>(
                *theme_, *e);
            smartPanel_->onSaved = [this] {
                reloadRail();
                smartPanel_->setVisible(false);
                resized();
            };
            smartPanel_->setVisible(false);
            addChildComponent(*smartPanel_);
        }
        reloadRail();
    }

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
    HarmonicPanel&   harmonic() { return harmonic_; }

    /// Phase 22 — per-tick feed: ref = reference deck's key/bpm,
    /// candidate = current table selection. Advisory only.
    void setHarmonicRef(int keyIndex, float conf, bool ambiguous,
                        double bpm, const std::string& title) {
        ref_ = { keyIndex, conf, ambiguous, bpm, title };
        HarmonicPanel::State s;
        s.refKey = keyIndex; s.refConf = conf; s.refAmbiguous = ambiguous;
        s.refBpm = bpm; s.refTitle = title;
        const library::Track sel = selected();
        if (sel.id > 0 && sel.keyIndex >= 0) {
            s.candKey = sel.keyIndex; s.candConf = sel.keyConfidence;
            s.candBpm = sel.bpm;
            s.candTitle = sel.title.empty() ? sel.filename : sel.title;
            const auto mc = harmonic::MusicalCompatibilityEngine::relate(
                harmonic::MusicalKey::fromIndex(keyIndex, conf), bpm,
                harmonic::MusicalKey::fromIndex(sel.keyIndex,
                                                sel.keyConfidence),
                sel.bpm);
            s.relation = mc.explanation;
            s.mixable  = mc.mixableNow;
        }
        harmonic_.setState(s);
    }

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
        // Phase 23 — preset sources merge their filters/sort into
        // the search spec; same engine, still indexed.
        if (preset_ >= 0 && smart_) {
            library::QuerySpec pq;
            switch ((smart::LibraryPresets::Id)preset_) {
                case smart::LibraryPresets::Id::RecentlyAdded:
                    pq = smart::LibraryPresets::recentlyAdded(); break;
                case smart::LibraryPresets::Id::RecentlyPlayed:
                    pq = smart::LibraryPresets::recentlyPlayed(); break;
                case smart::LibraryPresets::Id::MostPlayed:
                    pq = smart::LibraryPresets::mostPlayed(); break;
                case smart::LibraryPresets::Id::Unplayed:
                    pq = smart::LibraryPresets::unplayed(); break;
                case smart::LibraryPresets::Id::NotPlayedRecently:
                    pq = smart::LibraryPresets::notPlayedRecently(); break;
                case smart::LibraryPresets::Id::MissingFiles:
                    pq = smart::LibraryPresets::missingFiles(); break;
                case smart::LibraryPresets::Id::Unanalyzed:
                    pq = smart::LibraryPresets::unanalyzed(); break;
                default: break;
            }
            for (auto& f : pq.filters) q.filters.push_back(f);
            if (q.search.empty()) {
                q.sort = pq.sort; q.ascending = pq.ascending;
            }
        }
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
        } else if (duplicatesMode_ && lib_) {
            // §37 — possible duplicates: flattened detector groups.
            // Read-only view; deletion/merge is always user-initiated.
            model_.setPageSource(
                [this](const library::QuerySpec&, int off, int lim) {
                    std::vector<int64_t> ids;
                    for (auto& g : lib_->duplicates().findAll())
                        for (int64_t id : g.trackIds)
                            ids.push_back(id);
                    std::sort(ids.begin(), ids.end());
                    ids.erase(std::unique(ids.begin(), ids.end()),
                              ids.end());
                    library::TrackPage page;
                    page.total = (int)ids.size();
                    for (int i = off;
                         i < off + lim && i < (int)ids.size(); ++i) {
                        library::Track t;
                        if (lib_->tracks().getById(ids[(size_t)i], t))
                            page.items.push_back(std::move(t));
                    }
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
        if (smart_) {
            railItems_.push_back({ "SMART", -2 });   // section header
            // rail id = -(10 + LibraryPresets::Id ordinal)
            railItems_.push_back({ "Recently Added", -10 });
            railItems_.push_back({ "Recently Played", -11 });
            railItems_.push_back({ "Most Played", -12 });
            railItems_.push_back({ "Unplayed", -13 });
            railItems_.push_back({ "Not Played Recently", -14 });
            railItems_.push_back({ "Missing Files", -17 });
            railItems_.push_back({ "Analysis Status", -18 });
            railItems_.push_back({ "Possible Duplicates", -21 });
            railItems_.push_back({ "+ New Smart Query", -20 });
        }
        railItems_.push_back({ "PLAYLISTS", -2 });   // section header
        for (auto& pl : lib_->playlists().playlists(-1))
            railItems_.push_back({ pl.name, (int)pl.id });
        rail_.updateContent();
    }

    /// Begin an app drag for a table row — payload
    /// "ondatrack:<path>"; deck/waveform targets load the track. — a labelled chip follows
    /// the cursor; decks accept the drop and load the track.
    void startTrackDrag(int row, const juce::MouseEvent&) {
        const library::Track* t = model_.row(row);
        if (!t || t->path.empty()) return;
        auto* dnd =
            juce::DragAndDropContainer::findParentDragContainerFor(this);
        if (!dnd) return;

        const juce::String label =
            juce::String(t->artist.empty() ? t->filename : t->artist)
            + " - " + juce::String(t->title.empty() ? "" : t->title);
        const int iw = 220, ih = 24;
        juce::Image img(juce::Image::ARGB, iw, ih, true);
        {
            juce::Graphics g(img);
            g.setColour(col(theme_->theme().palette.surfaceRaised));
            g.fillRoundedRectangle(0, 0, (float)iw, (float)ih, 4.0f);
            g.setColour(col(theme_->theme().accentColor()));
            g.drawRoundedRectangle(0.5f, 0.5f, iw - 1.0f, ih - 1.0f,
                                   4.0f, 1.0f);
            g.setColour(col(theme_->theme().palette.text));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText(label, 8, 0, iw - 12, ih,
                       juce::Justification::centredLeft, true);
        }
        dnd->startDragging(
            juce::var(juce::String(kTrackDragPrefix)
                      + juce::String(t->path)),
            &table_, juce::ScaledImage(img), false);
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

        if (dragHover_) {
            g.setColour(col(p.accent).withAlpha(0.15f));
            g.fillRect(getLocalBounds());
            g.setColour(col(p.accent));
            g.drawRect(getLocalBounds().reduced(1), 2);
            g.setFont(juce::Font(juce::FontOptions(
                (float)theme_->metrics().fontLg, juce::Font::bold)));
            g.drawText("DROP TO IMPORT", getLocalBounds(),
                       juce::Justification::centred);
        }
    }

    void resized() override {
        auto b = getLocalBounds();
        auto top = b.removeFromTop(30).reduced(4);
        importDir_.setBounds(top.removeFromRight(84));
        top.removeFromRight(4);
        importFiles_.setBounds(top.removeFromRight(64));
        top.removeFromRight(4);
        search_.setBounds(top);
        // Phase 23 — smart query editor docks to the bottom edge
        if (smartPanel_ && smartPanel_->isVisible()) {
            const int ph = juce::jmin(300, getHeight() / 2);
            smartPanel_->setBounds(b.removeFromBottom(ph));
        }
        // Phase 22 — harmonic panel column (square-ish, right edge)
        const int hw = juce::jmin(220, b.getWidth() / 4);
        if (hw >= 140)
            harmonic_.setBounds(b.removeFromRight(hw));
        else
            harmonic_.setBounds(0, 0, 0, 0);
        auto rail = b.removeFromLeft(railW_);
        b.removeFromTop(20);           // header strip
        rail_.setBounds(rail.withTrimmedTop(20));
        table_.setBounds(b);
    }

    // ---- file import (drag & drop + pickers) ----
    bool isInterestedInFileDrag(const juce::StringArray& files) override {
        for (const auto& f : files)
            if (juce::File(f).isDirectory() ||
                library::TrackImporter::isAudioFile(f.toStdString()))
                return true;
        return false;
    }
    void fileDragEnter(const juce::StringArray&, int, int) override {
        dragHover_ = true; repaint();
    }
    void fileDragExit(const juce::StringArray&) override {
        dragHover_ = false; repaint();
    }
    void filesDropped(const juce::StringArray& files, int, int) override {
        dragHover_ = false; repaint();
        importPaths(files);
    }

    /// pickFolder → watched root (recursive, autoImport, persisted);
    /// otherwise multi-file picker → direct import.
    void openImportChooser(bool pickFolder) {
        chooser_ = std::make_unique<juce::FileChooser>(
            pickFolder ? "Choose a music folder to watch"
                       : "Choose audio files to import",
            juce::File::getSpecialLocation(
                juce::File::userMusicDirectory),
            "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac;*.mp3;*.m4a;"
            "*.aac;*.ogg;*.opus;*.wma", true);
        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectMultipleItems
            | (pickFolder
               ? juce::FileBrowserComponent::canSelectDirectories
               : juce::FileBrowserComponent::canSelectFiles),
            [this](const juce::FileChooser& fc) {
                juce::StringArray items;
                for (const auto& f : fc.getResults())
                    items.add(f.getFullPathName());
                importPaths(items);
            });
    }

private:
    /// Folders become watched roots + scan; audio files import directly.
    void importPaths(const juce::StringArray& items) {
        std::vector<std::string> files;
        bool dirs = false;
        for (const auto& s : items) {
            const juce::File f(s);
            if (f.isDirectory()) {
                lib_->addWatchRoot(s.toStdString(), true, true);
                dirs = true;
            } else if (f.existsAsFile() &&
                       library::TrackImporter::isAudioFile(
                           s.toStdString())) {
                files.push_back(s.toStdString());
            }
        }
        if (dirs)
            lib_->scanAsync([this](const library::ScanDiff&) {
                refreshAfterImport();
            });
        if (!files.empty())
            lib_->importPathsAsync(std::move(files),
                [this](const library::ImportResult&) {
                    refreshAfterImport();
                });
    }

    /// Import callbacks run on the library worker — bounce to the
    /// message thread before touching the model.
    void refreshAfterImport() {
        juce::Component::SafePointer<BrowserView> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (auto* b = safe.getComponent()) {
                b->applySpec();
                b->reloadRail();
            }
        });
    }

    struct RailItem { std::string name; int id; };

    // -------- track table: ListBox + row-drag source --------
    /// A ListBox that reports when a row drag passes the threshold —
    /// the view turns it into a track drag ("ondatrack:<path>").
    struct TrackTable : juce::ListBox {
        using juce::ListBox::ListBox;
        std::function<void(int, const juce::MouseEvent&)> onRowDrag;
        void mouseDown(const juce::MouseEvent& e) override {
            // Capture the row at press time — by the drag threshold
            // the cursor may sit on a neighbour row.
            dragRow_ = getRowContainingPosition(e.x, e.y);
            juce::ListBox::mouseDown(e);
        }
        void mouseDrag(const juce::MouseEvent& e) override {
            if (onRowDrag && dragRow_ >= 0
                && e.getDistanceFromDragStart() > 10) {
                const int row = dragRow_;
                dragRow_ = -1;      // one drag attempt per gesture
                onRowDrag(row, e);
                return;
            }
            juce::ListBox::mouseDrag(e);
        }
        void mouseUp(const juce::MouseEvent& e) override {
            dragRow_ = -1;
            juce::ListBox::mouseUp(e);
        }
        int dragRow_ = -1;
    };

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
            // Phase 23 — smart presets and the query editor entry.
            // Preset rail ids are -(10 + enum ordinal); special
            // entries use -20 (query panel) and -21 (duplicates).
            if (it.id <= -10 && it.id >= -18) {
                o_.preset_ = -it.id - 10;
                o_.historyMode_ = false;
                o_.duplicatesMode_ = false;
                o_.playlistFilter_ = -1;
                o_.applySpec();
                return;
            }
            if (it.id == -20) {
                if (o_.smartPanel_) {
                    o_.smartPanel_->setVisible(
                        !o_.smartPanel_->isVisible());
                    o_.resized();
                }
                return;                     // keep current source
            }
            if (it.id == -21) {
                o_.preset_ = -1;
                o_.historyMode_ = false;
                o_.duplicatesMode_ = true;
                o_.playlistFilter_ = -1;
                o_.applySpec();
                return;
            }
            o_.preset_ = -1;
            o_.duplicatesMode_ = false;
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
        void listBoxItemClicked(int row,
                                const juce::MouseEvent& e) override {
            if (!e.mods.isPopupMenu()) return;
            o_.table_.selectRow(row);
            o_.showTrackMenu(row, e.getScreenX(), e.getScreenY());
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
    juce::TextButton   importFiles_{ "+ FILES" },
                       importDir_{ "+ FOLDER" };
    std::unique_ptr<juce::FileChooser> chooser_;
    bool               dragHover_ = false;
    juce::ListBox      rail_{ "rail", nullptr };
    TrackTable         table_{ "tracks" };
    HarmonicPanel      harmonic_;
    struct { int keyIndex { -1 }; float conf { 0.0f };
             bool ambiguous { false }; double bpm { 0.0 };
             std::string title; } ref_;
    std::vector<RailItem> railItems_;
    int playlistFilter_ = -1;
    int preset_         = -1;          // Phase 23 smart preset id
    bool historyMode_   = false;
    bool duplicatesMode_ = false;      // §37 possible-duplicates view
    smart::SmartLibraryEngine* smart_ = nullptr;
    std::unique_ptr<SmartQueryPanel> smartPanel_;
    int railW_ = 150;
    int searchDebounce_ = 0;
};

} // namespace onda::ui
