#pragma once

#include <deque>
#include <functional>
#include <string>

namespace onda::ui {

/// §41 — undo/redo stack for performance edits. Command handlers call
/// record() AFTER applying an action, supplying the revert/re-apply
/// closures; perf.undo/perf.redo commands walk the stacks. Entries
/// capture full state snapshots — a record NEVER diffs or guesses.
///
/// Deliberately coarse: one entry per user-visible action (a cue
/// drag commits once on release, a loop resize once per step) — the
/// spec explicitly forbids per-frame entries for continuous ops.
class UndoManager {
public:
    struct Entry {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
    };
    static constexpr size_t kMaxDepth = 64;

    /// Record an already-applied action. `undo` restores the captured
    /// prior state; `redo` replays the action (stored for redo-after-
    /// undo — cheap because it reuses the same captured snapshot).
    void record(std::string label, std::function<void()> undo,
                std::function<void()> redo) {
        undo_.push_back({ std::move(label), std::move(undo),
                          std::move(redo) });
        if (undo_.size() > kMaxDepth) undo_.pop_front();
        redo_.clear();                       // new action kills redo
    }

    bool undo() {
        if (undo_.empty()) return false;
        Entry e = std::move(undo_.back());
        undo_.pop_back();
        if (e.undo) e.undo();
        redo_.push_back(std::move(e));
        return true;
    }
    bool redo() {
        if (redo_.empty()) return false;
        Entry e = std::move(redo_.back());
        redo_.pop_back();
        if (e.redo) e.redo();
        undo_.push_back(std::move(e));
        return true;
    }

    bool   canUndo() const { return !undo_.empty(); }
    bool   canRedo() const { return !redo_.empty(); }
    size_t depth()   const { return undo_.size(); }
    const std::string& undoLabel() const {
        static const std::string none;
        return undo_.empty() ? none : undo_.back().label;
    }
    void clear() { undo_.clear(); redo_.clear(); }

private:
    std::deque<Entry> undo_, redo_;
};

} // namespace onda::ui
