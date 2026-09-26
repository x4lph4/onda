#include "Flx4Profile.h"

#include "../ParamNames.h"
#include "core/mixer/ParameterRegistry.h"
#include "Flx4Ids.h"

namespace onda::ctrl::flx4 {

static MidiAddr NA(int ch, int n) {
    return { AddrKind::Note, (uint8_t)ch, (uint8_t)n };
}
static MidiAddr CA(int ch, int c) {
    return { AddrKind::CC, (uint8_t)ch, (uint8_t)c };
}

static Mapping cmdMap(MidiAddr a, const char* name, MappingMode mode,
                      bool led) {
    Mapping m;
    m.in = a;
    m.target.kind = Target::Kind::Command;
    m.target.cmdName = name;   // resolved at applyProfile
    m.mode = mode;
    if (led) {
        m.out.addr = a;        // LED echo: same note, vel 0x7F/0x00
        m.out.hasCustom = true;
        m.out.onValue  = kLedOn;
        m.out.offValue = kLedOff;
    }
    return m;
}

static Mapping parMap(MidiAddr a, uint16_t param, float lo, float hi,
                      bool hiRes, Curve c = Curve::Linear) {
    Mapping m;
    m.in = a;
    m.target.kind = Target::Kind::Param;
    m.target.paramId = param;
    m.mode = MappingMode::Absolute;
    m.curve = c;
    m.outMin = lo;
    m.outMax = hi;
    m.hiRes = hiRes;
    return m;
}

ControllerProfile buildProfile() {
    ControllerProfile p;
    p.version = 1;
    p.meta.manufacturer = kManufacturer;
    p.meta.model        = kModel;
    p.meta.nameContains = kNameMatch;
    p.meta.author       = "onda (documented public sources only)";
    p.capabilities = CAP_JOG_WHEEL | CAP_TOUCH | CAP_PADS | CAP_FADERS |
                     CAP_EQ | CAP_FILTER | CAP_CROSSFADER |
                     CAP_FX_CONTROLS | CAP_LED_FEEDBACK;

    for (int d = 0; d < 2; ++d) {
        const int ch = kDeckCh(d);
        const std::string dn = std::string("deck") + char('a' + d);
        auto cmd = [&](int n, const char* tail, MappingMode md,
                       bool led) {
            p.mappings.push_back(cmdMap(
                NA(ch, n), (dn + "." + tail).c_str(), md, led));
        };
        cmd(N_PLAY,       "playToggle",     MappingMode::Toggle,    true);
        cmd(N_PLAY_SHIFT, "reverseToggle",  MappingMode::Momentary, false);
        cmd(N_CUE,        "cueTrigger",     MappingMode::Momentary, true);
        cmd(N_CUE_SHIFT,  "stop",           MappingMode::Momentary, false);
        cmd(N_SYNC,       "syncToggle",     MappingMode::Toggle,    true);
        cmd(N_SYNC_LONG,  "syncToggle",     MappingMode::Momentary, false);
        cmd(N_LOOP_IN,    "loopIn",         MappingMode::Momentary, false);
        cmd(N_LOOP_OUT,   "loopOut",        MappingMode::Momentary, false);
        cmd(N_4BEAT_EXIT, "loopToggle",     MappingMode::Toggle,    false);
        cmd(N_CALL_L,     "loopHalve",      MappingMode::Momentary, false);
        cmd(N_CALL_R,     "loopDouble",     MappingMode::Momentary, false);
        cmd(N_PFL,        "pflToggle",      MappingMode::Toggle,    true);
        cmd(N_QUANTIZE,   "quantizeToggle", MappingMode::Toggle,    false);
        cmd(N_JUMP_FWD,   "jumpFwd",        MappingMode::Momentary, false);
        cmd(N_JUMP_BACK,  "jumpBack",       MappingMode::Momentary, false);

        // 14-bit continuous — pct range is absolute percent; the
        // deck engine clamps to the active pitch range. Sign of
        // physical fader orientation is documented-pending.
        p.mappings.push_back(parMap(
            CA(ch, CC_TEMPO), deckParamId(d, kDeckParamPitch),
            -25.0f, 25.0f, true));
        p.mappings.push_back(parMap(
            CA(ch, CC_TRIM), mixer::ParameterRegistry::channelGain(d), 0.0f, 2.0f, true));
        p.mappings.push_back(parMap(
            CA(ch, CC_EQ_HI), mixer::ParameterRegistry::channelHigh(d), -26.0f, 6.0f,
            true));
        p.mappings.push_back(parMap(
            CA(ch, CC_EQ_MID), mixer::ParameterRegistry::channelMid(d), -26.0f, 6.0f,
            true));
        p.mappings.push_back(parMap(
            CA(ch, CC_EQ_LOW), mixer::ParameterRegistry::channelLow(d), -26.0f, 6.0f,
            true));
        p.mappings.push_back(parMap(
            CA(ch, CC_FADER), mixer::ParameterRegistry::channelFader(d), 0.0f, 1.0f,
            true));
    }

    // ---- mixer bus (ch 6) ----
    p.mappings.push_back(parMap(
        CA(kMixCh, CC_XF), mixer::ParameterRegistry::CROSSFADER,
        -1.0f, 1.0f, true));
    p.mappings.push_back(parMap(
        CA(kMixCh, CC_HEADMIX), mixer::ParameterRegistry::CUE_MIX,
        0.0f, 1.0f, true));
    p.mappings.push_back(parMap(
        CA(kMixCh, CC_CFX_A), mixer::ParameterRegistry::channelFilter(0), -1.0f, 1.0f,
        true));
    p.mappings.push_back(parMap(
        CA(kMixCh, CC_CFX_B), mixer::ParameterRegistry::channelFilter(1), -1.0f, 1.0f,
        true));

    // ---- browser load buttons (ch 6 notes) ----
    p.mappings.push_back(cmdMap(NA(kMixCh, N_LOAD_A), "decka.load",
                                MappingMode::Momentary, false));
    p.mappings.push_back(cmdMap(NA(kMixCh, N_LOAD_B), "deckb.load",
                                MappingMode::Momentary, false));

    // ---- beat-FX unit ----
    p.mappings.push_back(parMap(
        CA(kFxChA, CC_FX_DEPTH),
        mixer::ParameterRegistry::fxMix(
            mixer::ParameterRegistry::T_DECK_A, 0),
        0.0f, 1.0f, true));
    p.mappings.push_back(parMap(
        NA(kFxChA, N_FX_ON),
        mixer::ParameterRegistry::fxEnabled(
            mixer::ParameterRegistry::T_DECK_A, 0),
        0.0f, 1.0f, false));
    // NOTE-ON on/off is a button: use Toggle mode on the param.
    p.mappings.back().mode = MappingMode::Toggle;
    p.mappings.push_back(parMap(
        NA(kFxChB, N_FX_ON),
        mixer::ParameterRegistry::fxEnabled(
            mixer::ParameterRegistry::T_DECK_B, 0),
        0.0f, 1.0f, false));
    p.mappings.back().mode = MappingMode::Toggle;

    return p;
}

std::string installProfile(ProfileManager& pm, std::string& err) {
    ControllerProfile p = buildProfile();
    return pm.installProfile(p, err);
}

} // namespace onda::ctrl::flx4
