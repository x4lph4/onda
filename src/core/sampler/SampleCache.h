#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

#include "SamplerTypes.h"

namespace onda::sampler {

/// Bounded decoded-audio cache (task §25/§26). Keyed by slot id;
/// LRU eviction of non-pinned, non-playing entries when the byte cap
/// is exceeded. App thread only — the RT engine holds its own
/// shared_ptr so eviction never pulls audio from under playback.
class SampleCache {
public:
    explicit SampleCache(int64_t byteCap = 256ull * 1024 * 1024)
        : cap_(byteCap) {}

    void setCap(int64_t bytes) { cap_ = bytes; evict(); }
    int64_t cap()       const { return cap_; }
    int64_t bytesUsed() const { return used_; }
    int     entries()   const { return (int)map_.size(); }

    static int64_t bytesOf(const SampleData& d) {
        return d.frames * 2 * (int64_t)sizeof(float);
    }

    /// Insert/lookup/bump-to-front. `pinned` = preloaded for
    /// performance; pinned entries are never evicted.
    void put(int64_t key, std::shared_ptr<const SampleData> d,
             bool pinned = false) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            used_ -= bytesOf(*it->second.data);
            lru_.erase(it->second.lru);
            map_.erase(it);
        }
        lru_.push_front(key);
        map_[key] = { std::move(d), lru_.begin(), pinned };
        used_ += bytesOf(*map_[key].data);
        evict();
    }
    std::shared_ptr<const SampleData> get(int64_t key) {
        auto it = map_.find(key);
        if (it == map_.end()) return {};
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return it->second.data;
    }
    std::shared_ptr<const SampleData> peek(int64_t key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : it->second.data;
    }
    void pin(int64_t key, bool on) {
        auto it = map_.find(key);
        if (it != map_.end()) it->second.pinned = on;
    }
    bool remove(int64_t key) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        used_ -= bytesOf(*it->second.data);
        lru_.erase(it->second.lru);
        map_.erase(it);
        return true;
    }
    void clear() { map_.clear(); lru_.clear(); used_ = 0; }

    /// Drop the least-recently-used NON-pinned entries until under cap.
    /// Returns how many entries were evicted.
    int evict() {
        int n = 0;
        for (auto it = lru_.end(); used_ > cap_ && it != lru_.begin();) {
            --it;
            if (map_[*it].pinned) continue;
            used_ -= bytesOf(*map_[*it].data);
            map_.erase(*it);
            it = lru_.erase(it);
            ++n;
        }
        return n;
    }

private:
    struct Entry {
        std::shared_ptr<const SampleData> data;
        std::list<int64_t>::iterator      lru;
        bool pinned = false;
    };
    int64_t cap_;
    int64_t used_ = 0;
    std::list<int64_t>                          lru_;  // front = MRU
    std::unordered_map<int64_t, Entry>          map_;
};

} // namespace onda::sampler
