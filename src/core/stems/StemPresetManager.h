#pragma once

#include <map>
#include <string>

#include "../library/MiniJson.h"
#include "StemMixer.h"

namespace onda::stems {

/// ------------------------------------------------------------------
/// Named snapshots of a deck's stem mixing state (gain/mute/solo/pan
/// per stem). In-memory bank + JSON (de)serialisation so presets can
/// persist beside the library later — content is plain data, applying
/// one just pokes StemMixer params (which flow to RT via commands).
/// ------------------------------------------------------------------
class StemPresetManager {
public:
    static constexpr int kMaxPresets = 32;

    bool save(const std::string& name, const StemMixer& m) {
        if (name.empty()) return false;
        if (!bank_.count(name) && (int)bank_.size() >= kMaxPresets)
            return false;
        bank_[name] = m;
        return true;
    }
    bool apply(const std::string& name, StemMixer& m) const {
        auto it = bank_.find(name);
        if (it == bank_.end()) return false;
        m = it->second;
        return true;
    }
    bool remove(const std::string& name) { return bank_.erase(name) > 0; }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (auto& [k, v] : bank_) out.push_back(k);
        return out;
    }
    int count() const { return (int)bank_.size(); }

    std::string toJson() const {
        library::MiniJson::Array arr;
        for (auto& [name, m] : bank_) {
            library::MiniJson::Object o;
            o.emplace_back("name", name);
            library::MiniJson::Array stems;
            for (int i = 0; i < kStemMax; ++i) {
                const auto& p = m.param(i);
                library::MiniJson::Object s;
                s.emplace_back("stem", i);
                s.emplace_back("gain", (double)p.gain);
                s.emplace_back("pan", (double)p.pan);
                s.emplace_back("muted", p.muted);
                s.emplace_back("solo", p.solo);
                stems.emplace_back(library::MiniJson(std::move(s)));
            }
            o.emplace_back("stems", library::MiniJson(std::move(stems)));
            arr.emplace_back(library::MiniJson(std::move(o)));
        }
        return library::MiniJson(std::move(arr)).dump();
    }

    bool fromJson(const std::string& js) {
        const auto v = library::MiniJson::parse(js);
        if (!v.isArr()) return false;
        for (auto& e : v.arr()) {
            const std::string name = e.at("name").str();
            StemMixer m;
            for (auto& s : e.at("stems").arr()) {
                const int i = (int)s.at("stem").num();
                if (i < 0 || i >= kStemMax) continue;
                m.setGain(i, (float)s.at("gain").num());
                m.setPan(i, (float)s.at("pan").num());
                m.setMute(i, s.at("muted").boolean());
                m.setSolo(i, s.at("solo").boolean());
            }
            if (!name.empty()) bank_[name] = m;
        }
        return true;
    }

private:
    std::map<std::string, StemMixer> bank_;
};

} // namespace onda::stems
