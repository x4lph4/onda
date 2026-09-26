#include "Flx4Inventory.h"
#include "Flx4Ids.h"

namespace onda::ctrl::flx4 {

static MidiAddr NA(int ch, int n) {
    return { AddrKind::Note, (uint8_t)ch, (uint8_t)n };
}
static MidiAddr CA(int ch, int c) {
    return { AddrKind::CC, (uint8_t)ch, (uint8_t)c };
}
static const MidiAddr kNone{};

const std::vector<Flx4Control>& inventory() {
    static const std::vector<Flx4Control> t = [] {
        std::vector<Flx4Control> v;
        auto push = [&](std::string id, const char* lbl, int g,
                        MidiAddr a, std::string tgt, bool fb,
                        const char* src, bool ver) {
            v.push_back({ std::move(id), lbl, g, a, std::move(tgt),
                          fb, src, ver });
        };

        for (int d = 0; d < 2; ++d) {
            const std::string p  = d ? "deck1" : "deck0";
            const std::string dn = d ? "deckb" : "decka";
            const int ch = kDeckCh(d);
            const int g  = d ? G_DECK_B : G_DECK_A;
            push(p+".play",      "PLAY/PAUSE",   g, NA(ch,N_PLAY),
                 "cmd:"+dn+".playToggle", true, "OFF+MIX", false);
            push(p+".playShift", "PLAY/PAUSE +SHIFT", g, NA(ch,N_PLAY_SHIFT),
                 "cmd:"+dn+".reverseToggle", false, "OFF+MIX", false);
            push(p+".cue",       "CUE",          g, NA(ch,N_CUE),
                 "cmd:"+dn+".cueTrigger", true, "OFF+MIX", false);
            push(p+".cueShift",  "CUE +SHIFT",   g, NA(ch,N_CUE_SHIFT),
                 "cmd:"+dn+".stop", false, "OFF+MIX", false);
            push(p+".shift",     "SHIFT",        g, NA(ch,N_SHIFT),
                 "plugin:shift", false, "OFF+MIX", false);
            push(p+".sync",      "BEAT SYNC",    g, NA(ch,N_SYNC),
                 "cmd:"+dn+".syncToggle", true, "OFF+MIX", false);
            push(p+".syncLong",  "BEAT SYNC long", g, NA(ch,N_SYNC_LONG),
                 "cmd:"+dn+".syncToggle", false, "MIX", false);
            push(p+".tempoRange","TEMPO RANGE (+SHIFT)", g, NA(ch,N_TEMPO_RANGE),
                 "plugin:rangeCycle", false, "MIX", false);
            push(p+".loopIn",    "IN",           g, NA(ch,N_LOOP_IN),
                 "cmd:"+dn+".loopIn", false, "MIX+SLP", true);
            push(p+".loopOut",   "OUT",          g, NA(ch,N_LOOP_OUT),
                 "cmd:"+dn+".loopOut", false, "MIX+SLP", true);
            push(p+".beat4exit", "4BEAT/EXIT",   g, NA(ch,N_4BEAT_EXIT),
                 "cmd:"+dn+".loopToggle", false, "MIX+SLP", true);
            push(p+".callLeft",  "CUE/LOOP CALL <", g, NA(ch,N_CALL_L),
                 "cmd:"+dn+".loopHalve", false, "MIX+SLP", true);
            push(p+".callRight", "CUE/LOOP CALL >", g, NA(ch,N_CALL_R),
                 "cmd:"+dn+".loopDouble", false, "MIX+SLP", true);
            push(p+".reloopStop","CUE/LOOP CALL (memory)", g, NA(ch,N_RELOOP_STOP),
                 "unmapped", false, "MIX", false);
            push(p+".loopAdjIn", "LOOP ADJUST IN", g, NA(ch,N_LOOPADJ_IN),
                 "unmapped", false, "MIX", false);
            push(p+".loopAdjOut","LOOP ADJUST OUT", g, NA(ch,N_LOOPADJ_OUT),
                 "unmapped", false, "MIX", false);
            push(p+".pfl",       "CH CUE",       g, NA(ch,N_PFL),
                 "cmd:"+dn+".pflToggle", false, "MIX", false);
            push(p+".quantize",  "QUANTIZE (+SHIFT)", g, NA(ch,N_QUANTIZE),
                 "cmd:"+dn+".quantizeToggle", false, "MIX", false);
            push(p+".jumpFwd",   "BEAT JUMP FWD (loop sect.)", g,
                 NA(ch,N_JUMP_FWD), "cmd:"+dn+".jumpFwd", false,
                 "MIX", false);
            push(p+".jumpBack",  "BEAT JUMP BACK (loop sect.)", g,
                 NA(ch,N_JUMP_BACK), "cmd:"+dn+".jumpBack", false,
                 "MIX", false);
            // jog family — plugin-owned semantics
            push(p+".jogTouch",  "JOG touch (platter top)", g,
                 NA(ch,N_JOG_TOUCH), "plugin:jogTouch", false,
                 "OFF+MIX", false);
            push(p+".jogTouchSh","JOG touch +SHIFT", g,
                 NA(ch,N_JOG_TOUCH_SH), "plugin:jogTouch", false,
                 "MIX", false);
            push(p+".jogSide",   "JOG wheel side", g, CA(ch,CC_JOG_SIDE),
                 "plugin:jog", false, "OFF+MIX", false);
            push(p+".jogVinyl",  "JOG platter (vinyl)", g, CA(ch,CC_JOG_VON),
                 "plugin:jog", false, "OFF+MIX", false);
            push(p+".jogCdj",    "JOG platter (cdj)", g, CA(ch,CC_JOG_VOFF),
                 "plugin:jog", false, "OFF+MIX", false);
            push(p+".jogSearch", "JOG +SHIFT (search)", g, CA(ch,CC_JOG_SEARCH),
                 "plugin:jog", false, "OFF+SLP", true);
            // 14-bit continuous
            push(p+".tempo",     "TEMPO fader", g, CA(ch,CC_TEMPO),
                 "param:DECK_"+std::to_string(d)+"_PITCH", false,
                 "OFF+MIX+SLP", true);
            push(p+".trim",      "TRIM",        g, CA(ch,CC_TRIM),
                 "param:CH_"+std::to_string(d)+"_GAIN", false,
                 "OFF+MIX", false);
            push(p+".eqHi",      "EQ HI",       g, CA(ch,CC_EQ_HI),
                 "param:CH_"+std::to_string(d)+"_HIGH", false,
                 "OFF+MIX", false);
            push(p+".eqMid",     "EQ MID",      g, CA(ch,CC_EQ_MID),
                 "param:CH_"+std::to_string(d)+"_MID", false,
                 "OFF+MIX", false);
            push(p+".eqLow",     "EQ LOW",      g, CA(ch,CC_EQ_LOW),
                 "param:CH_"+std::to_string(d)+"_LOW", false,
                 "OFF+MIX", false);
            push(p+".fader",     "CH FADER",    g, CA(ch,CC_FADER),
                 "param:CH_"+std::to_string(d)+"_FADER", true,
                 "OFF+MIX", false);
            push(p+".vu",        "VU meter",    g, CA(ch,CC_VU),
                 "unmapped:led-only", true, "MIX", false);
            // pad-mode selectors
            const struct { const char* n; uint8_t b; } pm[] = {
                {"hotcue",N_PM_HOTCUE},{"padfx1",N_PM_PADFX1},
                {"beatjump",N_PM_BEATJUMP},{"sampler",N_PM_SAMPLER},
                {"keyboard",N_PM_KEYBOARD},{"padfx2",N_PM_PADFX2},
                {"beatloop",N_PM_BEATLOOP},{"keyshift",N_PM_KEYSHIFT}};
            for (auto& m : pm)
                push(p+".padmode."+m.n, "PAD MODE btn", g, NA(ch,m.b),
                     "plugin:padmode", true, "OFF+MIX+SLP",
                     m.b==N_PM_KEYBOARD || m.b==N_PM_HOTCUE ||
                     m.b==N_PM_PADFX1);
            // pads (both layers share bank ranges)
            for (int layer = 0; layer < 2; ++layer) {
                const int pc = layer ? kPadChShift(d) : kPadCh(d);
                for (int i = 0; i < 8; ++i) {
                    push(p+".pad"+std::to_string(i+1)+
                         (layer?".shift":""),
                         "PERFORMANCE PAD", G_PADS,
                         NA(pc,(uint8_t)i), "plugin:pad", true,
                         "OFF+MIX", false);
                }
            }
        }

        // ---- mixer / browser / master (ch 6) ----
        push("mix.crossfader", "CROSSFADER", G_MIXER, CA(kMixCh,CC_XF),
             "param:CROSSFADER", false, "OFF+MIX", false);
        push("mix.headmix",   "HEADPHONES MIX", G_MIXER,
             CA(kMixCh,CC_HEADMIX), "param:CUE_MIX", false,
             "OFF+MIX", false);
        push("mix.cfxA",      "SMART CFX deck A", G_MIXER,
             CA(kMixCh,CC_CFX_A), "param:CH_0_FILTER", false,
             "SLP+MIX", false);
        push("mix.cfxB",      "SMART CFX deck B", G_MIXER,
             CA(kMixCh,CC_CFX_B), "param:CH_1_FILTER", false,
             "SLP+MIX", false);
        push("util.browse",   "BROWSE rotary", G_UTILITY,
             CA(kMixCh,CC_BROWSE), "plugin:browse", false,
             "OFF+MIX", false);
        push("util.browseZoom","BROWSE rotary +SHIFT", G_UTILITY,
             CA(kMixCh,CC_BROWSE_Z), "unmapped", false, "MIX", false);
        push("util.focusFwd", "BROWSE press fwd", G_UTILITY,
             NA(kMixCh,N_BROWSE_FWD), "unmapped", false, "MIX", false);
        push("util.focusBck", "BROWSE press back", G_UTILITY,
             NA(kMixCh,N_BROWSE_BCK), "unmapped", false, "MIX", false);
        push("util.loadA",    "LOAD deck A", G_UTILITY,
             NA(kMixCh,N_LOAD_A), "cmd:decka.load", false,
             "OFF+MIX", false);
        push("util.loadB",    "LOAD deck B", G_UTILITY,
             NA(kMixCh,N_LOAD_B), "cmd:deckb.load", false,
             "OFF+MIX", false);
        push("util.xfStart",  "XF-start notes", G_UTILITY,
             NA(kMixCh,N_FSTART_PLAY), "unmapped", false,
             "OFF", false);
        push("mix.masterLevel","MASTER LEVEL", G_MIXER, kNone,
             "unmapped:unknown-msg", false, "UNVERIFIED", false);
        push("mix.masterCue", "MASTER CUE", G_MIXER, kNone,
             "unmapped:unknown-msg", false, "UNVERIFIED", false);
        push("mix.mic",       "MIC input", G_MIXER, kNone,
             "unmapped:not-usb-routed", false, "MIX-doc", true);

        // ---- beat-FX unit ----
        push("fx.chSelA",     "FX CH SELECT A", G_FX, NA(kFxChA,N_FX_CHSEL_A),
             "unmapped", false, "OFF+MIX", false);
        push("fx.chSelB",     "FX CH SELECT B", G_FX, NA(kFxChB,N_FX_CHSEL_B),
             "unmapped", false, "OFF+MIX", false);
        push("fx.select",     "FX SELECT", G_FX, NA(kFxChA,N_FX_SELECT),
             "unmapped", false, "MIX", false);
        push("fx.selectShift","FX SELECT +SHIFT", G_FX, NA(kFxChA,N_FX_SEL_SH),
             "unmapped", false, "MIX", false);
        push("fx.beatLeft",   "BEAT <", G_FX, NA(kFxChA,N_FX_BEAT_L),
             "unmapped", false, "MIX", false);
        push("fx.beatRight",  "BEAT >", G_FX, NA(kFxChA,N_FX_BEAT_R),
             "unmapped", false, "MIX", false);
        push("fx.depth",      "LEVEL/DEPTH", G_FX, CA(kFxChA,CC_FX_DEPTH),
             "param:FX_DECK_A_0_MIX", false, "OFF+MIX", false);
        push("fx.onA",        "FX ON/OFF A", G_FX, NA(kFxChA,N_FX_ON),
             "param:FX_DECK_A_0_ENABLED", false, "OFF+MIX", false);
        push("fx.onB",        "FX ON/OFF B", G_FX, NA(kFxChB,N_FX_ON),
             "param:FX_DECK_B_0_ENABLED", false, "OFF+MIX", false);
        push("fx.onShift",    "FX ON/OFF +SHIFT", G_FX, NA(kFxChA,N_FX_ON_SH),
             "unmapped", false, "MIX", false);
        return v;
    }();
    return t;
}

std::string controlNameFor(const MidiAddr& a) {
    for (const auto& c : inventory()) {
        if (c.in.pack() == a.pack()) return c.id;
        // 14-bit LSB halves report the MSB control
        if (c.in.kind == AddrKind::CC &&
            c.in.channel == a.channel &&
            (uint8_t)(c.in.number + 0x20) == a.number &&
            a.kind == AddrKind::CC)
            return c.id + ".lsb";
    }
    // pad banks: any note in a documented bank range on pad channels
    if (a.kind == AddrKind::Note && a.channel >= 7 && a.channel <= 10) {
        const int deck = (a.channel - 7) / 2;
        const char* bank = nullptr;
        const uint8_t b = a.number & 0xF0;
        if (b == kPadBankHotCue)        bank = "hotcue";
        else if (b == kPadBankBeatJump) bank = "beatjump";
        else if (b == kPadBankSampler)  bank = "sampler";
        else if (b == kPadBankKeyboard) bank = "keyboard";
        else if (b == kPadBankBeatLoop) bank = "beatloop";
        else if (b == kPadBankKeyShift) bank = "keyshift";
        if (bank)
            return std::string(deck ? "deck1" : "deck0") + ".pad." +
                   bank + std::to_string(a.number & 0x0F) +
                   ((a.channel & 1) == 0 ? ".shift" : "");
    }
    return "";
}

} // namespace onda::ctrl::flx4
