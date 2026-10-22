// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/JsonLite.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace jsonlite {

const JsonValue* JsonValue::find(const std::string& key) const {
    if (_type != Type::Object)
        return nullptr;

    for (const auto& [name, value] : _object) {
        if (name == key)
            return &value;
    }
    return nullptr;
}

JsonValue JsonValue::makeString(std::string value) {
    JsonValue out;
    out._type = Type::String;
    out._string = std::move(value);
    return out;
}

JsonValue JsonValue::makeNumber(double value) {
    JsonValue out;
    out._type = Type::Number;
    out._number = value;
    return out;
}

JsonValue JsonValue::makeBool(bool value) {
    JsonValue out;
    out._type = Type::Bool;
    out._bool = value;
    return out;
}

JsonValue JsonValue::makeObject(std::vector<std::pair<std::string, JsonValue>> fields) {
    JsonValue out;
    out._type = Type::Object;
    out._object = std::move(fields);
    return out;
}

namespace {

void appendString(std::string& output, const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (unsigned char character : value) {
        if (character == '"' || character == '\\') {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        } else if (character < 0x20) {
            output += "\\u00";
            output.push_back(hex[character >> 4]);
            output.push_back(hex[character & 0x0F]);
        } else {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

void appendValue(std::string& output, const JsonValue& value) {
    switch (value.type()) {
        case JsonValue::Type::Null: output += "null"; break;
        case JsonValue::Type::Bool: output += value.asBool() ? "true" : "false"; break;
        case JsonValue::Type::String: appendString(output, value.asString()); break;
        case JsonValue::Type::Number: {
            if (!std::isfinite(value.asDouble()))
                throw std::invalid_argument("JSON numbers must be finite");
            char buffer[64];
            const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value.asDouble());
            if (converted.ec != std::errc{})
                throw std::invalid_argument("cannot serialize JSON number");
            output.append(buffer, converted.ptr);
            break;
        }
        case JsonValue::Type::Array:
            output.push_back('[');
            for (const JsonValue& entry : value.asArray()) {
                if (output.back() != '[')
                    output.push_back(',');
                appendValue(output, entry);
            }
            output.push_back(']');
            break;
        case JsonValue::Type::Object:
            output.push_back('{');
            for (const auto& [name, entry] : value._object) {
                if (output.back() != '{')
                    output.push_back(',');
                appendString(output, name);
                output.push_back(':');
                appendValue(output, entry);
            }
            output.push_back('}');
            break;
    }
}

// A minimal recursive-descent reader. Deliberately forgiving of nothing:
// its input is expected to be well-formed, so the only input it needs to
// handle well is correct JSON -- malformed input just needs to fail safely,
// which returning false and an error message does.
class Parser {
public:
    explicit Parser(std::string_view text) : _text(text) {}

    bool parseValue(JsonValue& out) {
        skipWhitespace();
        if (_at >= _text.size()) {
            _error = "unexpected end of input";
            return false;
        }
        const char c = _text[_at];
        if (c == '{')
            return parseObject(out);
        if (c == '[')
            return parseArray(out);
        if (c == '"')
            return parseString(out);
        if (c == 't' || c == 'f')
            return parseBool(out);
        if (c == 'n')
            return parseNull(out);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parseNumber(out);

        _error = "unexpected character";
        return false;
    }

    const std::string& error() const { return _error; }

private:
    void skipWhitespace() {
        while (_at < _text.size() && std::isspace(static_cast<unsigned char>(_text[_at])))
            ++_at;
    }

    bool expect(char c) {
        if (_at >= _text.size() || _text[_at] != c) {
            _error = std::string("expected '") + c + "'";
            return false;
        }
        ++_at;
        return true;
    }

    bool parseObject(JsonValue& out) {
        if (!expect('{'))
            return false;

        out = JsonValue{};
        out._type = JsonValue::Type::Object;
        skipWhitespace();
        if (_at < _text.size() && _text[_at] == '}') {
            ++_at;
            return true;
        }
        for (;;) {
            skipWhitespace();
            JsonValue key;
            if (!parseString(key))
                return false;

            skipWhitespace();
            if (!expect(':'))
                return false;

            JsonValue value;
            if (!parseValue(value))
                return false;

            out._object.emplace_back(key._string, std::move(value));
            skipWhitespace();
            if (_at < _text.size() && _text[_at] == ',') {
                ++_at;
                continue;
            }
            return expect('}');
        }
    }

    bool parseArray(JsonValue& out) {
        if (!expect('['))
            return false;

        out = JsonValue{};
        out._type = JsonValue::Type::Array;
        skipWhitespace();
        if (_at < _text.size() && _text[_at] == ']') {
            ++_at;
            return true;
        }
        for (;;) {
            JsonValue value;
            if (!parseValue(value))
                return false;

            out._array.push_back(std::move(value));
            skipWhitespace();
            if (_at < _text.size() && _text[_at] == ',') {
                ++_at;
                continue;
            }
            return expect(']');
        }
    }

    bool parseString(JsonValue& out) {
        if (!expect('"'))
            return false;

        out = JsonValue{};
        out._type = JsonValue::Type::String;
        std::string& result = out._string;
        while (_at < _text.size() && _text[_at] != '"') {
            char c = _text[_at++];
            if (c == '\\') {
                if (_at >= _text.size()) {
                    _error = "truncated escape";
                    return false;
                }
                const char escaped = _text[_at++];
                switch (escaped) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'n': result.push_back('\n'); break;
                    case 't': result.push_back('\t'); break;
                    case 'r': result.push_back('\r'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'u': {
                        // Only the basic multilingual plane, as plain UTF-8
                        // bytes rather than full surrogate-pair decoding.
                        if (_at + 4 > _text.size()) {
                            _error = "truncated \\u escape";
                            return false;
                        }
                        const unsigned codepoint = std::strtoul(std::string(_text.substr(_at, 4)).c_str(), nullptr, 16);
                        _at += 4;
                        if (codepoint < 0x80) {
                            result.push_back(static_cast<char>(codepoint));
                        } else if (codepoint < 0x800) {
                            result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                        } else {
                            result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                        }
                        break;
                    }
                    default:
                        _error = "unknown escape";
                        return false;
                }
            } else {
                result.push_back(c);
            }
        }
        return expect('"');
    }

    bool parseBool(JsonValue& out) {
        if (_text.compare(_at, 4, "true") == 0) {
            out = JsonValue::makeBool(true);
            _at += 4;
            return true;
        }
        if (_text.compare(_at, 5, "false") == 0) {
            out = JsonValue::makeBool(false);
            _at += 5;
            return true;
        }
        _error = "expected true or false";
        return false;
    }

    bool parseNull(JsonValue& out) {
        if (_text.compare(_at, 4, "null") == 0) {
            out = JsonValue{};
            _at += 4;
            return true;
        }
        _error = "expected null";
        return false;
    }

    bool parseNumber(JsonValue& out) {
        const size_t start = _at;
        if (_at < _text.size() && _text[_at] == '-')
            ++_at;

        while (_at < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_at])))
            ++_at;

        if (_at < _text.size() && _text[_at] == '.') {
            ++_at;
            while (_at < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_at])))
                ++_at;
        }
        if (_at < _text.size() && (_text[_at] == 'e' || _text[_at] == 'E')) {
            ++_at;
            if (_at < _text.size() && (_text[_at] == '+' || _text[_at] == '-'))
                ++_at;
            while (_at < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_at])))
                ++_at;
        }
        if (_at == start) {
            _error = "expected a number";
            return false;
        }
        out = JsonValue::makeNumber(std::strtod(std::string(_text.substr(start, _at - start)).c_str(), nullptr));
        return true;
    }

    std::string_view _text;
    size_t _at = 0;
    std::string _error;
};

} // namespace

std::string JsonValue::dump() const {
    std::string output;
    appendValue(output, *this);
    return output;
}

bool JsonValue::parse(std::string_view text, JsonValue& out, std::string& error) {
    Parser parser(text);
    if (!parser.parseValue(out)) {
        error = parser.error();
        return false;
    }
    return true;
}

float numberOr(const JsonValue& object, const char* key, float fallback) {
    const JsonValue* value = object.find(key);
    return value && value->isNumber() ? static_cast<float>(value->asDouble()) : fallback;
}

std::string stringOr(const JsonValue& object, const char* key, const char* fallback) {
    const JsonValue* value = object.find(key);
    return value && value->isString() ? value->asString() : std::string(fallback);
}

} // namespace jsonlite
