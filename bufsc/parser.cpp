// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The schema language: a FlatBuffers-style subset.
//
//   include "other.bufs";
//   namespace a.b;
//   enum Color : ubyte { Red, Green = 4, Blue }
//   struct Vec3 { x:float; y:float; z:float; }
//   struct Embedding { values:[float:128]; }
//   table Monster { pos:Vec3; hp:short = 100; name:string; tags:[string];
//                   friends:[Monster]; color:Color = Blue; old:int (deprecated); }
//   root_type Monster;
//
// attribute, file_identifier and file_extension are accepted and ignored.
// union is rejected: dispatch on a message's typeId instead.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include "schema.hpp"

namespace bufsc {

namespace {

struct Token {
    enum Kind { End, Ident, Number, String, Punct } kind = End;
    std::string text;
    int line = 0;
};

class Lexer {
public:
    Lexer(std::string text, std::string file) : text_(std::move(text)), file_(std::move(file)) {}

    // Doc comments (///) seen since the last token, for the next declaration.
    std::vector<std::string> takeDoc() { return std::exchange(doc_, {}); }

    Token next() {
        skip();
        Token t;
        t.line = line_;
        if (at_ >= text_.size())
            return t;
        const char c = text_[at_];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const size_t start = at_;
            while (at_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[at_])) || text_[at_] == '_'))
                ++at_;
            t.kind = Token::Ident;
            t.text = text_.substr(start, at_ - start);
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && at_ + 1 < text_.size() && std::isdigit(static_cast<unsigned char>(text_[at_ + 1]))) ||
            ((c == '-' || c == '+') && at_ + 1 < text_.size() &&
             (std::isdigit(static_cast<unsigned char>(text_[at_ + 1])) || text_[at_ + 1] == '.' ||
              std::isalpha(static_cast<unsigned char>(text_[at_ + 1]))))) {
            const size_t start = at_;
            ++at_;
            while (at_ < text_.size() &&
                   (std::isalnum(static_cast<unsigned char>(text_[at_])) || text_[at_] == '.' ||
                    ((text_[at_] == '-' || text_[at_] == '+') &&
                     (text_[at_ - 1] == 'e' || text_[at_ - 1] == 'E'))))
                ++at_;
            t.kind = Token::Number;
            t.text = text_.substr(start, at_ - start);
            return t;
        }
        if (c == '"') {
            const size_t start = ++at_;
            while (at_ < text_.size() && text_[at_] != '"' && text_[at_] != '\n')
                ++at_;
            if (at_ >= text_.size() || text_[at_] != '"')
                fail("unterminated string");
            t.kind = Token::String;
            t.text = text_.substr(start, at_ - start);
            ++at_;
            return t;
        }
        t.kind = Token::Punct;
        t.text = std::string(1, c);
        ++at_;
        return t;
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw SchemaError(file_ + ":" + std::to_string(line_) + ": " + message);
    }

private:
    void skip() {
        for (;;) {
            while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_]))) {
                if (text_[at_] == '\n')
                    ++line_;
                ++at_;
            }
            if (text_.compare(at_, 3, "///") == 0) {
                const size_t end = text_.find('\n', at_);
                std::string line = text_.substr(at_ + 3, (end == std::string::npos ? text_.size() : end) - at_ - 3);
                if (!line.empty() && line[0] == ' ')
                    line.erase(0, 1);
                doc_.push_back(line);
                at_ = end == std::string::npos ? text_.size() : end;
                continue;
            }
            if (text_.compare(at_, 2, "//") == 0) {
                const size_t end = text_.find('\n', at_);
                at_ = end == std::string::npos ? text_.size() : end;
                continue;
            }
            if (text_.compare(at_, 2, "/*") == 0) {
                const size_t end = text_.find("*/", at_ + 2);
                if (end == std::string::npos)
                    fail("unterminated comment");
                line_ += static_cast<int>(std::count(text_.begin() + static_cast<long>(at_),
                                                     text_.begin() + static_cast<long>(end), '\n'));
                at_ = end + 2;
                continue;
            }
            return;
        }
    }

    std::string text_;
    std::string file_;
    size_t at_ = 0;
    int line_ = 1;
    std::vector<std::string> doc_;
};

bool fitsIn(int64_t value, Base base) {
    switch (base) {
    case Base::Int8: return value >= INT8_MIN && value <= INT8_MAX;
    case Base::UInt8: return value >= 0 && value <= UINT8_MAX;
    case Base::Int16: return value >= INT16_MIN && value <= INT16_MAX;
    case Base::UInt16: return value >= 0 && value <= UINT16_MAX;
    case Base::Int32: return value >= INT32_MIN && value <= INT32_MAX;
    case Base::UInt32: return value >= 0 && value <= static_cast<int64_t>(UINT32_MAX);
    case Base::UInt64: return value >= 0;
    default: return true;
    }
}

