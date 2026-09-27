#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../data/Database.h"
#include "../library/TagManager.h"
#include "../library/TrackRepository.h"
#include "SmartLibraryTypes.h"

namespace onda::smart {

/// Phase 23 §39-40 — bulk metadata edits. Every op:
///   - runs off the UI thread (post() to the library worker, or
///     runSync for tests)
///   - is cancelable between items (atomic flag, checked per track)
///   - records ONE reversible undo entry — undo restores prior
///     values in a single transaction, never N individual ops (§40)
///
/// Metadata only: tags / rating / color / analysis enqueue. Files
/// are never touched, user data is never deleted by cleanup (§44).
class LibraryBatchEngine {
public:
    enum class Op : int {
        AddTag = 0, RemoveTag, SetRating, SetColor,
        Analyze, Reanalyze,
    };

    struct Progress {
        int total { 0 }, done { 0 }, failed { 0 };
        bool cancelled { false };
    };

    using PostFn     = std::function<void(std::function<void()>)>;
    using AnalyzeFn  = std::function<void(int64_t trackId, bool force)>;

    LibraryBatchEngine(Database& db, library::TrackRepository& repo)
        : db_(db), repo_(repo) {}

    void setAnalyzeFn(AnalyzeFn f) { analyzeFn_ = std::move(f); }

    /// One batch job. `tag` for tag ops; `value` for rating/color.
    /// Returns entries changed; progress is queryable during async.
    int apply(const std::vector<int64_t>& ids, Op op,
              const std::string& tag = "", int value = 0,
              const std::shared_ptr<std::atomic<bool>>& cancel =
                  nullptr) {
        undo_.clear();
        progress_ = Progress{ (int)ids.size(), 0, 0, false };
        int changed = 0;

        auto stop = [&] {
            return cancel && cancel->load();
        };
        for (const int64_t id : ids) {
            if (stop()) { progress_.cancelled = true; break; }
            if (applyOne(id, op, tag, value)) ++changed;
            else ++progress_.failed;
            ++progress_.done;
        }
        return changed;
    }

    /// Async on the library worker via `post`.
    void applyAsync(PostFn post, const std::vector<int64_t>& ids, Op op,
                    const std::string& tag, int value,
                    std::function<void(int)> done = {}) {
        cancel_ = std::make_shared<std::atomic<bool>>(false);
        auto c = cancel_;
        post([=, this] {
            const int n = apply(ids, op, tag, value, c);
            if (done) done(n);
        });
    }
    void cancel() { if (cancel_) cancel_->store(true); }
    Progress progress() const { return progress_; }

    /// §40 — undo the LAST batch as one operation. For tags it
    /// restores prior membership per track; for rating/color the
    /// prior value. One transaction for the whole batch.
    int undoLast() {
        if (undo_.empty()) return 0;
        std::lock_guard lk(db_.mutex());
        Database::Tx tx(db_);
        int n = 0;
        for (const auto& e : undo_) {
            if (!e.tag.empty()) {                    // tag ops
                // restore membership inline — repo_.addTag would open
                // a NESTED transaction and commit ours mid-loop
                const int64_t tid = repo_.tagId(e.tag);
                if (tid < 0) continue;
                if (e.hadTag) {
                    auto s = db_.prepare(
                        "INSERT OR IGNORE INTO track_tags"
                        "(track_id,tag_id) VALUES(?,?)");
                    s.bind(1, e.trackId); s.bind(2, tid);
                    s.step();
                } else {
                    auto s = db_.prepare(
                        "DELETE FROM track_tags"
                        " WHERE track_id=? AND tag_id=?");
                    s.bind(1, e.trackId); s.bind(2, tid);
                    s.step();
                }
                repo_.refreshIndex(e.trackId);
            } else {                                 // rating/color
                auto s = db_.prepare(undoField_ == 0
                    ? "UPDATE tracks SET rating=? WHERE id=?"
                    : "UPDATE tracks SET color=? WHERE id=?");
                s.bind(1, (int64_t)e.intValue);
                s.bind(2, e.trackId);
                s.step();
            }
            ++n;
        }
        tx.commit();
        undo_.clear();
        return n;
    }
    bool canUndo() const { return !undo_.empty(); }

private:
    bool hasTag(int64_t id, const std::string& name) {
        for (const auto& t : repo_.tagsOf(id))
            if (t == name) return true;
        return false;
    }
    bool applyOne(int64_t id, Op op, const std::string& tag,
                  int value) {
        switch (op) {
            case Op::AddTag: {
                const bool had = hasTag(id, tag);
                if (repo_.addTag(id, tag)) {
                    undo_.push_back({ id, tag, 0, had });
                    return true;
                }
                return false;
            }
            case Op::RemoveTag: {
                const bool had = hasTag(id, tag);
                if (repo_.removeTag(id, tag)) {
                    undo_.push_back({ id, tag, 0, had });
                    return true;
                }
                return false;
            }
            case Op::SetRating:
            case Op::SetColor: {
                library::Track t;
                if (!repo_.getById(id, t)) return false;
                const int prior =
                    op == Op::SetRating ? t.rating : t.color;
                if (op == Op::SetRating)
                    value = std::clamp(value, 0, 5);
                std::lock_guard lk(db_.mutex());
                auto s = db_.prepare(op == Op::SetRating
                    ? "UPDATE tracks SET rating=? WHERE id=?"
                    : "UPDATE tracks SET color=? WHERE id=?");
                s.bind(1, (int64_t)value); s.bind(2, id);
                s.step();
                undoField_ = op == Op::SetRating ? 0 : 1;
                undo_.push_back({ id, "", prior, false });
                return true;
            }
            case Op::Analyze:
            case Op::Reanalyze:
                if (analyzeFn_) {
                    analyzeFn_(id, op == Op::Reanalyze);
                    return true;
                }
                return false;
        }
        return false;
    }

    Database& db_;
    library::TrackRepository& repo_;
    AnalyzeFn analyzeFn_;
    std::vector<BatchUndoEntry> undo_;
    int undoField_ { 0 };
    Progress progress_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};

} // namespace onda::smart
