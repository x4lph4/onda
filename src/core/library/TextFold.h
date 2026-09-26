#pragma once

#include <cstdint>
#include <string>

namespace onda::library {

/// Accent- and case-insensitive text folding for search.
///
/// The FTS index stores *folded* text (this function applied at index
/// time) and queries fold their tokens the same way, so "beyonce"
/// matches "Beyoncé" and "BOULEVARD" matches "boulevard". This is a
/// pragmatic fold — ASCII lowercasing + the common Latin diacritic
/// ranges mapped to their base letter + combining marks dropped —
/// not a full NFKD pass (documented limit).
///
/// UTF-8 aware: multi-byte sequences we don't map are passed through
/// unchanged, so CJK/Cyrillic text still indexes verbatim.
class TextFold {
public:
    static std::string fold(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        size_t i = 0;
        while (i < in.size()) {
            const auto c = (uint8_t)in[i];
            if (c < 0x80) {
                out += (char)asciiLower(c);
                ++i;
                continue;
            }
            // multi-byte sequence
            const uint32_t cp = decode(in, i);
            if (cp >= 0x300 && cp <= 0x36F) continue;   // combining mark
            if (const char* rep = mapLatin(cp)) {
                while (*rep) out += *rep++;
            } else {
                // pass through the original bytes
                out.append(in, i, seqLen);
            }
            i += seqLen;
        }
        return out;
    }

    /// True if folding would change the text (cheap pre-check).
    static bool changes(const std::string& s) { return fold(s) != s; }

private:
    static int seqLen; // set by decode() — valid until next decode

    static char asciiLower(uint8_t c) {
        return c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c;
    }

    static uint32_t decode(const std::string& s, size_t i) {
        const auto b0 = (uint8_t)s[i];
        if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
            seqLen = 2;
            return ((uint32_t)(b0 & 0x1F) << 6)
                 | ((uint8_t)s[i + 1] & 0x3F);
        }
        if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
            seqLen = 3;
            return ((uint32_t)(b0 & 0x0F) << 12)
                 | (((uint8_t)s[i + 1] & 0x3F) << 6)
                 | ((uint8_t)s[i + 2] & 0x3F);
        }
        if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
            seqLen = 4;
            return ((uint32_t)(b0 & 0x07) << 18)
                 | (((uint8_t)s[i + 1] & 0x3F) << 12)
                 | (((uint8_t)s[i + 2] & 0x3F) << 6)
                 | ((uint8_t)s[i + 3] & 0x3F);
        }
        seqLen = 1;
        return 0; // invalid byte — caller passes it through
    }

    /// Latin range → base letters. Covers Latin-1 Supplement and the
    /// common Latin Extended-A letters; anything unlisted returns
    /// nullptr and is kept as-is.
    static const char* mapLatin(uint32_t cp) {
        switch (cp) {
            // Latin-1 supplement (À..ÿ)
            case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4:
            case 0xC5:
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4:
            case 0xE5:
            case 0x100: case 0x101: case 0x102: case 0x103:
            case 0x104: case 0x105: return "a";
            case 0xC7: case 0xE7:
            case 0x106: case 0x107: case 0x108: case 0x109:
            case 0x10A: case 0x10B: case 0x10C: case 0x10D: return "c";
            case 0xC8: case 0xC9: case 0xCA: case 0xCB:
            case 0xE8: case 0xE9: case 0xEA: case 0xEB:
            case 0x112: case 0x113: case 0x114: case 0x115:
            case 0x116: case 0x117: case 0x118: case 0x119:
            case 0x11A: case 0x11B: return "e";
            case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            case 0xEC: case 0xED: case 0xEE: case 0xEF:
            case 0x128: case 0x129: case 0x12A: case 0x12B:
            case 0x12C: case 0x12D: case 0x12E: case 0x12F:
            case 0x130: case 0x131: return "i";
            case 0xD1: case 0xF1:
            case 0x143: case 0x144: case 0x145: case 0x146:
            case 0x147: case 0x148: return "n";
            case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
            case 0xD8:
            case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
            case 0xF8:
            case 0x14C: case 0x14D: case 0x14E: case 0x14F:
            case 0x150: case 0x151: return "o";
            case 0xD9: case 0xDA: case 0xDB: case 0xDC:
            case 0xF9: case 0xFA: case 0xFB: case 0xFC:
            case 0x168: case 0x169: case 0x16A: case 0x16B:
            case 0x16C: case 0x16D: case 0x16E: case 0x16F:
            case 0x170: case 0x171: case 0x172: case 0x173: return "u";
            case 0xDD: case 0xFD: case 0xFF:
            case 0x176: case 0x177: case 0x178: return "y";
            case 0x154: case 0x155: case 0x156: case 0x157:
            case 0x158: case 0x159: return "r";
            case 0x15A: case 0x15B: case 0x15C: case 0x15D:
            case 0x15E: case 0x15F: case 0x160: case 0x161:
            case 0x17A: case 0x17B: case 0x179: case 0x17C:
            case 0x17D: case 0x17E: return cp >= 0x179 ? "z" : "s";
            case 0x139: case 0x13A: case 0x13B: case 0x13C:
            case 0x13D: case 0x13E: case 0x13F: case 0x140:
            case 0x141: case 0x142: return "l";
            case 0x10E: case 0x10F: case 0x110: case 0x111:
            case 0xD0: case 0xF0: return "d";
            case 0x11C: case 0x11D: case 0x11E: case 0x11F:
            case 0x120: case 0x121: case 0x122: case 0x123: return "g";
            case 0x124: case 0x125: case 0x126: case 0x127: return "h";
            case 0x134: case 0x135: return "j";
            case 0x136: case 0x137: case 0x138: return "k";
            case 0x164: case 0x165: case 0x166: case 0x167:
            case 0x162: case 0x163: return "t";
            case 0x174: case 0x175: return "w";
            case 0x17F: return "s";
            case 0xDF: return "ss";
            case 0xC6: case 0xE6: return "ae";
            case 0xDE: case 0xFE: return "th";
            case 0x152: case 0x153: return "oe";
            default: return nullptr;
        }
    }
};

inline int TextFold::seqLen = 1;

} // namespace onda::library