const std::map<std::string, Base>& scalarNames() {
    static const std::map<std::string, Base> names = {
        {"bool", Base::Bool},       {"byte", Base::Int8},       {"int8", Base::Int8},     {"ubyte", Base::UInt8},
        {"uint8", Base::UInt8},     {"short", Base::Int16},     {"int16", Base::Int16},   {"ushort", Base::UInt16},
        {"uint16", Base::UInt16},   {"int", Base::Int32},       {"int32", Base::Int32},   {"uint", Base::UInt32},
        {"uint32", Base::UInt32},   {"long", Base::Int64},      {"int64", Base::Int64},   {"ulong", Base::UInt64},
        {"uint64", Base::UInt64},   {"float", Base::Float32},   {"float32", Base::Float32},
        {"double", Base::Float64},  {"float64", Base::Float64}, {"string", Base::String},
    };
    return names;
}

class Parser {
public:
    Parser(Schema& schema, std::vector<std::string> includeDirs) : schema_(schema), includeDirs_(std::move(includeDirs)) {}

    void parseFile(const std::string& path, bool main) {
        std::string canonical = path;
        {
            char* real = ::realpath(path.c_str(), nullptr);
            if (real) {
                canonical = real;
                std::free(real);
            }
        }
        if (!seen_.insert(canonical).second)
            return;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw SchemaError(path + ": cannot open");
        std::stringstream text;
        text << in.rdbuf();

        Lexer lexer(text.str(), path);
        lexer_ = &lexer;
        file_ = path;
        ns_.clear();
        advance();
        while (token_.kind != Token::End) {
            const std::vector<std::string> doc = lexer.takeDoc();
            const Token keyword = expect(Token::Ident, "a declaration");
            if (keyword.text == "include") {
                const std::string name = expect(Token::String, "an include path").text;
                expectPunct(";");
                if (main)
                    schema_.includes.push_back(name);
                const std::string found = locate(name, path, keyword.line);
                Lexer* saved = lexer_;
                const std::string savedFile = file_;
                const std::string savedNs = ns_;
                const Token savedToken = token_;
                parseFile(found, false);
                lexer_ = saved;
                file_ = savedFile;
                ns_ = savedNs;
                token_ = savedToken;
            } else if (keyword.text == "namespace") {
                ns_ = qualifiedName();
                expectPunct(";");
            } else if (keyword.text == "enum") {
                parseEnum(doc, keyword.line);
            } else if (keyword.text == "struct") {
                parseStruct(doc, keyword.line);
            } else if (keyword.text == "table") {
                parseTable(doc, keyword.line);
            } else if (keyword.text == "root_type") {
                const std::string name = qualifiedName();
                expectPunct(";");
                if (main)
                    rootType_ = {name, ns_, keyword.line};
            } else if (keyword.text == "attribute" || keyword.text == "file_identifier" ||
                       keyword.text == "file_extension") {
                advance();
                expectPunct(";");
            } else if (keyword.text == "union") {
                fail(keyword.line, "unions are not supported: send each type as its own message and dispatch on "
                                   "bufs::typeOf()");
            } else if (keyword.text == "rpc_service") {
                parseService(doc, keyword.line);
            } else {
                fail(keyword.line, "unexpected '" + keyword.text + "'");
            }
        }
        lexer_ = nullptr;
    }

    // Names that refer to other definitions, once every file is read.
    void resolve() {
        auto resolveFields = [&](const Definition& owner, std::vector<Field>& fields) {
            for (Field& f : fields) {
                Type& t = f.type;
                if (t.name.empty())
                    continue;
                if (!find(t, owner.ns))
                    throw SchemaError(owner.file + ":" + std::to_string(f.line) + ": unknown type '" + t.name + "'");
            }
        };
        for (auto& s : schema_.structs)
            resolveFields(*s, s->fields);
        for (auto& t : schema_.tables)
            resolveFields(*t, t->fields);
        for (auto& s : schema_.services) {
            for (RpcMethod& m : s->methods) {
                if (m.natural)
                    lowerNaturalMethod(*s, m);
                else
                    resolveTableMethod(*s, m);
            }
        }
        if (!rootType_.name.empty()) {
            Type t;
            t.name = rootType_.name;
            if (!find(t, rootType_.ns) || t.base == Base::Enum)
                throw SchemaError(schema_.mainFile + ":" + std::to_string(rootType_.line) + ": root_type '" +
                                  rootType_.name + "' is not a table or struct");
            schema_.rootType = t.tableDef ? t.tableDef->qualified() : t.structDef->qualified();
        }
        schema_.files.assign(seen_.begin(), seen_.end());
    }

private:
    struct RootType {
        std::string name;
        std::string ns;
        int line = 0;
    };

