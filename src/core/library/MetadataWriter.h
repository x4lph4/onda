#pragma once

#include <functional>
#include <string>

#include "LibraryTypes.h"
#include "TrackRepository.h"

namespace onda::library {

/// §18-19 — explicit file-metadata writes.
///
/// Reading is automatic (import); writing is NEVER automatic — this
/// surface exists so the "write tags to file" gesture is a deliberate
/// user action routed through one auditable path.
///
/// The codec work is injected (`Writer` = path + editable fields →
/// bool) so this header stays codec-free like TrackImporter; the app
/// plugs a writer for whichever metadata library is available. When
/// no writer is configured, every call fails cleanly — a library
/// without write support never silently pretends to write.
///
/// On success the DB row updates AND the matching `editedFields`
/// bits are set, so later rescans preserve user intent (§18 priority:
/// user-edited > embedded > database fallback).
class MetadataWriter {
public:
    /// field names honoured: title/artist/album/album_artist/genre/
    /// year/track_num/disc_num/label/composer/comments
    using Writer =
        std::function<bool(const std::string& path,
                           const Track& fields)>;

    explicit MetadataWriter(TrackRepository& repo) : repo_(repo) {}

    void setWriter(Writer w) { writer_ = std::move(w); }
    bool canWrite() const { return (bool)writer_; }

    /// Write the editable fields of `t` to its file, then persist the
    /// same values + edited mask into the DB row so file and
    /// collection agree. Returns false without a writer or when the
    /// file write itself fails (DB stays untouched then — never claim
    /// a write that didn't happen).
    bool writeBack(int64_t trackId, const Track& t,
                   uint32_t editedMask) {
        if (!writer_ || editedMask == 0) return false;
        Track cur;
        if (!repo_.getById(trackId, cur)) return false;
        if (!writer_(cur.path, t)) return false;
        return repo_.setMetadataFields(trackId, t, editedMask);
    }

private:
    TrackRepository& repo_;
    Writer           writer_;
};

} // namespace onda::library
