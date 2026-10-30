// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// bufsc --check old.bufs new.bufs: may a reader built from one schema read what a
// writer built from the other wrote?
//
// Tables may gain fields at the end, and a field may become deprecated.
// Nothing else may change: a field's position comes from the fields declared
// before it, and its value is stored XOR its default, so a changed type,
// order or default changes what existing bytes mean. Structs are fixed.
// Enums may gain values. Renaming a field is allowed (it is not on the wire);
// renaming a table or struct changes its typeId and is reported.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "schema.hpp"

namespace bufsc {

namespace {

std::string typeText(const Type& t) {
    std::string base;
    switch (t.base) {
    case Base::Enum: base = t.enumDef->qualified(); break;
    case Base::Struct: base = t.structDef->qualified(); break;
    case Base::Table: base = t.tableDef->qualified(); break;
    default: base = baseName(t.base); break;
    }
    if (t.fixedLength)
        return "[" + base + ":" + std::to_string(t.fixedLength) + "]";
    return t.vector ? "[" + base + "]" : base;
}

class Checker {
public:
    bool run(const Schema& older, const Schema& newer) {
        for (const auto& e : older.enums) {
            const EnumDef* now = newer.findEnum(e->qualified());
            if (!now) {
                problem("enum " + e->qualified() + " was removed");
                continue;
            }
            if (now->underlying != e->underlying)
                problem("enum " + e->qualified() + " changed its type");
            for (const auto& [name, value] : e->values) {
                bool kept = false;
                for (const auto& [newName, newValue] : now->values)
                    kept = kept || (newName == name && newValue == value);
                if (!kept)
                    problem("enum " + e->qualified() + ": value " + name + " = " + std::to_string(value) +
                            " was changed or removed");
            }
        }
        for (const auto& s : older.structs) {
            const StructDef* now = newer.findStruct(s->qualified());
            if (!now) {
                problem("struct " + s->qualified() + " was removed or renamed");
                continue;
            }
            if (now->size != s->size || now->fields.size() != s->fields.size()) {
                problem("struct " + s->qualified() + " changed its layout (structs are fixed)");
                continue;
            }
            for (size_t i = 0; i < s->fields.size(); ++i)
                if (typeText(s->fields[i].type) != typeText(now->fields[i].type) ||
                    s->fields[i].bitOffset != now->fields[i].bitOffset)
                    problem("struct " + s->qualified() + ": field " + s->fields[i].name + " changed");
        }
        for (const auto& t : older.tables) {
            const TableDef* now = newer.findTable(t->qualified());
            if (!now) {
                problem("table " + t->qualified() + " was removed or renamed (its typeId changes)");
                continue;
            }
            if (now->fields.size() < t->fields.size()) {
                problem("table " + t->qualified() + " lost fields; deprecate them instead");
                continue;
            }
            for (size_t i = 0; i < t->fields.size(); ++i) {
                const Field& a = t->fields[i];
                const Field& b = now->fields[i];
                const std::string where = "table " + t->qualified() + ": field " + std::to_string(i) + " (" +
                                          a.name + ")";
                if (typeText(a.type) != typeText(b.type))
                    problem(where + " changed type from " + typeText(a.type) + " to " + typeText(b.type));
                else if (a.bitOffset != b.bitOffset || a.refIndex != b.refIndex)
                    problem(where + " moved");
                else if (canonicalDefault(a) != canonicalDefault(b))
                    problem(where + " changed its default; stored values are XOR the default");
                if (a.deprecated && !b.deprecated)
                    problem(where + " was deprecated and is in use again");
            }
        }
        if (!older.rootType.empty() && older.rootType != newer.rootType)
            std::fprintf(stderr, "note: root_type changed from %s to %s\n", older.rootType.c_str(),
                         newer.rootType.c_str());
        return problems_ == 0;
    }

private:
    // The bits a default is XORed with, as the generator emits them.
    static std::string canonicalDefault(const Field& f) {
        const std::string& d = f.defaultText;
        if (f.type.base == Base::Bool)
            return d == "true" || d == "1" ? "1" : "0";
        if (f.type.base == Base::Enum) {
            for (const auto& [name, value] : f.type.enumDef->values)
                if (name == d)
                    return std::to_string(value);
            return "0";
        }
        if (isFloat(f.type.base)) {
            const double v = d.empty()            ? 0.0
                             : d == "nan"         ? std::numeric_limits<double>::quiet_NaN()
                             : d == "inf"         ? std::numeric_limits<double>::infinity()
                             : d == "-inf"        ? -std::numeric_limits<double>::infinity()
                                                  : std::strtod(d.c_str(), nullptr);
            uint64_t bits = 0;
            if (f.type.base == Base::Float32) {
                const float narrow = d.empty() || d == "nan" || d == "inf" || d == "-inf"
                                         ? static_cast<float>(v)
                                         : std::strtof(d.c_str(), nullptr);
                uint32_t b32 = 0;
                std::memcpy(&b32, &narrow, sizeof b32);
                bits = b32;
            } else {
                std::memcpy(&bits, &v, sizeof bits);
            }
            return std::to_string(bits);
        }
        if (d.empty())
            return "0";
        if (f.type.base == Base::UInt64)
            return std::to_string(std::strtoull(d.c_str(), nullptr, 0));
        return std::to_string(std::strtoll(d.c_str(), nullptr, 0));
    }

    void problem(const std::string& text) {
        std::fprintf(stderr, "incompatible: %s\n", text.c_str());
        ++problems_;
    }

    int problems_ = 0;
};

} // namespace

bool checkCompatible(const Schema& older, const Schema& newer) { return Checker().run(older, newer); }

} // namespace bufsc