    // As FlatBuffers: the current namespace and its parents, innermost first;
    // in each scope any kind of definition matches.
    bool find(Type& t, const std::string& ns) const {
        std::string scope = ns;
        for (;;) {
            const std::string wanted = scope.empty() ? t.name : scope + "." + t.name;
            if (const EnumDef* e = schema_.findEnum(wanted)) {
                t.base = Base::Enum;
                t.enumDef = e;
                return true;
            }
            if (const StructDef* s = schema_.findStruct(wanted)) {
                t.base = Base::Struct;
                t.structDef = s;
                return true;
            }
            if (const TableDef* tb = schema_.findTable(wanted)) {
                t.base = Base::Table;
                t.tableDef = tb;
                return true;
            }
            if (scope.empty())
                return false;
            const size_t dot = scope.rfind('.');
            scope = dot == std::string::npos ? std::string() : scope.substr(0, dot);
        }
    }

    void resolveTrailer(const ServiceDef& s, RpcMethod& m) {
        const auto found = m.attributes.find("trailer");
        if (found == m.attributes.end())
            return;
        const std::string where = s.file + ":" + std::to_string(m.line) + ": " + s.name + "." + m.name + ": ";
        if (m.streaming != "server")
            throw SchemaError(where + "a trailer belongs to a streaming method");
        Type t;
        t.name = found->second;
        if (!find(t, s.ns) || t.base != Base::Table)
            throw SchemaError(where + "trailer '" + found->second + "' is not a table");
        m.trailerDef = t.tableDef;
    }

    // Name(Request):Response
    void resolveTableMethod(const ServiceDef& s, RpcMethod& m) {
        auto table = [&](const std::string& name, const TableDef*& out) {
            Type t;
            t.name = name;
            if (!find(t, s.ns) || t.base != Base::Table)
                throw SchemaError(s.file + ":" + std::to_string(m.line) + ": '" + name + "' (" + s.name + "." + m.name +
                                  ") is not a table");
            out = t.tableDef;
        };
        table(m.request, m.requestDef);
        table(m.response, m.responseDef);
        resolveTrailer(s, m);
    }

    // Name(a:double, b:string):result -> a request table, and a response table unless the result is one.
    void lowerNaturalMethod(const ServiceDef& s, RpcMethod& m) {
        const std::string where = s.file + ":" + std::to_string(m.line) + ": " + s.name + "." + m.name + ": ";
        auto resolveType = [&](Type& t, const std::string& what) {
            if (!t.name.empty() && !find(t, s.ns))
                throw SchemaError(where + "unknown type '" + t.name + "' of " + what);
            if (t.vector && t.base == Base::Table)
                throw SchemaError(where + what + " is a vector of tables; wrap them in a table and use that");
        };
        for (Field& f : m.params)
            resolveType(f.type, "parameter '" + f.name + "'");
        if (!m.isVoid)
            resolveType(m.result, "the result");
        if (m.inputStream)
            resolveType(m.input.type, "stream parameter '" + m.input.name + "'");

        auto synthesize = [&](const std::string& suffix, std::vector<Field> fields) {
            auto def = std::make_unique<TableDef>();
            def->name = s.name + m.name + suffix;
            def->ns = s.ns;
            def->file = s.file;
            def->line = m.line;
            def->fields = std::move(fields);
            if (!names_.insert(def->qualified()).second)
                throw SchemaError(where + "the generated table '" + def->qualified() + "' collides with another definition");
            const TableDef* raw = def.get();
            schema_.tables.push_back(std::move(def));
            return raw;
        };
        m.requestDef = synthesize("Request", m.params);
        if (m.inputStream) {
            if (m.input.type.base == Base::Table && !m.input.type.vector) {
                m.inputDef = m.input.type.tableDef;
            } else {
                Field item = m.input;
                item.name = "item";
                m.inputDef = synthesize("Item", {std::move(item)});
                m.inputWrapped = true;
            }
        }
        if (!m.isVoid && m.result.base == Base::Table && !m.result.vector) {
            m.responseDef = m.result.tableDef;
        } else {
            std::vector<Field> fields;
            if (!m.isVoid) {
                Field f;
                f.name = "result";
                f.type = m.result;
                f.line = m.line;
                fields.push_back(std::move(f));
                m.resultWrapped = true;
            }
            m.responseDef = synthesize("Response", std::move(fields));
        }
        resolveTrailer(s, m);
    }

