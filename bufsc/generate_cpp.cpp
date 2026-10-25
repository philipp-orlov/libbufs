// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Writes the C++ header for a schema: enums, fixed-layout structs, and for
// every table a class with nested Reader (zero-copy accessors), Builder
// (front-to-back setters) and a verifier. Everything is inline; the header
// needs only bufs/bufs.hpp.
//
// Names: field names become lowerCamelCase accessors (sample_rate ->
// sampleRate()), with set/init/has prefixes on the Builder and Reader.

#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include "schema.hpp"

namespace bufsc {

namespace {

// C++ keywords and alternative tokens up to C++20, and names the generated
// classes use themselves. An identifier from a schema that is one of these
// gets a trailing underscore.
const std::set<std::string>& keywords() {
    static const std::set<std::string> words = {
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept",
        "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
        "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "import", "inline", "int", "long", "module", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "reflexpr", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "synchronized", "template", "this", "thread_local", "throw", "true",
        "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
        "wchar_t", "while", "xor", "xor_eq", "final", "override", "std", "bufs", "Reader", "Builder", "Table",
    };
    return words;
}

std::string ident(const std::string& name) { return keywords().count(name) ? name + "_" : name; }

std::string camel(const std::string& name) {
    std::string out;
    bool upper = false;
    for (char c : name) {
        if (c == '_') {
            upper = !out.empty();
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    if (!out.empty())
        out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
    return out;
}

std::string accessor(const std::string& name) { return ident(camel(name)); }

std::string prefixed(const char* prefix, const std::string& name) {
    std::string c = camel(name);
    c[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(c[0])));
    return prefix + c;
}

std::string cppNamespace(const std::string& ns) {
    std::string out;
    size_t start = 0;
    for (;;) {
        const size_t dot = ns.find('.', start);
        out += ident(ns.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos)
            return out;
        out += "::";
        start = dot + 1;
    }
}

std::string cppQualified(const Definition& d) {
    return d.ns.empty() ? "::" + ident(d.name) : "::" + cppNamespace(d.ns) + "::" + ident(d.name);
}

std::string scalarCpp(Base base) {
    switch (base) {
    case Base::Bool: return "bool";
    case Base::Int8: return "int8_t";
    case Base::UInt8: return "uint8_t";
    case Base::Int16: return "int16_t";
    case Base::UInt16: return "uint16_t";
    case Base::Int32: return "int32_t";
    case Base::UInt32: return "uint32_t";
    case Base::Int64: return "int64_t";
    case Base::UInt64: return "uint64_t";
    case Base::Float32: return "float";
    case Base::Float64: return "double";
    default: return "?";
    }
}

std::string hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08xu", value);
    return text;
}

// The C++ literal for a field's default, in its own type.
std::string defaultLiteral(const Field& f) {
    const Type& t = f.type;
    const std::string& d = f.defaultText;
    if (t.base == Base::Bool)
        return d == "true" || d == "1" ? "true" : "false";
    if (t.base == Base::Enum) {
        const std::string type = cppQualified(*t.enumDef);
        if (d.empty())
            return "static_cast<" + type + ">(0)";
        return type + "::" + ident(d);
    }
    if (isFloat(t.base)) {
        const std::string type = scalarCpp(t.base);
        if (d.empty())
            return t.base == Base::Float32 ? "0.0f" : "0.0";
        if (d == "nan")
            return "std::numeric_limits<" + type + ">::quiet_NaN()";
        if (d == "inf" || d == "+inf" || d == "infinity")
            return "std::numeric_limits<" + type + ">::infinity()";
        if (d == "-inf" || d == "-infinity")
            return "-std::numeric_limits<" + type + ">::infinity()";
        // Exact: a hexadecimal literal of the value the type holds.
        char text[64];
        if (t.base == Base::Float32)
            std::snprintf(text, sizeof text, "%af", static_cast<double>(std::strtof(d.c_str(), nullptr)));
        else
            std::snprintf(text, sizeof text, "%a", std::strtod(d.c_str(), nullptr));
        return text;
    }
    const std::string type = scalarCpp(t.base);
    if (d.empty())
        return "static_cast<" + type + ">(0)";
    if (t.base == Base::Int64 && d == "-9223372036854775808")
        return "std::numeric_limits<int64_t>::min()";
    return "static_cast<" + type + ">(" + d + (t.base == Base::UInt64 ? "ull" : "ll") + ")";
}

class Writer {
public:
    explicit Writer(const Schema& schema) : schema_(schema) {}

