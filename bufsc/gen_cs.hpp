// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The CS generator: <stem>.bufs.cs for the messages of a schema. See gen_cs.cpp.

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "generators.hpp"

namespace bufsc {

namespace csgen {


struct ScalarInfo {
    const char* cs;       // C# type
    const char* table;    // Table accessor
    const char* setter;   // MessageBuilder setter
    uint32_t bytes;
    bool big;
};

inline ScalarInfo scalar(Base base) {
    switch (base) {
    case Base::Bool: return {"bool", "Bit", "SetBit", 1, false};
    case Base::Int8: return {"sbyte", "I8", "SetI8", 1, false};
    case Base::UInt8: return {"byte", "U8", "SetU8", 1, false};
    case Base::Int16: return {"short", "I16", "SetI16", 2, false};
    case Base::UInt16: return {"ushort", "U16", "SetU16", 2, false};
    case Base::Int32: return {"int", "I32", "SetI32", 4, false};
    case Base::UInt32: return {"uint", "U32", "SetU32", 4, false};
    case Base::Int64: return {"long", "I64", "SetI64", 8, true};
    case Base::UInt64: return {"ulong", "U64", "SetU64", 8, true};
    case Base::Float32: return {"float", "F32", "SetF32", 4, false};
    case Base::Float64: return {"double", "F64", "SetF64", 8, true};
    default: break;
    }
    return {"byte", "U8", "SetU8", 1, false};
}

inline std::string camel(const std::string& name) {
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

inline std::string pascal(const std::string& name) {
    std::string out = camel(name);
    if (!out.empty())
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

inline const std::set<std::string>& keywords() {
    static const std::set<std::string> words = {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const", "continue",
        "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern", "false", "finally",
        "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock",
        "long", "namespace", "new", "null", "object", "operator", "out", "override", "params", "private", "protected",
        "public", "readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string",
        "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort",
        "using", "virtual", "void", "volatile", "while"};
    return words;
}

// A parameter / local name.
inline std::string local(const std::string& name) {
    const std::string c = camel(name);
    return keywords().count(c) || c == "options" ? c + "_" : c;
}

inline std::string csNamespace(const std::string& ns) {
    std::string out;
    size_t start = 0;
    for (;;) {
        const size_t dot = ns.find('.', start);
        std::string part = ns.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!part.empty())
            part[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
        out += (out.empty() ? "" : ".") + part;
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return out;
}

inline std::string qualified(const Definition& def) {
    return "global::" + (def.ns.empty() ? std::string() : csNamespace(def.ns) + ".") + def.name;
}

inline std::string hex32(uint32_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%08xu", v);
    return text;
}


} // namespace csgen

using namespace csgen;

class CsGenerator {
public:
    CsGenerator(const Schema& schema, const GenOptions& options) : schema_(schema), options_(options) {}
    virtual ~CsGenerator() = default;

    std::string run() {
        // Group the definitions of the main file by namespace, in order.
        std::map<std::string, std::ostringstream> byNamespace;
        std::vector<std::string> order;
        auto into = [&](const Definition& def) -> std::ostream& {
            if (!byNamespace.count(def.ns))
                order.push_back(def.ns);
            return byNamespace[def.ns];
        };
        for (const auto& e : schema_.enums)
            if (inMainFile(schema_, *e))
                emitEnum(into(*e), *e);
        for (const auto& s : schema_.structs)
            if (inMainFile(schema_, *s))
                emitStruct(into(*s), *s);
        for (const auto& t : schema_.tables)
            if (inMainFile(schema_, *t))
                emitTable(into(*t), *t);
        for (const auto& s : schema_.services)
            if (inMainFile(schema_, *s))
                emitService(into(*s), *s);

        std::ostringstream out;
        out << "// Generated by " << options_.generator << " from " << stemOf(schema_.mainFile) << ".bufs. Do not edit.\n"
            << "#nullable enable\n#pragma warning disable CS1591, CS8981\n"
            << "using System;\nusing System.Buffers.Binary;\nusing System.Collections.Generic;\n"
            << "using " << runtimeNamespace() << ";\n" << extraUsings() << "\n";
        for (const std::string& ns : order) {
            if (!ns.empty())
                out << "namespace " << csNamespace(ns) << "\n{\n";
            out << byNamespace[ns].str();
            if (!ns.empty())
                out << "}\n\n";
        }
        return out.str();
    }

protected:
    // ---- types --------------------------------------------------------------------------------------------

    static std::string enumUnderlying(const EnumDef& e) { return scalar(e.underlying).cs; }

    // One element of type t, as the application sees it.
    std::string elementType(const Type& t) {
        switch (t.base) {
        case Base::String: return "string";
        case Base::Enum: return qualified(*t.enumDef);
        case Base::Struct: return qualified(*t.structDef);
        case Base::Table: return qualified(*t.tableDef);
        default: return scalar(t.base).cs;
        }
    }

    // What a caller passes for a field (or a parameter): Init classes for tables, arrays for vectors.
    std::string initType(const Type& t) {
        std::string type = t.base == Base::Table ? qualified(*t.tableDef) + "Init" : elementType(t);
        return t.vector ? type + "[]" : type;
    }

    // What a reader hands back for a field. Tables are reader structs; scalar vectors are spans over the message.
    std::string readType(const Type& t) {
        if (!t.vector)
            return elementType(t);
        if (t.base == Base::Table)
            return qualified(*t.tableDef) + "[]";
        if (t.base == Base::String)
            return "string[]";
        if (t.base == Base::Bool || t.base == Base::Enum || t.base == Base::Struct)
            return elementType(t) + "[]";
        return std::string("ReadOnlySpan<") + scalar(t.base).cs + ">";
    }

    static uint32_t structFieldBytes(const Type& t) {
        if (t.base == Base::Struct)
            return t.structDef->size;
        if (t.base == Base::Enum)
            return scalar(t.enumDef->underlying).bytes;
        return scalar(t.base).bytes;
    }
    static uint32_t elementBytes(const Type& t) { return t.base == Base::String || t.base == Base::Table ? 4 : structFieldBytes(t); }
    static uint32_t elementAlign(const Type& t) {
        if (t.base == Base::String || t.base == Base::Table)
            return 4;
        if (t.base == Base::Struct)
            return t.structDef->align >= 8 ? 8 : 4;
        return structFieldBytes(t) >= 8 ? 8 : 4;
    }

    // ---- defaults -----------------------------------------------------------------------------------------

    // The stored default as the argument of the accessor, typed for it.
    std::string defaultArg(const Field& f) {
        const Type& t = f.type;
        const std::string& d = f.defaultText;
        Base base = t.base == Base::Enum ? t.enumDef->underlying : t.base;
        if (t.base == Base::Bool)
            return d == "true" || d == "1" ? "1" : "0";
        long long enumValue = 0;
        if (t.base == Base::Enum && !d.empty())
            for (const auto& [name, value] : t.enumDef->values)
                if (name == d)
                    enumValue = value;
        const ScalarInfo info = scalar(base);
        if (isFloat(base)) {
            if (d.empty())
                return "0";
            if (base == Base::Float32) {
                float v = d == "nan" ? std::nanf("") : (d == "inf" || d == "+inf") ? INFINITY : d == "-inf" ? -INFINITY : std::strtof(d.c_str(), nullptr);
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                if (d == "nan")
                    bits = 0x7fc00000u;
                return std::to_string(bits) + "u";
            }
            double v = d == "nan" ? std::nan("") : (d == "inf" || d == "+inf") ? INFINITY : d == "-inf" ? -INFINITY : std::strtod(d.c_str(), nullptr);
            uint64_t bits;
            std::memcpy(&bits, &v, 8);
            if (d == "nan")
                bits = 0x7ff8000000000000ull;
            return std::to_string(bits) + "ul";
        }
        std::string value;
        if (t.base == Base::Enum)
            value = std::to_string(enumValue);
        else if (d.empty())
            return "0";
        else if (base == Base::UInt64)
            return std::to_string(std::strtoull(d.c_str(), nullptr, 0)) + "ul";
        else if (base == Base::Int64)
            return std::to_string(std::strtoll(d.c_str(), nullptr, 0)) + "L";
        else
            value = std::to_string(std::strtoll(d.c_str(), nullptr, 0));
        if (value == "0")
            return "0";
        if (base == Base::UInt64)
            return value + "ul";
        if (base == Base::Int64)
            return value + "L";
        return std::string("unchecked((") + info.cs + ")" + value + ")";
    }

    // ---- struct members -----------------------------------------------------------------------------------

    // Reads one value of `t` (not a vector) at absolute offset `at` of ReadOnlySpan<byte> `s`.
    std::string readExpr(const Type& t, const std::string& s, const std::string& at) {
        if (t.base == Base::Struct)
            return qualified(*t.structDef) + ".Read(" + s + ", " + at + ")";
        const Base base = t.base == Base::Enum ? t.enumDef->underlying : t.base;
        std::string read;
        switch (base) {
        case Base::Bool: read = s + "[" + at + "] != 0"; break;
        case Base::UInt8: read = s + "[" + at + "]"; break;
        case Base::Int8: read = "(sbyte)" + s + "[" + at + "]"; break;
        case Base::Int16: read = "BinaryPrimitives.ReadInt16LittleEndian(" + s + ".Slice(" + at + "))"; break;
        case Base::UInt16: read = "BinaryPrimitives.ReadUInt16LittleEndian(" + s + ".Slice(" + at + "))"; break;
        case Base::Int32: read = "BinaryPrimitives.ReadInt32LittleEndian(" + s + ".Slice(" + at + "))"; break;
        case Base::UInt32: read = "BinaryPrimitives.ReadUInt32LittleEndian(" + s + ".Slice(" + at + "))"; break;
        case Base::Int64: read = "BinaryPrimitives.ReadInt64LittleEndian(" + s + ".Slice(" + at + "))"; break;
        case Base::UInt64: read = "BinaryPrimitives.ReadUInt64LittleEndian(" + s + ".Slice(" + at + "))"; break;
        case Base::Float32: read = "BinaryPrimitives.ReadSingleLittleEndian(" + s + ".Slice(" + at + "))"; break;
        case Base::Float64: read = "BinaryPrimitives.ReadDoubleLittleEndian(" + s + ".Slice(" + at + "))"; break;
        default: read = s + "[" + at + "]"; break;
        }
        if (t.base == Base::Enum)
            return "(" + qualified(*t.enumDef) + ")(" + read + ")";
        return read;
    }

    std::string writeStmt(const Type& t, const std::string& s, const std::string& at, const std::string& value) {
        if (t.base == Base::Struct)
            return qualified(*t.structDef) + ".Write(" + s + ", " + at + ", " + value + ");";
        const Base base = t.base == Base::Enum ? t.enumDef->underlying : t.base;
        const std::string v = t.base == Base::Enum ? "(" + std::string(scalar(base).cs) + ")" + value : value;
        switch (base) {
        case Base::Bool: return s + "[" + at + "] = (byte)(" + value + " ? 1 : 0);";
        case Base::UInt8: return s + "[" + at + "] = " + v + ";";
        case Base::Int8: return s + "[" + at + "] = (byte)" + v + ";";
        case Base::Int16: return "BinaryPrimitives.WriteInt16LittleEndian(" + s + ".Slice(" + at + "), " + v + ");";
        case Base::UInt16: return "BinaryPrimitives.WriteUInt16LittleEndian(" + s + ".Slice(" + at + "), " + v + ");";
        case Base::Int32: return "BinaryPrimitives.WriteInt32LittleEndian(" + s + ".Slice(" + at + "), " + v + ");";
        case Base::UInt32: return "BinaryPrimitives.WriteUInt32LittleEndian(" + s + ".Slice(" + at + "), " + v + ");";
        case Base::Int64: return "BinaryPrimitives.WriteInt64LittleEndian(" + s + ".Slice(" + at + "), " + v + ");";
        case Base::UInt64: return "BinaryPrimitives.WriteUInt64LittleEndian(" + s + ".Slice(" + at + "), " + v + ");";
        case Base::Float32: return "BinaryPrimitives.WriteSingleLittleEndian(" + s + ".Slice(" + at + "), " + v + ");";
        case Base::Float64: return "BinaryPrimitives.WriteDoubleLittleEndian(" + s + ".Slice(" + at + "), " + v + ");";
        default: return s + "[" + at + "] = " + v + ";";
        }
    }

    // ---- emit ---------------------------------------------------------------------------------------------

    void doc(std::ostream& out, const std::vector<std::string>& lines, const char* indent) {
        if (lines.empty())
            return;
        out << indent << "/// <summary>\n";
        for (const std::string& line : lines) {
            std::string escaped;
            for (char c : line)
                escaped += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : std::string(1, c);
            out << indent << "/// " << escaped << "\n";
        }
        out << indent << "/// </summary>\n";
    }

    void emitEnum(std::ostream& out, const EnumDef& e) {
        doc(out, e.doc, "    ");
        out << "    public enum " << e.name << " : " << enumUnderlying(e) << "\n    {\n";
        for (const auto& [name, value] : e.values)
            out << "        " << name << " = " << value << ",\n";
        out << "    }\n\n";
    }

    void emitStruct(std::ostream& out, const StructDef& s) {
        doc(out, s.doc, "    ");
        out << "    public struct " << s.name << "\n    {\n        public const int Size = " << s.size << ";\n";
        for (const Field& f : s.fields)
            out << "        public " << elementType(f.type) << (f.type.fixedLength ? "[]" : "") << " " << member(s.name, f.name) << ";\n";
        out << "\n        public static " << s.name << " Read(ReadOnlySpan<byte> s, int at)\n        {\n            var v = new " << s.name << "();\n";
        for (const Field& f : s.fields) {
            const std::string at = "at + " + std::to_string(f.bitOffset / 8);
            if (f.type.fixedLength) {
                const uint32_t size = structFieldBytes(f.type);
                out << "            v." << member(s.name, f.name) << " = new " << elementType(f.type) << "[" << f.type.fixedLength << "];\n"
                    << "            for (int i = 0; i < " << f.type.fixedLength << "; i++) v." << member(s.name, f.name) << "[i] = "
                    << readExpr(f.type, "s", at + " + i * " + std::to_string(size)) << ";\n";
            } else {
                out << "            v." << member(s.name, f.name) << " = " << readExpr(f.type, "s", at) << ";\n";
            }
        }
        out << "            return v;\n        }\n\n        public static void Write(Span<byte> s, int at, " << s.name << " v)\n        {\n";
        for (const Field& f : s.fields) {
            const std::string at = "at + " + std::to_string(f.bitOffset / 8);
            const std::string m = "v." + member(s.name, f.name);
            if (f.type.fixedLength) {
                const uint32_t size = structFieldBytes(f.type);
                out << "            if (" << m << " != null) for (int i = 0; i < " << f.type.fixedLength << " && i < " << m.substr(0) << ".Length; i++) "
                    << writeStmt(f.type, "s", at + " + i * " + std::to_string(size), m + "[i]") << "\n";
            } else {
                out << "            " << writeStmt(f.type, "s", at, m) << "\n";
            }
        }
        out << "        }\n    }\n\n";
    }

    // A C# member name for field `name` of the type `owner` (a member cannot share its type's name).
    static std::string member(const std::string& owner, const std::string& name) {
        std::string m = pascal(name);
        return m == owner || keywords().count(m) ? m + "_" : m;
    }

    void emitGetter(std::ostream& out, const TableDef& table, const Field& f) {
        const Type& t = f.type;
        const std::string name = member(table.name, f.name);
        const std::string offset = std::to_string(f.bitOffset / 8);
        doc(out, f.doc, "        ");
        if (t.vector) {
            const std::string ref = std::to_string(f.refIndex);
            if (t.base == Base::String) {
                out << "        public string[] " << name << " => _t.Strings(" << ref << ");\n";
            } else if (t.base == Base::Table) {
                out << "        public " << qualified(*t.tableDef) << "[] " << name << " => Array.ConvertAll(_t.Tables(" << ref << "), x => new "
                    << qualified(*t.tableDef) << "(x));\n";
            } else if (t.base == Base::Bool) {
                out << "        public bool[] " << name << " => _t.Bools(" << ref << ");\n";
            } else if (t.base == Base::Enum || t.base == Base::Struct) {
                out << "        public " << elementType(t) << "[] " << name << " => _t.List(" << ref << ", " << elementBytes(t)
                    << ", (m, at) => " << readExpr(t, "m.Span", "at") << ");\n";
            } else if (t.base == Base::UInt8) {
                out << "        public ReadOnlySpan<byte> " << name << " => _t.Bytes(" << ref << ");\n";
            } else {
                out << "        public ReadOnlySpan<" << scalar(t.base).cs << "> " << name << " => _t.Scalars<" << scalar(t.base).cs << ">(" << ref << ");\n";
            }
            return;
        }
        switch (t.base) {
        case Base::String:
            out << "        public string " << name << " => _t.String(" << f.refIndex << ");\n";
            out << "        public ReadOnlySpan<byte> " << name << "Utf8 => _t.Utf8(" << f.refIndex << ");\n";
            return;
        case Base::Table:
            out << "        public " << qualified(*t.tableDef) << "? " << name << " => _t.TableField(" << f.refIndex << ") is { } x ? new "
                << qualified(*t.tableDef) << "(x) : null;\n";
            return;
        case Base::Struct: {
            const std::string sname = qualified(*t.structDef);
            out << "        public " << sname << " " << name << "\n        {\n            get\n            {\n"
                << "                int a = _t.StructAt(" << offset << ", " << t.structDef->size << ");\n"
                << "                return a < 0 ? " << sname << ".Read(new byte[" << t.structDef->size << "], 0) : " << sname << ".Read(_t.Buffer.Span, a);\n"
                << "            }\n        }\n";
            return;
        }
        case Base::Bool:
            out << "        public bool " << name << " => _t.Bit(" << offset << ", " << f.bitOffset % 8 << ", " << defaultArg(f) << ");\n";
            return;
        case Base::Enum: {
            const ScalarInfo info = scalar(t.enumDef->underlying);
            out << "        public " << qualified(*t.enumDef) << " " << name << " => (" << qualified(*t.enumDef) << ")_t." << info.table << "(" << offset << ", "
                << defaultArg(f) << ");\n";
            return;
        }
        default: {
            const ScalarInfo info = scalar(t.base);
            out << "        public " << info.cs << " " << name << " => _t." << info.table << "(" << offset << ", " << defaultArg(f) << ");\n";
            return;
        }
        }
    }

    void emitWriteField(std::ostream& out, const TableDef& table, const Field& f) {
        const Type& t = f.type;
        const std::string member_ = member(table.name, f.name);
        const std::string value = "init." + member_;
        const std::string dataBytes = std::to_string(table.dataBytes);
        out << "            if (" << value << " != null)\n            {\n";
        const bool nullableValue = !t.vector && t.base != Base::String && t.base != Base::Table;  // int? bool? Enum? Struct?
        const std::string v = nullableValue ? value + ".Value" : value;
        if (t.vector) {
            const std::string ref = std::to_string(f.refIndex);
            const std::string size = std::to_string(elementBytes(t));
            const std::string align = std::to_string(elementAlign(t));
            if (t.base == Base::String) {
                out << "                int vector = mb.NewVector(" << value << ".Length, 4, 4);\n"
                    << "                for (int i = 0; i < " << value << ".Length; i++) mb.SetElementRef(vector, i, mb.NewString(" << value << "[i]));\n";
            } else if (t.base == Base::Table) {
                out << "                int vector = mb.NewVector(" << value << ".Length, 4, 4);\n"
                    << "                for (int i = 0; i < " << value << ".Length; i++) mb.SetElementRef(vector, i, " << qualified(*t.tableDef) << ".Write(mb, "
                    << value << "[i]));\n";
            } else if (t.base == Base::UInt8) {
                out << "                int vector = mb.ByteVector(" << value << ");\n";
            } else {
                out << "                int vector = mb.NewVector(" << value << ".Length, " << size << ", " << align << ");\n"
                    << "                for (int i = 0; i < " << value << ".Length; i++)\n                {\n                    "
                    << writeStmt(t, "mb.Span", "vector + 4 + i * " + size, value + "[i]") << "\n                }\n";
            }
            out << "                mb.SetRef(t, " << dataBytes << ", " << ref << ", vector);\n";
        } else {
            switch (t.base) {
            case Base::String:
                out << "                mb.SetRef(t, " << dataBytes << ", " << f.refIndex << ", mb.NewString(" << value << "));\n";
                break;
            case Base::Table:
                out << "                mb.SetRef(t, " << dataBytes << ", " << f.refIndex << ", " << qualified(*t.tableDef) << ".Write(mb, " << value << "));\n";
                break;
            case Base::Struct:
                out << "                " << writeStmt(t, "mb.Span", "t + " + std::to_string(f.bitOffset / 8), v) << "\n";
                break;
            case Base::Bool:
                out << "                mb.SetBit(t + " << f.bitOffset / 8 << ", " << f.bitOffset % 8 << ", " << v << ", " << defaultArg(f) << ");\n";
                break;
            default: {
                const Base base = t.base == Base::Enum ? t.enumDef->underlying : t.base;
                const ScalarInfo info = scalar(base);
                const std::string raw = t.base == Base::Enum ? "(" + std::string(info.cs) + ")" + v : v;
                out << "                mb." << info.setter << "(t + " << f.bitOffset / 8 << ", " << raw << ", " << defaultArg(f) << ");\n";
                break;
            }
            }
        }
        out << "            }\n";
    }

    void emitTable(std::ostream& out, const TableDef& table) {
        const std::string name = table.name;
        doc(out, table.doc, "    ");
        out << "    public sealed class " << name << "Init\n    {\n";
        for (const Field& f : table.fields) {
            if (f.deprecated)
                continue;
            doc(out, f.doc, "        ");
            const Type& t = f.type;
            std::string type = initType(t);
            const bool valueType = !t.vector && t.base != Base::String && t.base != Base::Table;
            out << "        public " << type << (valueType || true ? "?" : "") << " " << member(name + "Init", f.name) << ";\n";
        }
        out << "    }\n\n";

        doc(out, table.doc, "    ");
        out << "    public readonly struct " << name << "\n    {\n"
            << "        public const uint TypeId = " << hex32(typeIdOf(table.qualified())) << ";\n"
            << "        public const string TypeName = \"" << table.qualified() << "\";\n"
            << "        private readonly Table _t;\n\n"
            << "        public " << name << "(Table table) { _t = table; }\n\n"
            << "        /// <summary>Reads the root of a received message; throws if it is not a " << table.qualified() << ".</summary>\n"
            << "        public static " << name << " Decode(ReadOnlyMemory<byte> data) => new " << name << "(Table.Root(data, TypeId, TypeName));\n\n"
            << "        /// <summary>Builds a message.</summary>\n"
            << "        public static byte[] Encode(" << name << "Init init)\n        {\n"
            << "            var mb = new MessageBuilder(TypeId);\n            Write(mb, init);\n            return mb.Finish();\n        }\n\n"
            << "        /// <summary>Writes <c>init</c> as a table of the message being built; returns the table's offset.</summary>\n"
            << "        public static int Write(MessageBuilder mb, " << name << "Init init)\n        {\n"
            << "            int t = mb.NewTable(" << table.dataBytes << ", " << table.refCount << ");\n";
        for (const Field& f : table.fields)
            if (!f.deprecated)
                emitWriteField(out, table, f);
        out << "            return t;\n        }\n\n";
        for (const Field& f : table.fields) {
            if (f.deprecated)
                continue;
            emitGetter(out, table, f);
            out << "\n";
        }
        out << "    }\n\n";
    }


    // Hooks for programs that add to the output (librpc's rpcc: the clients of rpc_services).
    virtual void emitService(std::ostream&, const ServiceDef&) {}
    virtual std::string extraUsings() { return ""; }
    virtual std::string runtimeNamespace() { return options_.runtime.empty() ? "Bufs" : options_.runtime; }

    const Schema& schema_;
    const GenOptions& options_;
};

} // namespace bufsc
