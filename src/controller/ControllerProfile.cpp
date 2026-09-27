#include "ControllerProfile.h"

#include <algorithm>
#include <cctype>

#include "MiniJson.h"
#include "ParamNames.h"
#include "MappingEngine.h"

namespace onda::ctrl {

const char* modeName(MappingMode m) {
    switch (m) {
        case MappingMode::Toggle:    return "toggle";
        case MappingMode::Momentary: return "momentary";
        case MappingMode::Absolute:  return "absolute";
        case MappingMode::Relative:  return "relative";
        case MappingMode::IncDec:    return "incdec";
        case MappingMode::Pickup:    return "pickup";
    }
    return "?";
}
MappingMode modeFromName(const std::string& s, bool& ok) {
    ok = true;
    if (s == "toggle")    return MappingMode::Toggle;
    if (s == "momentary") return MappingMode::Momentary;
    if (s == "absolute")  return MappingMode::Absolute;
    if (s == "relative")  return MappingMode::Relative;
    if (s == "incdec")    return MappingMode::IncDec;
    if (s == "pickup")    return MappingMode::Pickup;
    ok = false; return MappingMode::Absolute;
}
const char* curveName(Curve c) {
    switch (c) {
        case Curve::Linear:  return "linear";
        case Curve::Inverse: return "inverse";
        case Curve::Expo:    return "expo";
        case Curve::Log:     return "log";
    }
    return "?";
}
Curve curveFromName(const std::string& s, bool& ok) {
    ok = true;
    if (s == "linear")  return Curve::Linear;
    if (s == "inverse") return Curve::Inverse;
    if (s == "expo")    return Curve::Expo;
    if (s == "log")     return Curve::Log;
    ok = false; return Curve::Linear;
}
const char* encoderName(EncoderMode e) {
    switch (e) {
        case EncoderMode::Absolute:          return "absolute";
        case EncoderMode::RelBinaryOffset:   return "rel64";
        case EncoderMode::RelTwosComplement: return "rel2s";
        case EncoderMode::RelSignMagnitude:  return "relsm";
    }
    return "?";
}
EncoderMode encoderFromName(const std::string& s, bool& ok) {
    ok = true;
    if (s == "absolute") return EncoderMode::Absolute;
    if (s == "rel64")    return EncoderMode::RelBinaryOffset;
    if (s == "rel2s")    return EncoderMode::RelTwosComplement;
    if (s == "relsm")    return EncoderMode::RelSignMagnitude;
    ok = false; return EncoderMode::Absolute;
}
const char* addrKindName(AddrKind k) {
    switch (k) {
        case AddrKind::Note:            return "note";
        case AddrKind::CC:              return "cc";
        case AddrKind::PitchBend:       return "pitchbend";
        case AddrKind::ChannelPressure: return "pressure";
        case AddrKind::Program:         return "program";
    }
    return "?";
}
AddrKind addrKindFromName(const std::string& s, bool& ok) {
    ok = true;
    if (s == "note")      return AddrKind::Note;
    if (s == "cc")        return AddrKind::CC;
    if (s == "pitchbend") return AddrKind::PitchBend;
    if (s == "pressure")  return AddrKind::ChannelPressure;
    if (s == "program")   return AddrKind::Program;
    ok = false; return AddrKind::Note;
}
const char* jogModeName(JogMode j) {
    switch (j) {
        case JogMode::JogSeek:      return "seek";
        case JogMode::JogPitchBend: return "pitchbend";
        case JogMode::JogScratch:   return "scratch";
    }
    return "?";
}
JogMode jogModeFromName(const std::string& s, bool& ok) {
    ok = true;
    if (s == "seek")      return JogMode::JogSeek;
    if (s == "pitchbend") return JogMode::JogPitchBend;
    if (s == "scratch")   return JogMode::JogScratch;
    ok = false; return JogMode::JogSeek;
}

const char* capName(uint32_t bit) {
    switch (bit) {
        case CAP_JOG_WHEEL:    return "JOG_WHEEL";
        case CAP_TOUCH:        return "TOUCH";
        case CAP_PADS:         return "PADS";
        case CAP_FADERS:       return "FADERS";
        case CAP_EQ:           return "EQ";
        case CAP_FILTER:       return "FILTER";
        case CAP_CROSSFADER:   return "CROSSFADER";
        case CAP_FX_CONTROLS:  return "FX_CONTROLS";
        case CAP_LED_FEEDBACK: return "LED_FEEDBACK";
        case CAP_DISPLAY:      return "DISPLAY";
        case CAP_MIDI_CLOCK:   return "MIDI_CLOCK";
        case CAP_HID:          return "HID";
    }
    return "?";
}
uint32_t capFromName(const std::string& s) {
    static const uint32_t bits[] = {
        CAP_JOG_WHEEL, CAP_TOUCH, CAP_PADS, CAP_FADERS, CAP_EQ,
        CAP_FILTER, CAP_CROSSFADER, CAP_FX_CONTROLS, CAP_LED_FEEDBACK,
        CAP_DISPLAY, CAP_MIDI_CLOCK, CAP_HID };
    for (uint32_t b : bits)
        if (s == capName(b)) return b;
    return 0;
}
std::string capListToString(uint32_t caps) {
    std::string out;
    for (int i = 0; i < 32; ++i)
        if (caps & (1u << i)) {
            if (!out.empty()) out += ' ';
            out += capName(1u << i);
        }
    return out;
}

// ----------------------------------------------------------------------

std::string ControllerProfile::validate() const {
    if (version < 1 || version > kVersion)
        return "unsupported profileVersion";
    for (size_t i = 0; i < mappings.size(); ++i) {
        const Mapping& m = mappings[i];
        if (m.in.channel > 15) return "mapping in.channel > 15";
        if (m.outMax == m.outMin && m.mode != MappingMode::Toggle &&
            m.mode != MappingMode::Momentary &&
            m.mode != MappingMode::IncDec)
            return "mapping " + std::to_string(i) +
                   " has empty output range";
        if (m.target.kind == Target::Kind::Command &&
            m.target.cmd == 0 && m.target.cmdName.empty() &&
            m.target.control.empty())
            return "mapping " + std::to_string(i) + " targets no command";
        if (m.target.kind == Target::Kind::Param &&
            m.target.paramId == kInvalidParam)
            return "mapping " + std::to_string(i) +
                   " targets invalid param";
        if (m.target.kind == Target::Kind::Jog && m.target.jogDeck > 3)
            return "mapping " + std::to_string(i) + " jog deck > 3";
    }
    return "";
}

std::vector<std::string> ControllerProfile::findIssues() const {
    return MappingEngine::findIssues(mappings);
}

int ControllerProfile::unverifiedCount() const {
    int n = 0;
    for (const auto& m : mappings) if (!m.verified) ++n;
    return n;
}

static JsonValue addrToJson(const MidiAddr& a) {
    JsonValue o = JsonValue::makeObject();
    o["type"] = JsonValue::makeString(addrKindName(a.kind));
    o["ch"] = JsonValue::makeNumber(a.channel);
    o["num"] = JsonValue::makeNumber(a.number);
    return o;
}
static bool addrFromJson(const JsonValue& o, MidiAddr& a) {
    if (!o.isObject()) return false;
    bool ok = false;
    a.kind = addrKindFromName(o.text("type", "note"), ok);
    a.channel = (uint8_t)o.integer("ch", 0);
    a.number = (uint8_t)o.integer("num", 0);
    return ok && a.channel <= 15;
}

static JsonValue targetToJson(const Target& t,
                              const std::string& cmdName) {
    JsonValue o = JsonValue::makeObject();
    switch (t.kind) {
        case Target::Kind::Command:
            o["kind"] = JsonValue::makeString("command");
            o["id"] = JsonValue::makeString(
                cmdName.empty() ? std::to_string(t.cmd) : cmdName);
            break;
        case Target::Kind::Param:
            o["kind"] = JsonValue::makeString("param");
            o["id"] = JsonValue::makeString(paramNameFromId(t.paramId));
            break;
        case Target::Kind::Jog:
            o["kind"] = JsonValue::makeString("jog");
            o["deck"] = JsonValue::makeNumber(t.jogDeck);
            o["mode"] = JsonValue::makeString(jogModeName((JogMode)t.jogMode));
            break;
        default:
            o["kind"] = JsonValue::makeString("none");
    }
    return o;
}

std::string ControllerProfile::toJson() const {
    JsonValue root = JsonValue::makeObject();
    root["profileVersion"] = JsonValue::makeNumber(version);

    JsonValue c = JsonValue::makeObject();
    c["manufacturer"] = JsonValue::makeString(meta.manufacturer);
    c["model"]        = JsonValue::makeString(meta.model);
    c["nameContains"] = JsonValue::makeString(meta.nameContains);
    c["author"]       = JsonValue::makeString(meta.author);
    if (!meta.protocolRev.empty())
        c["protocolRev"] = JsonValue::makeString(meta.protocolRev);
    if (!meta.origin.empty())
        c["origin"] = JsonValue::makeString(meta.origin);
    root["controller"] = std::move(c);

    JsonValue caps = JsonValue::makeArray();
    for (int i = 0; i < 32; ++i)
        if (capabilities & (1u << i))
            caps.arr.push_back(JsonValue::makeString(capName(1u << i)));
    root["capabilities"] = std::move(caps);

    JsonValue arr = JsonValue::makeArray();
    for (const Mapping& m : mappings) {
        JsonValue e = JsonValue::makeObject();
        e["in"] = addrToJson(m.in);
        e["target"] = targetToJson(m.target, m.target.cmdName);
        e["mode"]    = JsonValue::makeString(modeName(m.mode));
        e["curve"]   = JsonValue::makeString(curveName(m.curve));
        e["encoder"] = JsonValue::makeString(encoderName(m.encoder));
        e["min"]     = JsonValue::makeNumber(m.outMin);
        e["max"]     = JsonValue::makeNumber(m.outMax);
        e["step"]    = JsonValue::makeNumber(m.step);
        if (m.hiRes) e["hires"] = JsonValue::makeBool(true);
        e["enabled"] = JsonValue::makeBool(m.enabled);
        // Phase 27 fields — only emitted when non-default so v1
        // files stay byte-stable for unchanged entries.
        if (m.layer != 0)    e["layer"]    = JsonValue::makeNumber(m.layer);
        if (m.priority != 0) e["priority"] = JsonValue::makeNumber(m.priority);
        if (m.longMs != 0)   e["longMs"]   = JsonValue::makeNumber(m.longMs);
        if (m.longCmd != 0)  e["longCmd"]  = JsonValue::makeNumber(m.longCmd);
        if (!m.verified)     e["verified"] = JsonValue::makeBool(false);
        if (!m.target.control.empty())
            e["control"] = JsonValue::makeString(m.target.control);
        if (m.out.addr.pack() != 0 || m.out.followValue) {
            JsonValue fo = addrToJson(m.out.addr);
            fo["on"] = JsonValue::makeNumber(m.out.onValue);
            fo["off"] = JsonValue::makeNumber(m.out.offValue);
            fo["follow"] = JsonValue::makeBool(m.out.followValue);
            e["out"] = std::move(fo);
        }
        arr.arr.push_back(std::move(e));
    }
    root["mappings"] = std::move(arr);
    return jsonWrite(root, true);
}

bool ControllerProfile::fromJson(const std::string& text,
                                 std::string& err) {
    const auto r = jsonParse(text);
    if (!r.ok) { err = "json: " + r.err + " @" +
                      std::to_string(r.errPos); return false; }
    const JsonValue& root = r.value;
    if (!root.isObject()) { err = "root must be an object"; return false; }

    ControllerProfile p;
    p.version = root.integer("profileVersion", 0);
    if (p.version < 1 || p.version > kVersion) {
        err = "unsupported profileVersion " +
              std::to_string(p.version); return false;
    }

    if (const JsonValue* c = root.at("controller")) {
        p.meta.manufacturer = c->text("manufacturer");
        p.meta.model        = c->text("model");
        p.meta.nameContains = c->text("nameContains");
        p.meta.author       = c->text("author");
        p.meta.protocolRev  = c->text("protocolRev");
        p.meta.origin       = c->text("origin", "user");
    }

    if (const JsonValue* caps = root.at("capabilities")) {
        if (!caps->isArray()) { err = "capabilities must be array";
                                return false; }
        for (auto& s : caps->arr)
            p.capabilities |= capFromName(s.isString() ? s.str : "");
    }

    if (const JsonValue* maps = root.at("mappings")) {
        if (!maps->isArray()) { err = "mappings must be array";
                                return false; }
        for (size_t i = 0; i < maps->arr.size(); ++i) {
            const JsonValue& e = maps->arr[i];
            if (!e.isObject()) { err = "mapping " +
                std::to_string(i) + " not an object"; return false; }
            Mapping m;
            if (const JsonValue* in = e.at("in"))
                if (!addrFromJson(*in, m.in)) {
                    err = "mapping " + std::to_string(i) + " bad 'in'";
                    return false; }

            bool ok = false;
            m.mode    = modeFromName(e.text("mode", "absolute"), ok);
            if (!ok) { err = "mapping " + std::to_string(i) +
                             " bad mode"; return false; }
            m.curve   = curveFromName(e.text("curve", "linear"), ok);
            if (!ok) { err = "mapping " + std::to_string(i) +
                             " bad curve"; return false; }
            m.encoder = encoderFromName(e.text("encoder", "absolute"),
                                        ok);
            if (!ok) { err = "mapping " + std::to_string(i) +
                             " bad encoder"; return false; }

            m.outMin = (float)e.num("min", 0.0);
            m.outMax = (float)e.num("max", 1.0);
            m.step   = (float)e.num("step", 1.0);
            m.hiRes  = e.flag("hires", false);
            m.enabled = e.flag("enabled", true);
            m.layer    = (uint8_t)e.integer("layer", 0);
            m.priority = (uint8_t)e.integer("priority", 0);
            m.longMs   = (uint16_t)e.integer("longMs", 0);
            m.longCmd  = (uint32_t)e.integer("longCmd", 0);
            m.verified = e.flag("verified", true);
            m.target.control = e.text("control");

            if (const JsonValue* t = e.at("target")) {
                const std::string k = t->text("kind", "none");
                if (k == "command") {
                    m.target.kind = Target::Kind::Command;
                    // "id" may be a number or a command name — the
                    // engine resolves names when the profile applies
                    if (const JsonValue* id = t->at("id")) {
                        if (id->isString())
                            m.target.cmdName = id->str;
                        else if (id->isNumber())
                            m.target.cmd = (uint32_t)id->number;
                    }
                } else if (k == "param") {
                    m.target.kind = Target::Kind::Param;
                    m.target.paramId =
                        paramIdFromName(t->text("id", ""));
                } else if (k == "jog") {
                    m.target.kind = Target::Kind::Jog;
                    m.target.jogDeck =
                        (uint8_t)t->integer("deck", 0);
                    m.target.jogMode = (uint8_t)jogModeFromName(
                        t->text("mode", "seek"), ok);
                    if (!ok) { err = "mapping " + std::to_string(i) +
                                     " bad jog mode"; return false; }
                }
            }

            if (const JsonValue* fo = e.at("out")) {
                addrFromJson(*fo, m.out.addr);
                m.out.hasCustom = true;
                m.out.onValue  = (uint8_t)fo->integer("on", 127);
                m.out.offValue = (uint8_t)fo->integer("off", 0);
                m.out.followValue = fo->flag("follow", false);
            }
            p.mappings.push_back(m);
        }
    }

    const std::string v = p.validate();
    if (!v.empty()) { err = v; return false; }
    *this = std::move(p);
    return true;
}

bool ControllerProfile::matchesDevice(const std::string& osName) const {
    if (meta.nameContains.empty()) return false;
    auto low = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return s;
    };
    return low(osName).find(low(meta.nameContains))
           != std::string::npos;
}

} // namespace onda::ctrl
