#pragma once

#include "../engine/DeckEngine.h"

namespace onda {

/// RT-side sync pass. Runs inside the audio callback between command
/// dispatch and deck rendering. Pure math on RT-owned state: no locks,
/// no allocation, no I/O.
///
/// The master deck's beat phase is *predicted* one block ahead so the
/// correction a follower applies is independent of deck render order.
/// A stopped master produces no corrections: followers hold their last
/// converged tempo (deterministic "master stopped = hold" behaviour).
struct SyncRtCore {
    int masterDeck { -1 };

    void apply(DeckEngine* decks, int numDecks, int blockFrames) noexcept {
        if (masterDeck < 0 || masterDeck >= numDecks) return;
        DeckEngine& m = decks[masterDeck];
        if (!m.hasGrid() || !m.isPlaying()) return;

        const double mBpm   = m.effBpm();
        const double mPhase = m.phaseAfter(blockFrames);
        if (mBpm <= 0.0) return; // master grid unreliable → don't corrupt followers
        for (int i = 0; i < numDecks; ++i)
            if (i != masterDeck)
                decks[i].applySyncCorrection(mBpm, mPhase, blockFrames);
    }
};

} // namespace onda
