#pragma once

namespace onda::ui {

/// §2 — what the deck visualization is allowed to draw. Pure flags:
/// the engine consults them when building each frame, so turning a
/// layer off costs zero draw work (not just zero paint).
///
/// Visibility is layered, not per-marker: a deck can hide memory
/// cues while keeping hot cues, drop the beatgrid in a minimal
/// performance layout, etc. `perfMode` (§45) is the coarse degrade
/// switch — under sustained frame-cost the host flips it and the
/// engine sheds secondary detail automatically.
struct VisualizationState {
    bool showBeatgrid   = true;    // beat/bar/downbeat ticks
    bool showBarNumbers = true;    // bar labels (detail only)
    bool showCues       = true;    // any cue flag at all
    bool showHotCues    = true;    // type-0 flags
    bool showMemoryCues = true;    // type-1 flags
    bool showPrimaryCue = true;    // type-3 primary cue flag
    bool showLoops      = true;    // loop regions (active + saved)
    bool showRms        = true;    // inner RMS trace on columns
    bool showPhrases    = false;   // §30 seam — off until detected

    /// §45 — performance degrade: when true the frame builder omits
    /// beat ticks (bars stay), bar numbers, the RMS trace and phrase
    /// markers — the geometry a DJ actually reads while scratching
    /// stays. Hosts flip this on sustained buildMs or on explicit
    /// "performance mode" layouts.
    bool perfMode = false;
};

} // namespace onda::ui
