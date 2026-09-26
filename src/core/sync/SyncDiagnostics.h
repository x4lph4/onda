#pragma once

#include <cstdio>
#include <string>

#include "SyncTypes.h"

namespace onda::sync {

/// Copyable sync snapshot (§39): everything needed to diagnose a sync
/// problem without attaching a debugger.
struct SyncDiagnostics {
    int    masterDeck     { -1 };
    double masterBpm      { 0.0 };
    int    deck           { -1 };
    double deckBpm        { 0.0 };   // effective BPM now
    double targetBpm      { 0.0 };   // what sync chases
    double beat           { -1.0 };
    double phase          { 0.0 };
    double gridOffsetBeats{ 0.0 };   // this deck's beat vs master beat
    double tempoErrPct    { 0.0 };   // |effBpm - target| / target
    double phaseErrMs     { 0.0 };   // RT-measured phase error
    float  confidence     { 0.0f };  // §30
    SyncStatus status     { SyncStatus::Off };
    bool   limited        { false };

    static const char* statusName(SyncStatus s) {
        switch (s) {
            case SyncStatus::Synced:           return "SYNCED";
            case SyncStatus::Syncing:          return "SYNCING";
            case SyncStatus::NoMaster:         return "NO MASTER";
            case SyncStatus::NoGrid:           return "NO GRID";
            case SyncStatus::NoGridMaster:     return "NO MASTER GRID";
            case SyncStatus::UnreliableMaster: return "UNRELIABLE MASTER";
            case SyncStatus::OutOfRange:       return "RANGE LIMIT";
            case SyncStatus::DeckStopped:      return "DECK STOPPED";
            default:                           return "OFF";
        }
    }

    std::string toText() const {
        char b[512];
        std::snprintf(b, sizeof b,
            "master=%s %.2f BPM | deck %c: %.2f -> %.2f BPM | "
            "beat %.2f phase %.2f | grid offset %+.3f beats | "
            "tempo err %.2f%% | phase err %.1f ms | conf %.0f%% | %s%s",
            masterDeck >= 0 ? std::string(1, char('A' + masterDeck)).c_str()
                            : "-",
            masterBpm, deck >= 0 ? char('A' + deck) : '-',
            deckBpm, targetBpm, beat, phase, gridOffsetBeats,
            tempoErrPct * 100.0, phaseErrMs, confidence * 100.0f,
            statusName(status), limited ? " [LIMITED]" : "");
        return b;
    }
};

} // namespace onda::sync
