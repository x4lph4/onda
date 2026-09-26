#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/library/LibraryTypes.h"

namespace onda::ui {

/// Column descriptor for the track table — user-reorderable, resizable,
/// hideable. `id` is a stable key ("title","bpm",…), not a position.
struct TableColumn {
    std::string id;
    int         widthPx = 90;
    bool        visible = true;
    int         minW    = 34;
};

/// Virtualized table model over the library: keeps only a small window
/// of rows in memory regardless of library size. Pages are fetched
/// lazily via a pluggable page-source (production: MusicLibraryEngine::
/// query; tests: any function returning TrackPage).
///
/// Usage: views ask `row(i)` for visible indices; the model fetches
/// kPage-track chunks around the viewport and drops distant pages.
class TrackTableModel {
public:
    static constexpr int kPage = 128;        // rows per fetch
    static constexpr int kMaxPages = 8;      // cached window ≈ 1024 rows

    /// pageSource(offset, limit) → TrackPage{items,total}.
    using PageSource = std::function<
        library::TrackPage(const library::QuerySpec&, int off, int lim)>;

    void setPageSource(PageSource s) { src_ = std::move(s); }

    /// Replace the query — resets caches and re-reads `total`.
    void setSpec(const library::QuerySpec& q) {
        spec_ = q; spec_.offset = 0;        // paging done by row index
        pages_.clear();
        total_ = -1;
        version_++;
    }

    const library::QuerySpec& spec() const { return spec_; }
    int  version() const { return version_; }

    /// Total rows matching the spec (queries once, then cached).
    int total() {
        if (total_ < 0) fetchPage(0);
        return total_ < 0 ? 0 : total_;
    }

    /// Row `i` (0..total-1), or nullptr when out of range.
    const library::Track* row(int i) {
        if (i < 0 || i >= total()) return nullptr;
        return ensurePage(i / kPage)->slot(i % kPage);
    }

    /// Prefetch around a viewport (call when the view scrolls).
    void prefetch(int firstRow, int visibleRows) {
        const int a = std::max(0, firstRow - kPage) / kPage;
        const int b = (firstRow + visibleRows + kPage) / kPage;
        for (int p = a; p <= b; ++p) ensurePage(p);
        evictFar(a, b);
    }

    /// The row indices currently resident — tests verify windowing.
    int cachedRowCount() const {
        int n = 0;
        for (auto& [p, pg] : pages_) n += (int)pg.items.size();
        return n;
    }
    int pageFetches() const { return fetches_; }   // perf/tests

    // ---- columns ----
    std::vector<TableColumn>&       columns()       { return cols_; }
    const std::vector<TableColumn>& columns() const { return cols_; }
    void setDefaultColumns();
    /// Reorder a visible column before another (drag-drop by id).
    bool moveColumn(const std::string& id, int newVisiblePos);
    bool setColumnVisible(const std::string& id, bool on);
    bool setColumnWidth(const std::string& id, int px);
    int  visibleColumns() const;

    // ---- cell text (column id → display string) ----
    static std::string cellText(const library::Track& t,
                                const std::string& col);

private:
    struct Page {
        std::vector<library::Track> items;
        const library::Track* slot(int i) const {
            return i < (int)items.size() ? &items[(size_t)i] : nullptr;
        }
    };

    Page* ensurePage(int p);
    Page* fetchPage(int p);
    void  evictFar(int keepA, int keepB);

    library::QuerySpec spec_;
    PageSource         src_;
    int                total_   = -1;
    int                version_ = 0;
    int                fetches_ = 0;
    std::unordered_map<int, Page> pages_;
    std::vector<TableColumn>      cols_;
};

} // namespace onda::ui
