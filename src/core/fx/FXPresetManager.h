#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "FXPreset.h"

namespace onda::fx {

/// Factory + user presets. Pure non-RT bookkeeping — applying a preset
/// is the caller's job (it needs the MixerController + loadEffect path
/// anyway); serialize()/restore() give the app a blob it can persist
/// wherever it likes (DB kv, file, session).
class FXPresetManager {
public:
    FXPresetManager() { seedFactory(); }

    int count() const noexcept { return (int)presets_.size(); }
    const FXPreset& at(int i) const noexcept { return presets_[(size_t)i]; }

    int find(const std::string& name) const noexcept {
        for (size_t i = 0; i < presets_.size(); ++i)
            if (presets_[i].name == name) return (int)i;
        return -1;
    }

    int save(const FXPreset& p) {
        const int i = find(p.name);
        if (i >= 0 && presets_[(size_t)i].factory)
            return -1; // factory presets are immutable
        if (i >= 0) { presets_[(size_t)i] = p; return i; }
        FXPreset copy = p;
        copy.factory = false;
        presets_.push_back(std::move(copy));
        return (int)presets_.size() - 1;
    }

    bool remove(int i) {
        if (i < 0 || i >= (int)presets_.size() || presets_[(size_t)i].factory)
            return false;
        presets_.erase(presets_.begin() + i);
        return true;
    }

    bool rename(int i, const std::string& name) {
        if (i < 0 || i >= (int)presets_.size() || presets_[(size_t)i].factory)
            return false;
        presets_[(size_t)i].name = name;
        return true;
    }

    /// Whole-manager blob: 'ONFP' | ver u8 | count u16 | preset blobs.
    std::vector<uint8_t> serializeAll() const {
        uint16_t userCount = 0;
        for (const auto& p : presets_) if (!p.factory) ++userCount;
        std::vector<uint8_t> b = { 'O', 'N', 'F', 'P', 1 };
        b.insert(b.end(), (const uint8_t*)&userCount,
                 (const uint8_t*)&userCount + 2);
        for (const auto& p : presets_) {
            if (p.factory) continue; // factory list is regenerated
            auto pb = serializePreset(p);
            const uint16_t len = (uint16_t)pb.size();
            b.insert(b.end(), (const uint8_t*)&len, (const uint8_t*)&len + 2);
            b.insert(b.end(), pb.begin(), pb.end());
        }
        return b;
    }

    /// Restore user presets from serializeAll() output (factory list
    /// is kept; user entries are appended).
    bool restoreAll(const uint8_t* d, size_t n) {
        if (d == nullptr || n < 7 || std::memcmp(d, "ONFP", 4) != 0 || d[4] != 1)
            return false;
        // drop existing user presets
        presets_.erase(std::remove_if(presets_.begin(), presets_.end(),
                       [](const FXPreset& p) { return !p.factory; }),
                       presets_.end());
        uint16_t count = 0;
        std::memcpy(&count, d + 5, 2);
        size_t off = 7;
        for (uint16_t i = 0; i < count; ++i) {
            if (off + 2 > n) return false;
            uint16_t len = 0;
            std::memcpy(&len, d + off, 2); off += 2;
            if (off + len > n) return false;
            FXPreset p;
            if (!deserializePreset(d + off, len, p)) return false;
            p.factory = false;
            presets_.push_back(std::move(p));
            off += len;
        }
        return true;
    }

private:
    void seedFactory() {
        auto add = [&](const char* nm, FXKind k, int sync, int div,
                       float mix, std::initializer_list<float> prm) {
            FXPreset p;
            p.name = nm; p.factory = true; p.effectKind = (int)k;
            p.syncMode = sync; p.division = div; p.mix = mix;
            int i = 0;
            for (float v : prm) {
                if (i < kMaxFxParams) p.params[i] = v;
                ++i;
            }
            presets_.push_back(std::move(p));
        };
        add("Classic Delay", FXKind::Delay, 1, (int)BeatDiv::D_1_4, 0.45f,
            { 350.0f, 0.40f, 9000.0f, 1.0f });
        add("Dub Echo",      FXKind::Echo,  1, (int)BeatDiv::D_1_2, 0.55f,
            { 0.55f, 5000.0f, 1.0f, 0.0f });
        add("Room Verb",     FXKind::Reverb,0, (int)BeatDiv::D_1_4, 0.35f,
            { 0.55f, 0.55f, 0.40f, 12.0f, 0.9f });
        add("Reso Sweep",    FXKind::Filter,1, (int)BeatDiv::D_1_1, 1.0f,
            { 900.0f, 0.45f, 0.0f, 0.5f, 0.5f });
        add("Jet",           FXKind::Flanger,1,(int)BeatDiv::D_1_1, 0.5f,
            { 0.30f, 0.75f, 0.45f, 0.25f });
        add("Phaser Sweep",  FXKind::Phaser,1, (int)BeatDiv::D_2Bar,0.5f,
            { 0.40f, 0.70f, 0.35f, 6.0f, 900.0f });
        add("Trance Gate",   FXKind::Gate,  1, (int)BeatDiv::D_1_16,0.9f,
            { 1.0f, 4.0f, 0.5f, 0.0f });
    }

    std::vector<FXPreset> presets_;
};

} // namespace onda::fx