    std::string run() {
        for (const auto& t : schema_.tables)
            if (mine(*t))
                checkNames(*t);
        line("// Generated by bufsc (libbufs) from " + baseName(schema_.mainFile) + ". Do not edit.");
        line("#pragma once");
        line("");
        line("#include <array>");
        line("#include <cstddef>");
        line("#include <cstdint>");
        line("#include <cstring>");
        line("#include <limits>");
        line("#include <string_view>");
        line("");
        line("#include \"bufs/bufs.hpp\"");
        for (const std::string& include : schema_.includes) {
            // bufsc writes every header flat into its output directory.
            std::string header = baseName(include);
            const size_t dot = header.rfind('.');
            if (dot != std::string::npos)
                header.erase(dot);
            line("#include \"" + header + ".bufs.hpp\"");
        }
        line("");

        for (const auto& e : schema_.enums)
            if (mine(*e))
                writeEnum(*e);
        std::set<const StructDef*> written;
        for (const auto& s : schema_.structs)
            if (mine(*s))
                writeStructAfterItsFields(*s, written);
        for (const auto& t : schema_.tables)
            if (mine(*t))
                writeTableClass(*t);
        for (const auto& t : schema_.tables)
            if (mine(*t))
                writeReaderClass(*t);
        for (const auto& t : schema_.tables)
            if (mine(*t))
                writeBuilderClass(*t);
        for (const auto& t : schema_.tables)
            if (mine(*t))
                writeDefinitions(*t);
        closeNamespace();
        writeStructInfo();
        return out_.str();
    }

private:
    static std::string baseName(const std::string& path) {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    bool mine(const Definition& d) const { return d.file == schema_.mainFile; }

    // Two fields whose generated accessors would have the same name (bar_baz
    // and barBaz, or has_x and an x string) cannot both be generated.
    static void checkNames(const TableDef& t) {
        std::set<std::string> reader = {"bufsTable"}, builder = {"bufsOffset"};
        auto claim = [&](std::set<std::string>& names, const std::string& name, const Field& f) {
            if (!names.insert(name).second)
                throw SchemaError(t.file + ":" + std::to_string(f.line) + ": field '" + f.name +
                                  "' generates the accessor " + name + "(), which another field of " + t.name +
                                  " already has");
        };
        for (const Field& f : t.fields) {
            if (f.deprecated)
                continue;
            claim(reader, accessor(f.name), f);
            if (f.refIndex >= 0)
                claim(reader, prefixed("has", f.name), f);
            const bool initOnly = !f.type.vector && f.type.base == Base::Table;
            const bool both = f.type.vector && f.type.base != Base::String && f.type.base != Base::Table;
            if (!initOnly)
                claim(builder, prefixed(f.type.vector && !both ? "init" : "set", f.name), f);
            if (initOnly || both)
                claim(builder, prefixed("init", f.name), f);
        }
    }

    void line(const std::string& text) { out_ << text << "\n"; }

    void enter(const Definition& d) {
        if (d.ns == currentNs_)
            return;
        closeNamespace();
        if (!d.ns.empty()) {
            line("namespace " + cppNamespace(d.ns) + " {");
            line("");
        }
        currentNs_ = d.ns;
    }

    void closeNamespace() {
        if (!currentNs_.empty()) {
            line("} // namespace " + cppNamespace(currentNs_));
            line("");
        }
        currentNs_.clear();
    }

    void doc(const std::vector<std::string>& lines, const std::string& indent) {
        for (const std::string& l : lines)
            line(indent + "// " + l);
    }

    // The C++ type of one element of `t` (the field itself unless a vector).
    std::string elementType(const Type& t) const {
        switch (t.base) {
        case Base::Enum: return cppQualified(*t.enumDef);
        case Base::Struct: return cppQualified(*t.structDef);
        case Base::Table: return cppQualified(*t.tableDef);
        case Base::String: return "std::string_view";
        default: return scalarCpp(t.base);
        }
    }

    std::string storageType(const Type& t) const {
        return t.base == Base::Enum ? scalarCpp(t.enumDef->underlying) : scalarCpp(t.base);
    }

    void writeEnum(const EnumDef& e) {
        enter(e);
        doc(e.doc, "");
        const std::string under = scalarCpp(e.underlying);
        line("enum class " + ident(e.name) + " : " + under + " {");
        for (const auto& [name, value] : e.values)
            line("    " + ident(name) + " = " + std::to_string(value) + ",");
        line("};");
        line("");
        line("inline constexpr const char* enumName(" + ident(e.name) + " value) {");
        line("    switch (value) {");
        std::set<int64_t> seen;
        for (const auto& [name, value] : e.values)
            if (seen.insert(value).second)
                line("    case " + ident(e.name) + "::" + ident(name) + ": return \"" + name + "\";");
        line("    }");
        line("    return \"\";");
        line("}");
        line("");
    }

    // A struct after the structs it holds, whatever the declaration order.
    void writeStructAfterItsFields(const StructDef& s, std::set<const StructDef*>& written) {
        if (!written.insert(&s).second)
            return;
        for (const Field& f : s.fields)
            if (f.type.base == Base::Struct && mine(*f.type.structDef))
                writeStructAfterItsFields(*f.type.structDef, written);
        writeStruct(s);
    }

    void writeStruct(const StructDef& s) {
        enter(s);
        doc(s.doc, "");
        line("struct " + ident(s.name) + " {");
        uint32_t at = 0;
        int pad = 0;
        for (const Field& f : s.fields) {
            const uint32_t offset = f.bitOffset / 8;
            if (offset > at)
                line("    uint8_t bufsPadding" + std::to_string(pad++) + "_[" + std::to_string(offset - at) + "]{};");
            doc(f.doc, "    ");
            // A bool in a struct is a byte the peer wrote: uint8_t, so any value
            // is defined behavior (non-zero means true).
            const std::string one = f.type.base == Base::Bool ? "uint8_t" : elementType(f.type);
            const std::string type = f.type.fixedLength
                                         ? "std::array<" + one + ", " + std::to_string(f.type.fixedLength) + ">"
                                         : one;
            line("    " + type + " " + accessor(f.name) + "{};");
            at = offset + structFieldBytes(f.type);
        }
        if (s.size > at)
            line("    uint8_t bufsPadding" + std::to_string(pad++) + "_[" + std::to_string(s.size - at) + "]{};");
        line("};");
        line("static_assert(sizeof(" + ident(s.name) + ") == " + std::to_string(s.size) + " && alignof(" + ident(s.name) +
             ") == " + std::to_string(s.align) + ", \"bufs: layout of " + ident(s.name) + "\");");
        for (const Field& f : s.fields)
            line("static_assert(offsetof(" + ident(s.name) + ", " + accessor(f.name) + ") == " +
                 std::to_string(f.bitOffset / 8) + ", \"bufs: layout of " + ident(s.name) + "\");");
        line("");
        structs_.push_back(&s);
    }

    static uint32_t structFieldBytes(const Type& t) {
        uint32_t one = t.base == Base::Struct ? t.structDef->size
                       : t.base == Base::Enum ? scalarBytes(t.enumDef->underlying)
                                              : scalarBytes(t.base);
        return one * (t.fixedLength ? t.fixedLength : 1);
    }

    void writeTableClass(const TableDef& t) {
        enter(t);
        doc(t.doc, "");
        line("class " + ident(t.name) + " {");
        line("public:");
        line("    static constexpr uint32_t typeId = " + hex(typeIdOf(t.qualified())) + "; // " + t.qualified());
        line("    static constexpr std::string_view typeName = \"" + t.qualified() + "\";");
        line("    static constexpr uint16_t kDataBytes = " + std::to_string(t.dataBytes) + ";");
        line("    static constexpr uint16_t kRefCount = " + std::to_string(t.refCount) + ";");
        line("");
        line("    class Reader;");
        line("    class Builder;");
        line("");
        line("    static bool verifyTable(::bufs::Verifier& verifier, const uint8_t* table, uint32_t depth);");
        line("};");
        line("");
    }

    void writeReaderClass(const TableDef& t) {
        enter(t);
        line("class " + ident(t.name) + "::Reader {");
        line("public:");
        line("    Reader() = default;");
        line("    explicit Reader(const uint8_t* table) : bufsTable_(table) {}");
        line("    explicit operator bool() const { return bufsTable_ != nullptr; }");
        line("    const uint8_t* bufsTable() const { return bufsTable_; }");
        line("");
        for (const Field& f : t.fields) {
            if (f.deprecated)
                continue;
            doc(f.doc, "    ");
            const std::string name = accessor(f.name);
            line("    " + readerReturn(f.type) + " " + name + "() const;");
            if (f.refIndex >= 0)
                line("    bool " + prefixed("has", f.name) + "() const;");
        }
        line("");
        line("private:");
        line("    const uint8_t* bufsTable_ = nullptr;");
        line("};");
        line("");
    }

    std::string readerReturn(const Type& t) const {
        if (t.vector) {
            switch (t.base) {
            case Base::Bool: return "::bufs::BoolVector";
            case Base::String: return "::bufs::StringVector";
            case Base::Table: return "::bufs::TableVector<" + elementType(t) + "::Reader>";
            default: return "::bufs::Vector<" + elementType(t) + ">";
            }
        }
        switch (t.base) {
        case Base::String: return "std::string_view";
        case Base::Struct: return "const " + elementType(t) + "&";
        case Base::Table: return elementType(t) + "::Reader";
        default: return elementType(t);
        }
    }

    void writeBuilderClass(const TableDef& t) {
        enter(t);
        line("class " + ident(t.name) + "::Builder {");
        line("public:");
        line("    using Table = " + ident(t.name) + ";");
        line("    Builder(::bufs::MessageBuilder& message, uint32_t table) : bufsMessage_(&message), bufsTable_(table) {}");
        line("    uint32_t bufsOffset() const { return bufsTable_; }");
        line("");
        for (const Field& f : t.fields) {
            if (f.deprecated)
                continue;
            const Type& type = f.type;
            const std::string set = prefixed("set", f.name);
            const std::string init = prefixed("init", f.name);
            const std::string element = elementType(type);
            if (type.vector) {
                switch (type.base) {
                case Base::String:
                    line("    ::bufs::StringListBuilder " + init + "(size_t count);");
                    break;
                case Base::Table:
                    line("    ::bufs::TableListBuilder<" + element + "::Builder> " + init + "(size_t count);");
                    break;
                default:
                    line("    ::bufs::ListBuilder<" + element + "> " + init + "(size_t count);");
                    line("    ::bufs::ListBuilder<" + element + "> " + init + "(size_t count, ::bufs::Uninitialized);");
                    line("    void " + set + "(const " + element + "* values, size_t count);");
                    break;
                }
                continue;
            }
            switch (type.base) {
            case Base::String: line("    void " + set + "(std::string_view value);"); break;
            case Base::Table: line("    " + element + "::Builder " + init + "();"); break;
            case Base::Struct: line("    void " + set + "(const " + element + "& value);"); break;
            default: line("    void " + set + "(" + element + " value);"); break;
            }
        }
        line("");
        line("private:");
        line("    ::bufs::MessageBuilder* bufsMessage_;");
        line("    uint32_t bufsTable_;");
        line("};");
        line("");
    }

    void writeDefinitions(const TableDef& t) {
        enter(t);
        const std::string reader = ident(t.name) + "::Reader::";
        const std::string builder = ident(t.name) + "::Builder::";
        for (const Field& f : t.fields) {
            if (f.deprecated)
                continue;
            const Type& type = f.type;
            const std::string name = accessor(f.name);
            const std::string ret = readerReturn(type);
            const std::string element = elementType(type);
            const std::string offset = std::to_string(f.bitOffset / 8);
            if (f.refIndex >= 0) {
                const std::string ref = "::bufs::detail::readRef(bufsTable_, " + std::to_string(f.refIndex) + ")";
                std::string body;
                if (type.vector)
                    body = ret + "(" + ref + ")";
                else if (type.base == Base::String)
                    body = "::bufs::detail::readString(" + ref + ")";
                else
                    body = element + "::Reader(" + ref + ")";
                line("inline " + ret + " " + reader + name + "() const { return " + body + "; }");
                line("inline bool " + reader + prefixed("has", f.name) + "() const { return " + ref +
                     " != nullptr; }");
                continue;
            }
            if (type.base == Base::Bool) {
                line("inline bool " + reader + name + "() const { return ::bufs::detail::readBit(bufsTable_, " +
                     std::to_string(f.bitOffset) + ", " + defaultLiteral(f) + "); }");
            } else if (type.base == Base::Struct) {
                line("inline " + ret + " " + reader + name + "() const { return ::bufs::detail::readStruct<" +
                     element + ">(bufsTable_, " + offset + "); }");
            } else if (type.base == Base::Enum) {
                const std::string storage = storageType(type);
                line("inline " + ret + " " + reader + name + "() const { return static_cast<" + element +
                     ">(::bufs::detail::readField<" + storage + ">(bufsTable_, " + offset + ", static_cast<" + storage +
                     ">(" + defaultLiteral(f) + "))); }");
            } else {
                line("inline " + ret + " " + reader + name + "() const { return ::bufs::detail::readField<" +
                     element + ">(bufsTable_, " + offset + ", " + defaultLiteral(f) + "); }");
            }
        }
        line("");

        for (const Field& f : t.fields) {
            if (f.deprecated)
                continue;
            const Type& type = f.type;
            const std::string element = elementType(type);
            const std::string set = prefixed("set", f.name);
            const std::string init = prefixed("init", f.name);
            const std::string offset = std::to_string(f.bitOffset / 8);
            const std::string slot = "::bufs::MessageBuilder::refSlot(bufsTable_, Table::kDataBytes, " +
                                     std::to_string(f.refIndex) + ")";
            if (type.vector) {
                switch (type.base) {
                case Base::String:
                    line("inline ::bufs::StringListBuilder " + builder + init + "(size_t count) {");
                    line("    const uint32_t at = bufsMessage_->allocateVector(count, 4, 4);");
                    line("    bufsMessage_->setRef(" + slot + ", at);");
                    line("    return ::bufs::StringListBuilder(*bufsMessage_, at);");
                    line("}");
                    break;
                case Base::Table:
                    line("inline ::bufs::TableListBuilder<" + element + "::Builder> " + builder + init +
                         "(size_t count) {");
                    line("    const uint32_t at = bufsMessage_->allocateVector(count, 4, 4);");
                    line("    bufsMessage_->setRef(" + slot + ", at);");
                    line("    return ::bufs::TableListBuilder<" + element + "::Builder>(*bufsMessage_, at);");
                    line("}");
                    break;
                case Base::Bool:
                    line("inline ::bufs::ListBuilder<bool> " + builder + init + "(size_t count) {");
                    line("    const uint32_t at = bufsMessage_->allocateVector(count, 1, 1);");
                    line("    bufsMessage_->setRef(" + slot + ", at);");
                    line("    return ::bufs::ListBuilder<bool>(*bufsMessage_, at);");
                    line("}");
                    line("inline void " + builder + set + "(const bool* values, size_t count) {");
                    line("    const uint32_t at = ::bufs::detail::createBoolVector(*bufsMessage_, values, count);");
                    line("    bufsMessage_->setRef(" + slot + ", at);");
                    line("}");
                    break;
                default:
                    line("inline ::bufs::ListBuilder<" + element + "> " + builder + init + "(size_t count) {");
                    line("    const uint32_t at = bufsMessage_->allocateVector(count, sizeof(" + element + "), alignof(" +
                         element + ") >= 8 ? 8 : 4);");
                    line("    bufsMessage_->setRef(" + slot + ", at);");
                    line("    return ::bufs::ListBuilder<" + element + ">(*bufsMessage_, at);");
                    line("}");
                    line("inline ::bufs::ListBuilder<" + element + "> " + builder + init +
                         "(size_t count, ::bufs::Uninitialized) {");
                    line("    const uint32_t at = bufsMessage_->allocateVector(count, sizeof(" + element + "), alignof(" +
                         element + ") >= 8 ? 8 : 4, false);");
                    line("    bufsMessage_->setRef(" + slot + ", at);");
                    line("    return ::bufs::ListBuilder<" + element + ">(*bufsMessage_, at);");
                    line("}");
                    line("inline void " + builder + set + "(const " + element + "* values, size_t count) {");
                    line("    const uint32_t at = ::bufs::detail::createVector(*bufsMessage_, values, count);");
                    line("    bufsMessage_->setRef(" + slot + ", at);");
                    line("}");
                    break;
                }
                continue;
            }
            switch (type.base) {
            case Base::String:
                line("inline void " + builder + set + "(std::string_view value) {");
                line("    const uint32_t at = bufsMessage_->createString(value);");
                line("    bufsMessage_->setRef(" + slot + ", at);");
                line("}");
                break;
            case Base::Table:
                line("inline " + element + "::Builder " + builder + init + "() {");
                line("    const uint32_t at = bufsMessage_->allocateTable(" + element + "::kDataBytes, " + element +
                     "::kRefCount);");
                line("    bufsMessage_->setRef(" + slot + ", at);");
                line("    return " + element + "::Builder(*bufsMessage_, at);");
                line("}");
                break;
            case Base::Struct:
                line("inline void " + builder + set + "(const " + element + "& value) {");
                line("    std::memcpy(bufsMessage_->data() + bufsTable_ + " + offset + ", &value, sizeof value);");
                line("}");
                break;
            case Base::Bool:
                line("inline void " + builder + set + "(bool value) { ::bufs::detail::writeBit(bufsMessage_->data() + "
                                                       "bufsTable_, " +
                     std::to_string(f.bitOffset) + ", value, " + defaultLiteral(f) + "); }");
                break;
            case Base::Enum: {
                const std::string storage = storageType(type);
                line("inline void " + builder + set + "(" + element + " value) { ::bufs::detail::writeField<" +
                     storage + ">(bufsMessage_->data() + bufsTable_, " + offset + ", static_cast<" + storage +
                     ">(value), static_cast<" + storage + ">(" + defaultLiteral(f) + ")); }");
                break;
            }
            default:
                line("inline void " + builder + set + "(" + element + " value) { ::bufs::detail::writeField<" +
                     element + ">(bufsMessage_->data() + bufsTable_, " + offset + ", value, " + defaultLiteral(f) + "); }");
                break;
            }
        }
        line("");

        line("inline bool " + ident(t.name) +
             "::verifyTable(::bufs::Verifier& verifier, const uint8_t* table, uint32_t depth) {");
        line("    if (!verifier.table(table, depth))");
        line("        return false;");
        bool any = false;
        for (const Field& f : t.fields)
            any = any || (f.refIndex >= 0 && !f.deprecated);
        if (any) {
            line("    const uint8_t* object = nullptr;");
            line("    uint32_t count = 0;");
            line("    (void)count;");
        }
        for (const Field& f : t.fields) {
            if (f.refIndex < 0 || f.deprecated)
                continue;
            const Type& type = f.type;
            std::string check;
            if (type.vector) {
                switch (type.base) {
                case Base::String: check = "verifier.stringVector(object)"; break;
                case Base::Table:
                    check = "verifier.tableVector<" + elementType(type) + ">(object, depth + 1)";
                    break;
                case Base::Bool: check = "verifier.vector(object, 1, 1, count)"; break;
                default:
                    check = "verifier.vector(object, sizeof(" + elementType(type) + "), alignof(" +
                            elementType(type) + "), count)";
                    break;
                }
            } else if (type.base == Base::String) {
                check = "verifier.string(object)";
            } else {
                check = elementType(type) + "::verifyTable(verifier, object, depth + 1)";
            }
            line("    if (!verifier.ref(table, " + std::to_string(f.refIndex) + ", object) || (object && !" + check +
                 "))");
            line("        return false;");
        }
        line("    return true;");
        line("}");
        line("");
    }

    void writeStructInfo() {
        if (structs_.empty())
            return;
        line("namespace bufs {");
        for (const StructDef* s : structs_) {
            line("template <>");
            line("struct StructInfo<" + cppQualified(*s) + "> {");
            line("    static constexpr uint32_t typeId = " + hex(typeIdOf(s->qualified())) + "; // " +
                 s->qualified());
            line("    static constexpr std::string_view typeName = \"" + s->qualified() + "\";");
            line("};");
        }
        line("} // namespace bufs");
    }

    const Schema& schema_;
    std::ostringstream out_;
    std::string currentNs_;
    std::vector<const StructDef*> structs_;
};

} // namespace

std::string cppElementType(const Type& type) { return scalarCpp(type.base); }

std::string generateCpp(const Schema& schema, const std::string& outDir) {
    const std::string text = Writer(schema).run();
    std::string stem = schema.mainFile;
    const size_t slash = stem.rfind('/');
    if (slash != std::string::npos)
        stem.erase(0, slash + 1);
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos)
        stem.erase(dot);
    const std::string path = (outDir.empty() ? std::string(".") : outDir) + "/" + stem + ".bufs.hpp";

    // Unchanged output keeps its timestamp, so nothing that includes it rebuilds.
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            std::stringstream old;
            old << existing.rdbuf();
            if (old.str() == text)
                return path;
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw SchemaError(path + ": cannot write");
    out << text;
    if (!out)
        throw SchemaError(path + ": write failed");
    return path;
}

} // namespace bufsc
