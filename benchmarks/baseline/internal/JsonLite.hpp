// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// JsonLite: a minimal standalone JSON DOM parser, kept as the benchmark's
// "json/jsonlite" baseline next to libhttp's json.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jsonlite {

// A tiny JSON value: objects, arrays, strings (with the escapes JSON actually
// uses), numbers and booleans/null. Not a general-purpose JSON library: no
// dependencies, a vector of members per object, linear key lookup.
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Type type() const { return _type; }
    bool isNull() const { return _type == Type::Null; }
    bool isObject() const { return _type == Type::Object; }
    bool isArray() const { return _type == Type::Array; }
    bool isString() const { return _type == Type::String; }
    bool isNumber() const { return _type == Type::Number; }
    bool isBool() const { return _type == Type::Bool; }

    bool asBool() const { return _bool; }
    double asDouble() const { return _number; }
    long long asInt() const { return static_cast<long long>(_number); }
    const std::string& asString() const { return _string; }
    const std::vector<JsonValue>& asArray() const { return _array; }

    // nullptr when `this` is not an object or the key is absent.
    const JsonValue* find(const std::string& key) const;

    static bool parse(std::string_view text, JsonValue& out, std::string& error);
    std::string dump() const;

    static JsonValue makeString(std::string value);
    static JsonValue makeNumber(double value);
    static JsonValue makeBool(bool value);
    static JsonValue makeObject(std::vector<std::pair<std::string, JsonValue>> fields);

    // Internal storage used by the parser and serializer (JsonLite.cpp).
    // Callers use the factories and accessors above.
    Type _type = Type::Null;
    bool _bool = false;
    double _number = 0;
    std::string _string;
    std::vector<JsonValue> _array;
    std::vector<std::pair<std::string, JsonValue>> _object;
};

// `object[key]` as a float/string, or `fallback` when the key is absent or
// the wrong type.
float numberOr(const JsonValue& object, const char* key, float fallback);
std::string stringOr(const JsonValue& object, const char* key, const char* fallback);

} // namespace jsonlite