    std::string locate(const std::string& name, const std::string& from, int line) {
        std::vector<std::string> candidates;
        const size_t slash = from.rfind('/');
        candidates.push_back(slash == std::string::npos ? name : from.substr(0, slash + 1) + name);
        for (const std::string& dir : includeDirs_)
            candidates.push_back(dir + "/" + name);
        for (const std::string& c : candidates)
            if (std::ifstream(c))
                return c;
        fail(line, "cannot find include '" + name + "'");
    }

    void parseEnum(const std::vector<std::string>& doc, int line) {
        auto def = std::make_unique<EnumDef>();
        start(*def, doc, line);
        expectPunct(":");
        const Token type = expect(Token::Ident, "the enum's integer type");
        const auto found = scalarNames().find(type.text);
        if (found == scalarNames().end() || !isInteger(found->second))
            fail(type.line, "an enum's type must be an integer type");
        def->underlying = found->second;
        skipAttributes();
        expectPunct("{");
        int64_t next = 0;
        while (!isPunct("}")) {
            const Token name = expect(Token::Ident, "an enum value");
            int64_t value = next;
            if (isPunct("=")) {
                advance();
                const Token number = expect(Token::Number, "a value");
                value = parseInteger(number);
            }
            for (const auto& [existing, v] : def->values)
                if (existing == name.text)
                    fail(name.line, "duplicate enum value '" + name.text + "'");
            if (!fitsIn(value, def->underlying))
                fail(name.line, "value " + std::to_string(value) + " does not fit " + baseName(def->underlying));
            def->values.emplace_back(name.text, value);
            next = value + 1;
            if (isPunct(","))
                advance();
            else if (!isPunct("}"))
                fail(token_.line, "expected ',' or '}'");
        }
        advance();
        if (def->values.empty())
            fail(line, "enum '" + def->name + "' has no values");
        add(schema_.enums, std::move(def));
    }

