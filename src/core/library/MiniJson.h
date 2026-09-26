#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace onda::library {

/// Minimal JSON value + parser/writer — enough for smart-playlist
/// rules and library backup. Objects keep insertion order (vector of
/// pairs) so exported files diff cleanly.
struct MiniJson {
    using Array  = std::vector<MiniJson>;
    using Object = std::vector<std::pair<std::string, MiniJson>>;

    std::variant<std::nullptr_t, bool, double, std::string,
                 Array, Object> v { nullptr };

    // ---- constructors
    MiniJson() = default;
    MiniJson(std::nullptr_t) : v(nullptr) {}
    MiniJson(bool b) : v(b) {}
    MiniJson(double d) : v(d) {}
    MiniJson(int i) : v((double)i) {}
    MiniJson(int64_t i) : v((double)i) {}
    MiniJson(const char* s) : v(std::string(s)) {}
    MiniJson(std::string s) : v(std::move(s)) {}
    MiniJson(Array a) : v(std::move(a)) {}
    MiniJson(Object o) : v(std::move(o)) {}

    // ---- type checks
    bool isNull()   const { return std::holds_alternative<std::nullptr_t>(v); }
    bool isBool()   const { return std::holds_alternative<bool>(v); }
    bool isNum()    const { return std::holds_alternative<double>(v); }
    bool isStr()    const { return std::holds_alternative<std::string>(v); }
    bool isArr()    const { return std::holds_alternative<Array>(v); }
    bool isObj()    const { return std::holds_alternative<Object>(v); }

    // ---- access
    bool boolean() const { return isBool() ? std::get<bool>(v) : false; }
    double num()   const { return isNum() ? std::get<double>(v) : 0.0; }
    int64_t integer() const { return (int64_t)num(); }
    const std::string& str() const {
        static const std::string empty;
        return isStr() ? std::get<std::string>(v) : empty;
    }
    const Array&  arr() const {
        static const Array empty;
        return isArr() ? std::get<Array>(v) : empty;
    }
    const Object& obj() const {
        static const Object empty;
        return isObj() ? std::get<Object>(v) : empty;
    }
    /// Object member lookup — null value when missing/not-object.
    const MiniJson& at(const std::string& key) const {
        static const MiniJson null;
        if (!isObj()) return null;
        for (const auto& [k, val] : std::get<Object>(v))
            if (k == key) return val;
        return null;
    }

    // ---- builders
    static MiniJson object() { return MiniJson(Object{}); }
    static MiniJson array()  { return MiniJson(Array{}); }
    void set(const std::string& k, MiniJson val) {
        if (!isObj()) v = Object{};
        for (auto& [key, old] : std::get<Object>(v))
            if (key == k) { old = std::move(val); return; }
        std::get<Object>(v).emplace_back(k, std::move(val));
    }
    void push(MiniJson val) {
        if (!isArr()) v = Array{};
        std::get<Array>(v).push_back(std::move(val));
    }

    // ---- serialization
    std::string dump(int indent = 0) const {
        std::string out;
        write(out, indent, 0);
        return out;
    }

