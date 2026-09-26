#pragma once

#include "../rt/CommandQueue.h"
#include "DeckEngine.h"

namespace onda {

/// Apply a concrete deck-scoped command payload to a DeckEngine.
/// Shared by the RT callback (AudioEngine) and by tests that drive
/// decks directly — guarantees identical command semantics in both
/// worlds. Returns false when the command doesn't target `deck`
/// or isn't a deck-scoped command.
template <class C>
bool applyDeckCommand(DeckEngine& d, int deck, const C& c) noexcept {
    if constexpr (requires { c.deck; })
        if ((int)c.deck != deck) return false;

    if constexpr      (std::is_same_v<C, rt::DeckTransport>) {
        if (c.action == 1) d.setPlaying(true);
        else if (c.action == 0) d.setPlaying(false);
        else d.stop();
    }
    else if constexpr (std::is_same_v<C, rt::DeckSeek>)      d.seekSec(c.sec);
    else if constexpr (std::is_same_v<C, rt::DeckSeekFrame>) d.seekFrame(c.frame);
    else if constexpr (std::is_same_v<C, rt::DeckUnload>)    d.unloadTrack();
    else if constexpr (std::is_same_v<C, rt::SetDeckGain>)   d.setGain(c.gain);
    else if constexpr (std::is_same_v<C, rt::SetTrackGain>)  d.setTrackGain(c.gain);
    else if constexpr (std::is_same_v<C, rt::SetPitch>)      d.setPitchPercent(c.pct);
    else if constexpr (std::is_same_v<C, rt::SetPitchRange>) d.setPitchRange(c.pct);
    else if constexpr (std::is_same_v<C, rt::SetPitchBend>)  d.setPitchBend(c.pct);
    else if constexpr (std::is_same_v<C, rt::SetKeyLock>)    d.setKeyLock(c.on);
    else if constexpr (std::is_same_v<C, rt::SetReverse>)    d.setReverse(c.on);
    else if constexpr (std::is_same_v<C, rt::SetQuantize>)   d.setQuantize(c.on);
    else if constexpr (std::is_same_v<C, rt::SetLoop>)       d.setLoop(c.inSec, c.outSec);
    else if constexpr (std::is_same_v<C, rt::SetLoopFrames>) d.setLoopFrames(c.inFrame, c.outFrame);
    else if constexpr (std::is_same_v<C, rt::SetLoopOn>)     d.setLoopEnabled(c.on);
    else if constexpr (std::is_same_v<C, rt::SetLoopSize>)   d.setLoopSizeSec(c.sizeSec);
    else if constexpr (std::is_same_v<C, rt::SetEqBand>)     d.setEqDb(c.band, c.db);
    else if constexpr (std::is_same_v<C, rt::SetFilter>)     d.setFilterPos(c.pos);
    else if constexpr (std::is_same_v<C, rt::SyncSetMode>)   d.setSyncMode((int)c.mode);
    else if constexpr (std::is_same_v<C, rt::SlipBegin>)     d.beginSlip();
    else if constexpr (std::is_same_v<C, rt::SlipEnd>)       d.endSlip();
    else return false; // not a deck-scoped command
    return true;
}

/// Variant convenience wrapper.
inline bool applyDeckCommand(DeckEngine& d, int deck,
                             const rt::Command& cmd) noexcept {
    return std::visit([&](const auto& c) {
        return applyDeckCommand(d, deck, c);
    }, cmd);
}

} // namespace onda