    // rpc_service Name { Method(Request):Response (streaming: "server"); ... }
    // The request and response must be tables (checked in resolve()).
    void parseService(const std::vector<std::string>& doc, int line) {
        auto def = std::make_unique<ServiceDef>();
        start(*def, doc, line);
        expectPunct("{");
        while (!isPunct("}")) {
            RpcMethod m;
            m.doc = lexer_->takeDoc();
            const Token name = expect(Token::Ident, "a method name");
            m.name = name.text;
            m.line = name.line;
            for (const RpcMethod& other : def->methods)
                if (other.name == m.name)
                    fail(name.line, "duplicate method '" + m.name + "'");
            expectPunct("(");
            if (isPunct(")")) {
                m.natural = true;
                advance();
            } else {
                const Token first = expect(Token::Ident, "a parameter or a request type");
                if (isPunct(":")) {  // Name(a:double, b:string = "x"):result
                    m.natural = true;
                    std::string pname = first.text;
                    int pline = first.line;
                    for (;;) {
                        advance();  // the ':'
                        Field f;
                        f.name = pname;
                        f.line = pline;
                        bool streamed = false;
                        if (token_.kind == Token::Ident && token_.text == "stream") {
                            advance();
                            streamed = true;
                        }
                        f.type = parseType(true);
                        if (streamed) {
                            if (m.inputStream)
                                fail(f.line, "a method has at most one stream parameter");
                            if (isPunct("="))
                                fail(token_.line, "a stream parameter takes no default");
                            if (isPunct("("))
                                fail(token_.line, "a stream parameter cannot be (shared); mark a field of its item table");
                            m.inputStream = true;
                            m.input = std::move(f);
                            if (isPunct(",")) {
                                advance();
                                const Token next = expect(Token::Ident, "a parameter name");
                                pname = next.text;
                                pline = next.line;
                                if (!isPunct(":"))
                                    fail(next.line, "expected ':' after the parameter name");
                                continue;
                            }
                            break;
                        }
                        if (isPunct("=")) {
                            advance();
                            if (token_.kind != Token::Number && token_.kind != Token::Ident)
                                fail(token_.line, "expected a default value");
                            f.defaultText = token_.text;
                            advance();
                        }
                        if (isPunct("("))
                            parseFieldAttributes(f, true, true);
                        for (const Field& other : m.params)
                            if (other.name == f.name)
                                fail(f.line, "duplicate parameter '" + f.name + "'");
                        m.params.push_back(std::move(f));
                        if (isPunct(",")) {
                            advance();
                            const Token next = expect(Token::Ident, "a parameter name");
                            pname = next.text;
                            pline = next.line;
                            if (!isPunct(":"))
                                fail(next.line, "expected ':' after the parameter name");
                            continue;
                        }
                        break;
                    }
                    expectPunct(")");
                } else {  // Name(Request):Response
                    std::string name = first.text;
                    while (isPunct(".")) {
                        advance();
                        name += "." + expect(Token::Ident, "a name").text;
                    }
                    m.request = name;
                    expectPunct(")");
                }
            }
            expectPunct(":");
            if (token_.kind == Token::Ident && token_.text == "void" && m.natural) {
                m.isVoid = true;
                advance();
            } else if (token_.kind == Token::Ident && token_.text == "stream") {
                advance();
                m.streaming = "server";
                m.attributes["streaming"] = "server";
                if (m.natural)
                    m.result = parseType(true);
                else
                    m.response = qualifiedName();
            } else if (m.natural) {
                m.result = parseType(true);
            } else {
                m.response = qualifiedName();
            }
            if (isPunct("(")) {
                advance();
                while (!isPunct(")")) {
                    const Token key = expect(Token::Ident, "an attribute name");
                    std::string value;
                    if (isPunct(":")) {
                        advance();
                        if (token_.kind != Token::String && token_.kind != Token::Number && token_.kind != Token::Ident)
                            fail(token_.line, "expected an attribute value");
                        value = token_.text;
                        advance();
                    }
                    m.attributes[key.text] = value;
                    if (isPunct(","))
                        advance();
                    else if (!isPunct(")"))
                        fail(token_.line, "expected ',' or ')'");
                }
                advance();
            }
            expectPunct(";");
            const auto streaming = m.attributes.find("streaming");
            if (streaming != m.attributes.end() && !(m.natural && streaming->second == "server" && m.streaming == "server")) {
                m.streaming = streaming->second;
                if (m.streaming != "none" && m.streaming != "server" && m.streaming != "client" &&
                    m.streaming != "bidi")
                    fail(m.line, "streaming must be \"none\", \"server\", \"client\" or \"bidi\"");
            }
            if (m.natural && m.inputStream)
                m.streaming = m.streaming == "server" ? "bidi" : "client";
            def->methods.push_back(std::move(m));
        }
        advance();
        if (def->methods.empty())
            fail(line, "rpc_service '" + def->name + "' has no methods");
        add(schema_.services, std::move(def));
    }

    void parseStruct(const std::vector<std::string>& doc, int line) {
        auto def = std::make_unique<StructDef>();
        start(*def, doc, line);
        skipAttributes();
        expectPunct("{");
        while (!isPunct("}"))
            def->fields.push_back(parseField(false));
        advance();
        if (def->fields.empty())
            fail(line, "struct '" + def->name + "' has no fields");
        add(schema_.structs, std::move(def));
    }

    void parseTable(const std::vector<std::string>& doc, int line) {
        auto def = std::make_unique<TableDef>();
        start(*def, doc, line);
        skipAttributes();
        expectPunct("{");
        while (!isPunct("}"))
            def->fields.push_back(parseField(true));
        advance();
        add(schema_.tables, std::move(def));
    }

    // `( deprecated, shared )` after a field's type. `parameter`: a natural-form service parameter, which takes
    // `shared` only.
    void parseFieldAttributes(Field& f, bool inTable, bool parameter) {
        advance();  // the '('
        while (!isPunct(")")) {
            const Token attribute = expect(Token::Ident, "an attribute");
            if (attribute.text == "deprecated") {
                if (parameter)
                    fail(attribute.line, "a parameter cannot be deprecated");
                if (!inTable)
                    fail(attribute.line, "struct fields cannot be deprecated");
                f.deprecated = true;
            } else if (attribute.text == "shared") {
                if (!inTable)
                    fail(attribute.line, "struct fields cannot be shared");
                if (!f.type.vector || (f.type.base != Base::UInt8 && f.type.base != Base::Int8))
                    fail(attribute.line, "only a [ubyte] or [byte] field can be (shared)");
                f.shared = true;
                f.doc.push_back("(shared) bulk bytes: librpc sends them by reference on a local socket, inline elsewhere.");
            }
            if (isPunct(":")) {
                advance();
                advance();
            }
            if (isPunct(","))
                advance();
        }
        advance();
    }

