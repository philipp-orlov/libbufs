// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// bufsc's model of a schema: what the parser produces, layout() completes and the
// generator and the compatibility check read.

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bufsc {

// A located error: file:line: message.
class SchemaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Base {
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    String,
    Enum,
    Struct,
    Table,
};

struct EnumDef;
struct StructDef;
struct TableDef;

struct Type {
    Base base = Base::Int32;
    bool vector = false;       // [T]
    uint32_t fixedLength = 0;  // [T:N], structs only
    std::string name;          // as written, for enum/struct/table
    const EnumDef* enumDef = nullptr;
    const StructDef* structDef = nullptr;
    const TableDef* tableDef = nullptr;
};

struct Field {
    std::string name;
    Type type;
    std::string defaultText; // as written; empty for none
    bool deprecated = false;
    std::vector<std::string> doc;
    int line = 0;

    // layout(): data fields get a bit offset in the table (or a byte offset in
    // a struct), reference fields a slot index.
    uint32_t bitOffset = 0;
    int refIndex = -1;
};

struct Definition {
    std::string name;        // unqualified
    std::string ns;          // "a.b", may be empty
    std::string file;        // the schema file that declares it
    std::vector<std::string> doc;
    int line = 0;

    std::string qualified() const { return ns.empty() ? name : ns + "." + name; }
};

struct EnumDef : Definition {
    Base underlying = Base::Int32;
    std::vector<std::pair<std::string, int64_t>> values;
};

struct StructDef : Definition {
    std::vector<Field> fields;
    uint32_t size = 0;
    uint32_t align = 1;
};

struct TableDef : Definition {
    std::vector<Field> fields;
    uint32_t dataBytes = 4;
    uint32_t refCount = 0;
};

// Every definition of a schema and the files it includes, in declaration order.
struct Schema {
    std::vector<std::unique_ptr<EnumDef>> enums;
    std::vector<std::unique_ptr<StructDef>> structs;
    std::vector<std::unique_ptr<TableDef>> tables;
    std::string mainFile;                 // the file given on the command line
    std::vector<std::string> includes;    // as written in the main file
    std::string rootType;                 // qualified, may be empty
    std::vector<std::string> files;       // every schema file read, for build dependencies

    const EnumDef* findEnum(const std::string& qualified) const;
    const StructDef* findStruct(const std::string& qualified) const;
    const TableDef* findTable(const std::string& qualified) const;
};

// Parses `path` and everything it includes (searched next to the including
// file, then in `includeDirs`), resolves names and computes layouts. Throws
// SchemaError.
Schema load(const std::string& path, const std::vector<std::string>& includeDirs);

// Size and alignment of a scalar base type, in bytes.
uint32_t scalarBytes(Base base);
bool isScalar(Base base);
bool isInteger(Base base);
bool isFloat(Base base);
const char* baseName(Base base);

// The C++ type that holds `type` (one element when it is a vector).
std::string cppElementType(const Type& type);

// Writes `<stem>.bufs.hpp` for the main file of `schema` into `outDir`.
// Returns the path written.
std::string generateCpp(const Schema& schema, const std::string& outDir);

// Compares two versions of a schema; prints every incompatibility to stderr.
// True when messages written with either can be read with the other.
bool checkCompatible(const Schema& older, const Schema& newer);

// FNV-1a over the qualified name: the typeId of a table or struct.
uint32_t typeIdOf(const std::string& qualified);

} // namespace bufsc
