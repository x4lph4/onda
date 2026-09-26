#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace onda::ctrl {

/// Minimal strict JSON subset for declarative files (controller
/// profiles, mapping presets). NOT juce::JSON — this lives in the
/// JUCE-free core so tests and future tools share it.
///
/// Supports: objects, arrays, strings, numbers, true/false/null.
/// No execution, no comments, no trailing commas — pure data.

struct JsonValue {
    enum class Type : uint8_t {
        Null, Bool, Number, String, Array, Object
    };
    Type   type = Type::Null;
    bool   boolean = false;
    double number = 0.0;
    std::string               str;
    std::vector<JsonValue>    arr;
    std::map<std::string, JsonValue> obj;

    bool isNull()   const { return type == Type::Null; }
    bool isBool()   const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray()  const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    static JsonValue makeBool(bool b)     { JsonValue v; v.type = Type::Bool;   v.boolean = b; return v; }
    static JsonValue makeNumber(double n) { JsonValue v; v.type = Type::Number; v.number = n;  return v; }
    static JsonValue makeString(std::string s) {
        JsonValue v; v.type = Type::String; v.str = std::move(s); return v; }
    static JsonValue makeArray()  { JsonValue v; v.type = Type::Array;  return v; }
    static JsonValue makeObject() { JsonValue v; v.type = Type::Object; return v; }

    // ---- typed access (safe defaults) ----
    const JsonValue* at(const std::string& k) const {
        if (!isObject()) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    const JsonValue* at(size_t i) const {
        return isArray() && i < arr.size() ? &arr[i] : nullptr;
    }
    double num(const std::string& k, double def = 0.0) const {
        const JsonValue* v = at(k);
        return v && v->isNumber() ? v->number : def;
    }
    std::string text(const std::string& k,
                     const std::string& def = "") const {
        const JsonValue* v = at(k);
        return v && v->isString() ? v->str : def;
    }
    bool flag(const std::string& k, bool def = false) const {
        const JsonValue* v = at(k);
        return v && v->isBool() ? v->boolean : def;
    }
    int  integer(const std::string& k, int def = 0) const {
        return (int)num(k, (double)def);
    }
    JsonValue& operator[](const std::string& k) { return obj[k]; }
};

/// Parse result keeps the error position for profile validation.
struct JsonResult {
    JsonValue   value;
    bool        ok = false;
    size_t      errPos = 0;
    std::string err;
};

JsonResult jsonParse(const std::string& text);

/// Serialize — stable key order (std::map), compact or pretty.
std::string jsonWrite(const JsonValue& v, bool pretty = false,
                      int indent = 0);

} // namespace onda::ctrl
