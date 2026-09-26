#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ControllerProfile.h"

namespace onda::ctrl {

/// Owns the profile library: installed profiles on disk, the loaded
/// set per device, import/export. Profiles are pure data — this class
/// only validates + stores + retrieves them.
///
/// Layout: `dir/<profileId>.json`. profileId = "<manufacturer>-<model>"
/// sanitized. The "loaded" map holds per-device assignments.
class ProfileManager {
public:
    explicit ProfileManager(std::string dir) : dir_(std::move(dir)) {}

    const std::string& dir() const { return dir_; }

    // ---- library ops ----
    /// Validate + write to the library. Returns profileId or "" on
    /// failure (err filled).
    std::string installProfile(const ControllerProfile& p,
                               std::string& err);
    /// Read + validate a file from anywhere (import).
    bool importProfile(const std::string& path, std::string& err);
    bool exportProfile(const std::string& profileId,
                       const std::string& destPath,
                       std::string& err) const;
    bool deleteProfile(const std::string& profileId, std::string& err);

    /// All installed profile ids.
    std::vector<std::string> listProfiles() const;

    /// Load profile data from the library.
    bool loadProfileData(const std::string& profileId,
                         ControllerProfile& out,
                         std::string& err) const;

    // ---- per-device assignment ----
    /// Assign the active profile to a device (loads into memory).
    bool assignProfile(uint32_t deviceId, const std::string& profileId,
                       std::string& err);
    void unloadProfile(uint32_t deviceId);
    const ControllerProfile* profileFor(uint32_t deviceId) const;
    std::string profileIdFor(uint32_t deviceId) const;

    /// Auto-match: first installed profile whose nameContains hits.
    std::string autoMatch(const std::string& osDeviceName,
                          std::string& err);

    /// Reset: clear assignments + in-memory cache (files untouched).
    void resetProfiles();

    static std::string idFor(const ControllerProfile& p);
    static std::string sanitize(const std::string& s);

private:
    std::string dir_;
    std::unordered_map<std::string, ControllerProfile> installed_;
    std::unordered_map<uint32_t, std::string> assigned_;
    bool scanned_ = false;
    void rescan();
};

} // namespace onda::ctrl
