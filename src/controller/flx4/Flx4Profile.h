#pragma once

#include <string>

#include "../ControllerProfile.h"
#include "../ProfileManager.h"

namespace onda::ctrl::flx4 {

/// Builds the versioned DDJ-FLX4 controller profile from the
/// documented inventory (Flx4Ids/Flx4Inventory). The profile is pure
/// declarative data — every entry names a command or parameter that
/// the existing engine ports resolve.
ControllerProfile buildProfile();

/// Convenience: install into a ProfileManager. Returns profile id
/// ("Pioneer_DJ-DDJ-FLX4") or "" with err set.
std::string installProfile(ProfileManager& pm, std::string& err);

} // namespace onda::ctrl::flx4