    Field parseField(bool inTable) {
        Field f;
        f.doc = lexer_->takeDoc();
        const Token name = expect(Token::Ident, "a field name");
        f.name = name.text;
        f.line = name.line;
        expectPunct(":");
        f.type = parseType(inTable);
        if (isPunct("=")) {
            if (!inTable)
                fail(token_.line, "struct fields take no default");
            advance();
            if (token_.kind != Token::Number && token_.kind != Token::Ident)
                fail(token_.line, "expected a default value");
            f.defaultText = token_.text;
            advance();
        }
        if (isPunct("("))
            parseFieldAttributes(f, inTable, false);
        expectPunct(";");
        return f;
    }

    Type parseType(bool inTable) {
        Type t;
        const int line = token_.line;
        if (isPunct("[")) {
            advance();
            if (isPunct("["))
                fail(line, "vectors of vectors are not supported; wrap the inner vector in a table");
            Type element = parseNamed(line);
            if (isPunct(":")) {
                if (inTable)
                    fail(line, "fixed-length arrays [T:N] are for struct fields; use [T] in a table");
                advance();
                const Token n = expect(Token::Number, "an array length");
                const int64_t length = parseInteger(n);
                if (length < 1 || length > 65535)
                    fail(n.line, "array length must be 1..65535");
                element.fixedLength = static_cast<uint32_t>(length);
            } else {
                if (!inTable)
                    fail(line, "struct fields hold scalars, enums, structs and [T:N] arrays only");
                element.vector = true;
            }
            expectPunct("]");
            t = element;
        } else {
            t = parseNamed(line);
            if (!inTable && t.base == Base::String)
                fail(line, "struct fields hold scalars, enums, structs and [T:N] arrays only");
        }
        if (t.base == Base::String && t.fixedLength)
            fail(line, "[string:N] is not supported");
        if (t.fixedLength && t.base == Base::Bool)
            fail(line, "[bool:N] is not supported; use [ubyte:N]");
        return t;
    }

    Type parseNamed(int line) {
        Type t;
        if (token_.kind != Token::Ident)
            fail(line, "expected a type");
        const std::string name = qualifiedName();
        const auto found = scalarNames().find(name);
        if (found != scalarNames().end()) {
            t.base = found->second;
        } else {
            t.base = Base::Table; // resolved later
            t.name = name;
        }
        return t;
    }

    template <class Def>
    void start(Def& def, const std::vector<std::string>& doc, int line) {
        def.name = expect(Token::Ident, "a name").text;
        def.ns = ns_;
        def.file = file_;
        def.doc = doc;
        def.line = line;
        const std::string q = def.qualified();
        if (!names_.insert(q).second)
            fail(line, "'" + q + "' is declared twice");
    }

    template <class Def>
    void add(std::vector<std::unique_ptr<Def>>& list, std::unique_ptr<Def> def) {
        list.push_back(std::move(def));
    }

    std::string qualifiedName() {
        std::string name = expect(Token::Ident, "a name").text;
        while (isPunct(".")) {
            advance();
            name += "." + expect(Token::Ident, "a name").text;
        }
        return name;
    }

    void skipAttributes() {
        if (!isPunct("("))
            return;
        while (!isPunct(")")) {
            if (token_.kind == Token::End)
                fail(token_.line, "unterminated attribute list");
            advance();
        }
        advance();
    }

    int64_t parseInteger(const Token& t) {
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(t.text.c_str(), &end, 0);
        if (errno || !end || *end)
            fail(t.line, "'" + t.text + "' is not an integer");
        return v;
    }

    bool isPunct(const char* p) const { return token_.kind == Token::Punct && token_.text == p; }

    void expectPunct(const char* p) {
        if (!isPunct(p))
            fail(token_.line, std::string("expected '") + p + "'" +
                                  (token_.text.empty() ? std::string() : ", found '" + token_.text + "'"));
        advance();
    }

    Token expect(Token::Kind kind, const char* what) {
        if (token_.kind != kind)
            fail(token_.line, std::string("expected ") + what +
                                  (token_.text.empty() ? std::string() : ", found '" + token_.text + "'"));
        Token t = token_;
        advance();
        return t;
    }

    void advance() { token_ = lexer_->next(); }

    [[noreturn]] void fail(int line, const std::string& message) const {
        throw SchemaError(file_ + ":" + std::to_string(line) + ": " + message);
    }

    Schema& schema_;
    std::vector<std::string> includeDirs_;
    std::set<std::string> seen_;
    std::set<std::string> names_;
    Lexer* lexer_ = nullptr;
    std::string file_;
    std::string ns_;
    Token token_;
    RootType rootType_;
};

// ------------------------------------------------------------- validation --

