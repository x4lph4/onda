#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace onda::ui {

/// Non-modal toast queue. Producers (engine events, import results,
/// device changes) push items; the overlay drains + ages them out.
/// Bounded capacity — a burst can never grow memory unbounded.
struct Notification {
    enum class Level : int { Info = 0, Success, Warning, Error };
    Level       level = Level::Info;
    std::string text;
    std::string detail;
    double      ttlSec = 4.0;
    uint64_t    seq    = 0;     // arrival order
};

class NotificationCenter {
public:
    explicit NotificationCenter(size_t capacity = 8) : cap_(capacity) {}

    void push(Notification::Level level, std::string text,
              std::string detail = {}, double ttl = 4.0) {
        // dedup: identical text still visible → refresh instead of stacking
        for (auto& n : items_)
            if (n.text == text) { n.seq = ++seq_; return; }
        if (items_.size() >= cap_) items_.pop_front();
        items_.push_back({ level, std::move(text), std::move(detail),
                           ttl, ++seq_ });
    }
    void info   (std::string t, std::string d = {}) { push(Notification::Level::Info,    std::move(t), std::move(d)); }
    void success(std::string t, std::string d = {}) { push(Notification::Level::Success, std::move(t), std::move(d)); }
    void warn   (std::string t, std::string d = {}) { push(Notification::Level::Warning, std::move(t), std::move(d), 6.0); }
    void error  (std::string t, std::string d = {}) { push(Notification::Level::Error,   std::move(t), std::move(d), 8.0); }

    /// Age items; call once per UI frame with dt seconds.
    void tick(double dt) {
        for (auto it = items_.begin(); it != items_.end();)
            it = (it->ttlSec -= dt) <= 0.0 ? items_.erase(it)
                                          : std::next(it);
    }

    const std::deque<Notification>& items() const { return items_; }
    size_t size()  const { return items_.size(); }
    void   clear()       { items_.clear(); }

private:
    std::deque<Notification> items_;
    size_t   cap_ = 8;
    uint64_t seq_ = 0;
};

} // namespace onda::ui
