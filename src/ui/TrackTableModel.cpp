#include "TrackTableModel.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "core/analysis/KeyAnalyzer.h"

namespace onda::ui {

using library::Track;

TrackTableModel::Page* TrackTableModel::ensurePage(int p) {
    auto it = pages_.find(p);
    return it == pages_.end() ? fetchPage(p) : &it->second;
}

TrackTableModel::Page* TrackTableModel::fetchPage(int p) {
    Page pg;
    if (src_) {
        auto r = src_(spec_, p * kPage, kPage);
        total_ = r.total;
        pg.items = std::move(r.items);
    }
    ++fetches_;
    return &pages_.emplace(p, std::move(pg)).first->second;
}

void TrackTableModel::evictFar(int keepA, int keepB) {
    if ((int)pages_.size() <= kMaxPages) return;
    for (auto it = pages_.begin(); it != pages_.end();)
        it = (it->first < keepA || it->first > keepB)
             ? pages_.erase(it) : std::next(it);
}

void TrackTableModel::setDefaultColumns() {
    cols_ = {
        { "title",    220 }, { "artist",   170 }, { "bpm",    58 },
        { "key",       52 }, { "album",    150 }, { "genre",  90 },
        { "duration",  64 }, { "rating",   66 }, { "color",   40 },
        { "dateAdded", 95 }, { "plays",    52 },
    };
}

int TrackTableModel::visibleColumns() const {
    int n = 0;
    for (auto& c : cols_) n += c.visible ? 1 : 0;
    return n;
}

bool TrackTableModel::moveColumn(const std::string& id, int visPos) {
    auto src = std::find_if(cols_.begin(), cols_.end(),
        [&](const TableColumn& c) { return c.id == id && c.visible; });
    if (src == cols_.end()) return false;
    TableColumn col = *src;
    cols_.erase(src);
    int vis = 0;
    for (auto it = cols_.begin(); it != cols_.end(); ++it) {
        if (it->visible && vis == visPos) { cols_.insert(it, col); return true; }
        if (it->visible) ++vis;
    }
    cols_.push_back(col);
    return true;
}

bool TrackTableModel::setColumnVisible(const std::string& id, bool on) {
    for (auto& c : cols_)
        if (c.id == id) { c.visible = on; return true; }
    return false;
}

bool TrackTableModel::setColumnWidth(const std::string& id, int px) {
    for (auto& c : cols_)
        if (c.id == id) { c.widthPx = std::max(px, c.minW); return true; }
    return false;
}

std::string TrackTableModel::cellText(const Track& t,
                                      const std::string& col) {
    char buf[96];
    if (col == "title")    return t.title;
    if (col == "artist")   return t.artist;
    if (col == "album")    return t.album;
    if (col == "albumArtist") return t.albumArtist;
    if (col == "genre")    return t.genre;
    if (col == "filename") return t.filename;
    if (col == "format")   return t.format;
    if (col == "bpm") {
        return t.bpm > 0.0 ? (std::snprintf(buf, sizeof buf, "%.1f", t.bpm),
                              std::string(buf)) : "";
    }
    if (col == "key") {
        return t.keyIndex >= 0 && t.keyIndex < 24
             ? KeyAnalyzer::camelotName(t.keyIndex) : "";
    }
    if (col == "keyStd") {
        return t.keyIndex >= 0 && t.keyIndex < 24
             ? KeyAnalyzer::standardName(t.keyIndex) : "";
    }
    // Phase 22 §23 — mode + confidence columns
    if (col == "keyMode") {
        if (t.keyIndex < 0) return "";
        return t.keyIndex >= 12 ? "minor" : "major";
    }
    if (col == "keyConf") {
        if (t.keyIndex < 0 || t.keyConfidence <= 0.0f) return "";
        std::snprintf(buf, sizeof buf, "%d%%",
                      (int)(t.keyConfidence * 100.0f));
        return buf;
    }
    // Phase 23 — analysis energy descriptor (0..1, descriptive)
    if (col == "energyScore") {
        if (t.energyScore < 0.0f) return "";
        std::snprintf(buf, sizeof buf, "%.2f", (double)t.energyScore);
        return buf;
    }
    if (col == "year") {
        return t.year > 0 ? (std::snprintf(buf, sizeof buf, "%d", t.year),
                             std::string(buf)) : "";
    }
    if (col == "duration") {
        const int s = (int)(t.durationSec + 0.5);
        std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
        return buf;
    }
    if (col == "rating") {
        std::string r;
        for (int i = 0; i < t.rating && i < 5; ++i) r += '*';
        return r;
    }
    if (col == "plays") {
        std::snprintf(buf, sizeof buf, "%d", t.playCount);
        return buf;
    }
    if (col == "size") {
        std::snprintf(buf, sizeof buf, "%.1f MB", t.fileSize / 1048576.0);
        return buf;
    }
    if (col == "dateAdded") {
        const time_t ts = (time_t)t.dateAdded;
        std::tm tmv {};
#ifdef _WIN32
        localtime_s(&tmv, &ts);
#else
        localtime_r(&ts, &tmv);
#endif
        if (tmv.tm_year > 70) {
            std::snprintf(buf, sizeof buf, "%04d-%02d-%02d",
                          tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
            return buf;
        }
        return "";
    }
    if (col == "bitrate") {
        return t.bitrate > 0 ? (std::snprintf(buf, sizeof buf, "%d",
                                              (int)(t.bitrate / 1000)),
                                std::string(buf)) : "";
    }
    return "";
}

} // namespace onda::ui
