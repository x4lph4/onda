#include "ProfileManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace onda::ctrl {
namespace fs = std::filesystem;

std::string ProfileManager::sanitize(const std::string& s) {
    std::string o;
    for (const char c : s)
        o += (std::isalnum((unsigned char)c) || c == '-' || c == '_')
             ? c : '_';
    return o.empty() ? "profile" : o;
}

std::string ProfileManager::idFor(const ControllerProfile& p) {
    return sanitize(p.meta.manufacturer.empty()
                    ? "generic" : p.meta.manufacturer)
           + "-" + sanitize(p.meta.model.empty()
                            ? "controller" : p.meta.model);
}

void ProfileManager::rescan() {
    scanned_ = true;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return;
    for (auto& de : fs::directory_iterator(dir_, ec)) {
        if (!de.is_regular_file() ||
            de.path().extension() != ".json") continue;
        ControllerProfile p;
        std::string err;
        const std::string id = de.path().stem().string();
        if (loadProfileData(id, p, err))
            installed_[id] = std::move(p);
    }
}

std::vector<std::string> ProfileManager::listProfiles() const {
    std::vector<std::string> out;
    for (auto& [id, p] : installed_) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

std::string ProfileManager::installProfile(const ControllerProfile& p,
                                           std::string& err) {
    err = p.validate();
    if (!err.empty()) return "";
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) { err = "cannot create profile dir"; return ""; }
    const std::string id = idFor(p);
    const fs::path fp = fs::path(dir_) / (id + ".json");
    FILE* f = std::fopen(fp.string().c_str(), "wb");
    if (!f) { err = "cannot write " + fp.string(); return ""; }
    const std::string j = p.toJson();
    std::fwrite(j.data(), 1, j.size(), f);
    std::fclose(f);
    installed_[id] = p;
    return id;
}

bool ProfileManager::loadProfileData(const std::string& profileId,
                                     ControllerProfile& out,
                                     std::string& err) const {
    const fs::path fp = fs::path(dir_) / (profileId + ".json");
    FILE* f = std::fopen(fp.string().c_str(), "rb");
    if (!f) { err = "cannot read " + fp.string(); return false; }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) {           // 4 MB sanity cap
        std::fclose(f); err = "bad profile size"; return false; }
    std::string text((size_t)n, '\0');
    const size_t rd = std::fread(text.data(), 1, (size_t)n, f);
    std::fclose(f);
    text.resize(rd);
    return out.fromJson(text, err);
}

bool ProfileManager::importProfile(const std::string& path,
                                   std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot read " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) {
        std::fclose(f); err = "bad profile size"; return false; }
    std::string text((size_t)n, '\0');
    text.resize(std::fread(text.data(), 1, (size_t)n, f));
    std::fclose(f);
    ControllerProfile p;
    if (!p.fromJson(text, err)) return false;
    return !installProfile(p, err).empty();
}

bool ProfileManager::exportProfile(const std::string& profileId,
                                   const std::string& destPath,
                                   std::string& err) const {
    ControllerProfile p;
    if (!loadProfileData(profileId, p, err)) return false;
    FILE* f = std::fopen(destPath.c_str(), "wb");
    if (!f) { err = "cannot write " + destPath; return false; }
    const std::string j = p.toJson();
    std::fwrite(j.data(), 1, j.size(), f);
    std::fclose(f);
    return true;
}

bool ProfileManager::deleteProfile(const std::string& profileId,
                                   std::string& err) {
    const fs::path fp = fs::path(dir_) / (profileId + ".json");
    std::error_code ec;
    installed_.erase(profileId);
    for (auto it = assigned_.begin(); it != assigned_.end();)
        it = it->second == profileId ? assigned_.erase(it)
                                     : std::next(it);
    if (!fs::remove(fp, ec) || ec) {
        err = "cannot delete " + fp.string();
        return false;
    }
    return true;
}

bool ProfileManager::assignProfile(uint32_t deviceId,
                                   const std::string& profileId,
                                   std::string& err) {
    if (!scanned_) rescan();
    if (installed_.find(profileId) == installed_.end()) {
        ControllerProfile p;
        if (!loadProfileData(profileId, p, err)) return false;
        installed_[profileId] = std::move(p);
    }
    assigned_[deviceId] = profileId;
    return true;
}

void ProfileManager::unloadProfile(uint32_t deviceId) {
    assigned_.erase(deviceId);
}

const ControllerProfile*
ProfileManager::profileFor(uint32_t deviceId) const {
    auto a = assigned_.find(deviceId);
    if (a == assigned_.end()) return nullptr;
    auto p = installed_.find(a->second);
    return p == installed_.end() ? nullptr : &p->second;
}

std::string ProfileManager::profileIdFor(uint32_t deviceId) const {
    auto a = assigned_.find(deviceId);
    return a == assigned_.end() ? std::string() : a->second;
}

std::string ProfileManager::autoMatch(const std::string& osDeviceName,
                                      std::string& err) {
    if (!scanned_) rescan();
    for (auto& [id, p] : installed_)
        if (p.matchesDevice(osDeviceName)) return id;
    err = "no profile matches '" + osDeviceName + "'";
    return "";
}

void ProfileManager::resetProfiles() {
    assigned_.clear();
    installed_.clear();
    scanned_ = false;
}

} // namespace onda::ctrl
