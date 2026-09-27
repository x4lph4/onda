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
    /// failure (err filled). Marks the profile origin="user" — user
    /// files live on disk and are safe to edit/delete (§48).
    std::string installProfile(const ControllerProfile& p,
                               std::string& err);
    /// §48 factory install: registers the profile in memory ONLY —
    /// built-in mappings ship inside the binary, are never written
    /// to the user dir, and can never be overwritten or deleted
    /// through the library. User edits go through duplicateProfile().
    std::string installFactory(const ControllerProfile& p,
                               std::string& err);
    /// §47 duplicate: copy `profileId` under a new model name as a
    /// user profile. Works on factory profiles too (the original
    /// stays untouched — this is how users customize factory maps).
    std::string duplicateProfile(const std::string& profileId,
                                 const std::string& newModel,
                                 std::string& err);
    /// §47 rename: user profiles only — writes under the new id and
    /// deletes the old file. Refuses factory profiles.
    bool renameProfile(const std::string& profileId,
                       const std::string& newModel,
                       std::string& err);
    /// §47 reset-to-factory: removes a user profile, letting any
    /// same-id factory registration resurface. No-op otherwise.
    void resetToFactory(const std::string& profileId);
    /// origin of an installed profile ("factory"/"user"/"" if absent)
    std::string originOf(const std::string& profileId) const;
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