// Checks a default against its field's type and normalizes it: float
// specials become "nan", "inf" or "-inf".
void validateDefault(Field& f, const std::string& file) {
    if (f.defaultText.empty())
        return;
    auto bad = [&](const std::string& why) {
        throw SchemaError(file + ":" + std::to_string(f.line) + ": default of '" + f.name + "': " + why);
    };
    const Type& t = f.type;
    if (t.vector || t.base == Base::String || t.base == Base::Struct || t.base == Base::Table)
        bad("only scalars and enums take a default");
    if (t.base == Base::Bool) {
        if (f.defaultText != "true" && f.defaultText != "false" && f.defaultText != "0" && f.defaultText != "1")
            bad("expected true or false");
        return;
    }
    if (t.base == Base::Enum) {
        for (const auto& [name, value] : t.enumDef->values)
            if (name == f.defaultText)
                return;
        bad("'" + f.defaultText + "' is not a value of " + t.enumDef->name);
    }
    if (isFloat(t.base)) {
        std::string lower;
        for (char c : f.defaultText)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "nan" || lower == "+nan" || lower == "-nan") {
            f.defaultText = "nan";
            return;
        }
        if (lower == "inf" || lower == "+inf" || lower == "infinity" || lower == "+infinity") {
            f.defaultText = "inf";
            return;
        }
        if (lower == "-inf" || lower == "-infinity") {
            f.defaultText = "-inf";
            return;
        }
        char* end = nullptr;
        errno = 0;
        const double v = std::strtod(f.defaultText.c_str(), &end);
        if (!end || *end || errno || !std::isfinite(v))
            bad("'" + f.defaultText + "' is not a number");
        if (t.base == Base::Float32) {
            // A finite double can still overflow float32; generate_cpp.cpp
            // emits this value as a float literal, which must round-trip.
            errno = 0;
            const float narrow = std::strtof(f.defaultText.c_str(), &end);
            if (errno || !std::isfinite(narrow))
                bad("'" + f.defaultText + "' does not fit float32");
        }
        return;
    }
    errno = 0;
    char* end = nullptr;
    if (t.base == Base::UInt64) {
        if (f.defaultText[0] == '-')
            bad("negative default for an unsigned field");
        std::strtoull(f.defaultText.c_str(), &end, 0);
    } else {
        const long long v = std::strtoll(f.defaultText.c_str(), &end, 0);
        if (!errno && end && !*end && !fitsIn(v, t.base))
            bad("'" + f.defaultText + "' does not fit " + baseName(t.base));
    }
    if (errno || !end || *end)
        bad("'" + f.defaultText + "' is not an integer");
}

// ----------------------------------------------------------------- layout --

uint32_t fieldBytes(const Type& t) {
    uint32_t one = 0;
    if (t.base == Base::Struct)
        one = t.structDef->size;
    else if (t.base == Base::Enum)
        one = scalarBytes(t.enumDef->underlying);
    else
        one = scalarBytes(t.base);
    return one * (t.fixedLength ? t.fixedLength : 1);
}

uint32_t fieldAlign(const Type& t) {
    if (t.base == Base::Struct)
        return t.structDef->align;
    if (t.base == Base::Enum)
        return scalarBytes(t.enumDef->underlying);
    return scalarBytes(t.base);
}

void layoutStruct(StructDef& s, std::set<const StructDef*>& visiting) {
    if (s.size)
        return;
    if (!visiting.insert(&s).second)
        throw SchemaError(s.file + ":" + std::to_string(s.line) + ": struct '" + s.name + "' contains itself");
    uint32_t at = 0;
    s.align = 1;
    for (Field& f : s.fields) {
        if (f.type.base == Base::Table)
            throw SchemaError(s.file + ":" + std::to_string(f.line) + ": a struct cannot hold the table '" +
                              f.type.name + "'");
        if (f.type.base == Base::Struct)
            layoutStruct(const_cast<StructDef&>(*f.type.structDef), visiting);
        const uint32_t align = fieldAlign(f.type);
        at = (at + align - 1) / align * align;
        f.bitOffset = at * 8;
        at += fieldBytes(f.type);
        s.align = std::max(s.align, align);
    }
    s.size = (at + s.align - 1) / s.align * s.align;
    if (s.align > 8)
        throw SchemaError(s.file + ":" + std::to_string(s.line) + ": struct alignment above 8");
    visiting.erase(&s);
}

