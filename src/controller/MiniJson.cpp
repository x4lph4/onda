#include "MiniJson.h"

#include <cctype>
#include <cstdio>

namespace onda::ctrl {

namespace {

struct P {
    const char* s; const char* e; size_t pos = 0;
    char peek()  const { return s + pos < e ? s[pos] : '\0'; }
    char next()  { return s + pos < e ? s[pos++] : '\0'; }
    void skipWs() {
        while (s + pos < e &&
               (s[pos]==' '||s[pos]=='\t'||s[pos]=='\n'||s[pos]=='\r'))
            ++pos;
    }
    bool lit(const char* w) {
        size_t i = 0;
        while (w[i]) { if (s+pos+i >= e || s[pos+i] != w[i]) return false; ++i; }
        pos += i; return true;
    }
};

bool parseValue(P& p, JsonValue& out, std::string& err);

bool parseString(P& p, std::string& out, std::string& err) {
    if (p.next() != '"') { err = "expected '\"'"; return false; }
    out.clear();
    for (;;) {
        const char c = p.next();
        if (c == '\0') { err = "unterminated string"; return false; }
        if (c == '"') return true;
        if (c == '\\') {
            const char x = p.next();
            switch (x) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = p.next();
                        if (!std::isxdigit((unsigned char)h)) {
                            err = "bad \\u escape"; return false; }
                        cp = cp * 16 + (unsigned)(
                            h <= '9' ? h - '0'
                                     : (h | 32) - 'a' + 10);
                    }
                    // utf-8 encode
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 63));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 63));
                        out += (char)(0x80 | (cp & 63));
                    }
                    break;
                }
                default: err = "bad escape"; return false;
            }
        } else out += c;
    }
}

bool parseNumber(P& p, JsonValue& out, std::string& err) {
    const size_t start = p.pos;
    if (p.peek() == '-') p.next();
    if (!std::isdigit((unsigned char)p.peek())) {
        err = "bad number"; return false; }
    while (std::isdigit((unsigned char)p.peek())) p.next();
    if (p.peek() == '.') { p.next();
        while (std::isdigit((unsigned char)p.peek())) p.next(); }
    if (p.peek() == 'e' || p.peek() == 'E') {
        p.next();
        if (p.peek() == '+' || p.peek() == '-') p.next();
        while (std::isdigit((unsigned char)p.peek())) p.next();
    }
    char buf[64] {};
    const size_t n = p.pos - start;
    if (n >= sizeof buf) { err = "number too long"; return false; }
    for (size_t i = 0; i < n; ++i) buf[i] = p.s[start + i];
    out.type = JsonValue::Type::Number;
    out.number = std::strtod(buf, nullptr);
    return true;
}

bool parseArray(P& p, JsonValue& out, std::string& err) {
    p.next(); // [
    out = JsonValue::makeArray();
    p.skipWs();
    if (p.peek() == ']') { p.next(); return true; }
    for (;;) {
        JsonValue v;
        if (!parseValue(p, v, err)) return false;
        out.arr.push_back(std::move(v));
        p.skipWs();
        const char c = p.next();
        if (c == ']') return true;
        if (c != ',') { err = "expected ',' or ']'"; return false; }
        p.skipWs();
    }
}

bool parseObject(P& p, JsonValue& out, std::string& err) {
    p.next(); // {
    out = JsonValue::makeObject();
    p.skipWs();
    if (p.peek() == '}') { p.next(); return true; }
    for (;;) {
        p.skipWs();
        if (p.peek() != '"') { err = "expected object key"; return false; }
        std::string key;
        if (!parseString(p, key, err)) return false;
        p.skipWs();
        if (p.next() != ':') { err = "expected ':'"; return false; }
        JsonValue v;
        if (!parseValue(p, v, err)) return false;
        out.obj[key] = std::move(v);
        p.skipWs();
        const char c = p.next();
        if (c == '}') return true;
        if (c != ',') { err = "expected ',' or '}'"; return false; }
    }
}

bool parseValue(P& p, JsonValue& out, std::string& err) {
    p.skipWs();
    const char c = p.peek();
    if (c == '"') { out.type = JsonValue::Type::String;
                    return parseString(p, out.str, err); }
    if (c == '[') return parseArray(p, out, err);
    if (c == '{') return parseObject(p, out, err);
    if (c == 't') { if (!p.lit("true"))  { err = "bad literal"; return false; }
                    out = JsonValue::makeBool(true); return true; }
    if (c == 'f') { if (!p.lit("false")) { err = "bad literal"; return false; }
                    out = JsonValue::makeBool(false); return true; }
    if (c == 'n') { if (!p.lit("null"))  { err = "bad literal"; return false; }
                    return true; }
    if (c == '-' || std::isdigit((unsigned char)c))
        return parseNumber(p, out, err);
    err = "unexpected character"; return false;
}

void writeEscaped(std::string& o, const std::string& s) {
    o += '"';
    for (const char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c & 0xff);
                    o += b;
                } else o += c;
        }
    }
    o += '"';
}

void writeValue(std::string& o, const JsonValue& v, bool pretty,
                int ind) {
    auto pad = [&]{ if (pretty) o += std::string((size_t)ind * 2, ' '); };
    switch (v.type) {
        case JsonValue::Type::Null:   o += "null"; break;
        case JsonValue::Type::Bool:   o += v.boolean ? "true" : "false"; break;
        case JsonValue::Type::Number: {
            char b[32];
            if (v.number == (double)(long long)v.number &&
                v.number < 9e15 && v.number > -9e15)
                std::snprintf(b, sizeof b, "%lld", (long long)v.number);
            else std::snprintf(b, sizeof b, "%.8g", v.number);
            o += b; break; }
        case JsonValue::Type::String: writeEscaped(o, v.str); break;
        case JsonValue::Type::Array: {
            if (v.arr.empty()) { o += "[]"; break; }
            o += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) o += ',';
                if (pretty) { o += '\n'; o += std::string((size_t)(ind+1)*2,' '); }
                writeValue(o, v.arr[i], pretty, ind + 1);
            }
            if (pretty) { o += '\n'; pad(); }
            o += ']'; break; }
        case JsonValue::Type::Object: {
            if (v.obj.empty()) { o += "{}"; break; }
            o += '{';
            bool first = true;
            for (auto& [k, x] : v.obj) {
                if (!first) o += ',';
                first = false;
                if (pretty) { o += '\n';
                    o += std::string((size_t)(ind+1)*2,' '); }
                writeEscaped(o, k);
                o += ':';
                if (pretty) o += ' ';
                writeValue(o, x, pretty, ind + 1);
            }
            if (pretty) { o += '\n'; pad(); }
            o += '}'; break; }
    }
}

} // namespace

JsonResult jsonParse(const std::string& text) {
    P p { text.c_str(), text.c_str() + text.size(), 0 };
    JsonResult r;
    std::string err;
    if (!parseValue(p, r.value, err)) {
        r.errPos = p.pos; r.err = err;
        return r;
    }
    p.skipWs();
    if (p.pos < text.size()) {
        r.errPos = p.pos; r.err = "trailing content";
        return r;
    }
    r.ok = true;
    return r;
}

std::string jsonWrite(const JsonValue& v, bool pretty, int indent) {
    std::string o;
    writeValue(o, v, pretty, indent);
    return o;
}

} // namespace onda::ctrl