    // ---- parse: returns value; `ok` false on malformed input.
    static MiniJson parse(const std::string& s, bool& ok) {
        size_t i = 0;
        MiniJson v = parseValue(s, i, ok);
        skipWs(s, i);
        if (ok && i != s.size()) ok = false;
        return v;
    }
    static MiniJson parse(const std::string& s) {
        bool ok = false;
        return parse(s, ok);
    }

private:
    static void esc(std::string& out, const std::string& s) {
        out += '"';
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04x", c);
                        out += b;
                    } else out += c;
            }
        }
        out += '"';
    }

    void write(std::string& out, int indent, int depth) const {
        const std::string pad((size_t)indent * (depth + 1), ' ');
        const std::string pad0((size_t)indent * depth, ' ');
        const char* nl = indent ? "\n" : "";
        if (isNull())        { out += "null"; return; }
        if (isBool())        { out += boolean() ? "true" : "false"; return; }
        if (isNum()) {
            char b[32];
            const double d = num();
            if (d == (double)(int64_t)d && d < 9e15 && d > -9e15)
                std::snprintf(b, sizeof(b), "%lld", (long long)d);
            else
                std::snprintf(b, sizeof(b), "%.6g", d);
            out += b; return;
        }
        if (isStr())         { esc(out, str()); return; }
        if (isArr()) {
            out += '[';
            bool first = true;
            for (const auto& e : arr()) {
                if (!first) out += ',';
                out += nl; out += pad;
                e.write(out, indent, depth + 1);
                first = false;
            }
            if (!arr().empty()) { out += nl; out += pad0; }
            out += ']'; return;
        }
        // object
        out += '{';
        bool first = true;
        for (const auto& [k, val] : obj()) {
            if (!first) out += ',';
            out += nl; out += pad;
            esc(out, k); out += ':';
            if (indent) out += ' ';
            val.write(out, indent, depth + 1);
            first = false;
        }
        if (!obj().empty()) { out += nl; out += pad0; }
        out += '}';
    }

    static void skipWs(const std::string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'
              || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    static bool match(const std::string& s, size_t& i, const char* lit) {
        size_t n = std::strlen(lit);
        if (s.compare(i, n, lit) == 0) { i += n; return true; }
        return false;
    }

    static MiniJson parseValue(const std::string& s, size_t& i, bool& ok) {
        skipWs(s, i);
        if (i >= s.size()) { ok = false; return {}; }
        switch (s[i]) {
            case '{': {
                ++i; MiniJson o = object(); bool first = true;
                skipWs(s, i);
                if (i < s.size() && s[i] == '}') { ++i; ok = true; return o; }
                while (i < s.size()) {
                    skipWs(s, i);
                    if (i >= s.size() || s[i] != '"') { ok = false; return {}; }
                    std::string key = parseStr(s, i, ok);
                    if (!ok) return {};
                    skipWs(s, i);
                    if (i >= s.size() || s[i] != ':') { ok = false; return {}; }
                    ++i;
                    o.set(key, parseValue(s, i, ok));
                    if (!ok) return {};
                    skipWs(s, i);
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; ok = true; return o; }
                    ok = false; return {};
                }
                ok = false; return {};
            }
            case '[': {
                ++i; MiniJson a = array();
                skipWs(s, i);
                if (i < s.size() && s[i] == ']') { ++i; ok = true; return a; }
                while (i < s.size()) {
                    a.push(parseValue(s, i, ok));
                    if (!ok) return {};
                    skipWs(s, i);
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; ok = true; return a; }
                    ok = false; return {};
                }
                ok = false; return {};
            }
            case '"': {
                ok = true;
                return MiniJson(parseStr(s, i, ok));
            }
            case 't': if (match(s, i, "true"))  { ok = true; return MiniJson(true); }  break;
            case 'f': if (match(s, i, "false")) { ok = true; return MiniJson(false); } break;
            case 'n': if (match(s, i, "null"))  { ok = true; return MiniJson(); }      break;
            default: {
                // number
                const char* start = s.c_str() + i;
                char* end = nullptr;
                const double d = std::strtod(start, &end);
                if (end == start) { ok = false; return {}; }
                i += (size_t)(end - start);
                ok = true;
                return MiniJson(d);
            }
        }
        ok = false; return {};
    }

    static std::string parseStr(const std::string& s, size_t& i, bool& ok) {
        std::string out;
        if (i >= s.size() || s[i] != '"') { ok = false; return out; }
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') { ok = true; return out; }
            if (c == '\\' && i < s.size()) {
                const char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) { ok = false; return {}; }
                        const unsigned cp =
                            (unsigned)std::strtoul(s.substr(i, 4).c_str(),
                                                   nullptr, 16);
                        i += 4;
                        // BMP → UTF-8 (surrogates emitted raw — good
                        // enough for rules/backup metadata)
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: ok = false; return {};
                }
            } else out += c;
        }
        ok = false; return out;
    }
};

} // namespace onda::library