// Cap'n Proto's rule: each field, in declaration order, takes the lowest free
// position aligned to its own size. A field added later only fills holes or
// extends the section, so existing fields never move.
void layoutTable(TableDef& t) {
    std::vector<bool> used(32, true); // the 4-byte table header
    int refs = 0;
    for (Field& f : t.fields) {
        const Type& type = f.type;
        if (type.vector || type.base == Base::String || type.base == Base::Table) {
            f.refIndex = refs++;
            continue;
        }
        const uint32_t bits = type.base == Base::Bool ? 1 : fieldBytes(type) * 8;
        const uint32_t align = type.base == Base::Bool ? 1 : fieldAlign(type) * 8;
        uint32_t pos = 0;
        for (;; pos += align) {
            if (used.size() < pos + bits)
                used.resize(pos + bits, false);
            bool free = true;
            for (uint32_t b = pos; b < pos + bits && free; ++b)
                free = !used[b];
            if (free)
                break;
        }
        for (uint32_t b = pos; b < pos + bits; ++b)
            used[b] = true;
        f.bitOffset = pos;
    }
    size_t end = used.size();
    while (end > 32 && !used[end - 1])
        --end;
    t.dataBytes = static_cast<uint32_t>((end + 7) / 8);
    t.refCount = static_cast<uint32_t>(refs);
    if (t.dataBytes > 65535 || t.refCount > 65535)
        throw SchemaError(t.file + ":" + std::to_string(t.line) + ": table '" + t.name + "' is too large");
}

} // namespace

const EnumDef* Schema::findEnum(const std::string& q) const {
    for (const auto& d : enums)
        if (d->qualified() == q)
            return d.get();
    return nullptr;
}
const StructDef* Schema::findStruct(const std::string& q) const {
    for (const auto& d : structs)
        if (d->qualified() == q)
            return d.get();
    return nullptr;
}
const TableDef* Schema::findTable(const std::string& q) const {
    for (const auto& d : tables)
        if (d->qualified() == q)
            return d.get();
    return nullptr;
}

uint32_t typeIdOf(const std::string& qualified) {
    uint32_t hash = 2166136261u;
    for (char c : qualified) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

Schema load(const std::string& path, const std::vector<std::string>& includeDirs) {
    Schema schema;
    schema.mainFile = path;
    Parser parser(schema, includeDirs);
    parser.parseFile(path, true);
    parser.resolve();

    std::set<const StructDef*> visiting;
    for (auto& s : schema.structs)
        layoutStruct(*s, visiting);
    for (auto& s : schema.structs)
        for (Field& f : s->fields)
            validateDefault(f, s->file);
    for (auto& t : schema.tables) {
        std::set<std::string> names;
        for (Field& f : t->fields) {
            if (!names.insert(f.name).second)
                throw SchemaError(t->file + ":" + std::to_string(f.line) + ": duplicate field '" + f.name + "'");
            validateDefault(f, t->file);
        }
        layoutTable(*t);
    }
    std::map<uint32_t, std::string> ids;
    auto claim = [&](const Definition& d) {
        const uint32_t id = typeIdOf(d.qualified());
        const auto [it, fresh] = ids.emplace(id, d.qualified());
        if (!fresh)
            throw SchemaError(d.file + ":" + std::to_string(d.line) + ": typeId of '" + d.qualified() +
                              "' collides with '" + it->second + "'; rename one");
    };
    for (const auto& t : schema.tables)
        claim(*t);
    for (const auto& s : schema.structs)
        claim(*s);
    return schema;
}

uint32_t scalarBytes(Base base) {
    switch (base) {
    case Base::Bool:
    case Base::Int8:
    case Base::UInt8: return 1;
    case Base::Int16:
    case Base::UInt16: return 2;
    case Base::Int32:
    case Base::UInt32:
    case Base::Float32: return 4;
    case Base::Int64:
    case Base::UInt64:
    case Base::Float64: return 8;
    default: return 0;
    }
}

bool isScalar(Base base) { return base <= Base::Float64; }
bool isInteger(Base base) { return base >= Base::Int8 && base <= Base::UInt64; }
bool isFloat(Base base) { return base == Base::Float32 || base == Base::Float64; }

const char* baseName(Base base) {
    switch (base) {
    case Base::Bool: return "bool";
    case Base::Int8: return "byte";
    case Base::UInt8: return "ubyte";
    case Base::Int16: return "short";
    case Base::UInt16: return "ushort";
    case Base::Int32: return "int";
    case Base::UInt32: return "uint";
    case Base::Int64: return "long";
    case Base::UInt64: return "ulong";
    case Base::Float32: return "float";
    case Base::Float64: return "double";
    case Base::String: return "string";
    case Base::Enum: return "enum";
    case Base::Struct: return "struct";
    case Base::Table: return "table";
    }
    return "?";
}

} // namespace bufsc
