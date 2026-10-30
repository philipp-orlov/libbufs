// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// bufsc: the libbufs schema compiler.
//
//   bufsc [-o DIR] [-I DIR]... SCHEMA.bufs...      writes DIR/<stem>.bufs.hpp per schema
//   bufsc ... --ts --cs --py [--no-cpp]            also (or instead) writes <stem>.bufs.ts, <stem>.bufs.cs and
//                                              <stem>_bufs.py: the same messages for TypeScript, C# and Python
//   bufsc ... --depfile FILE SCHEMA.bufs            also writes a Makefile-style list of
//                                              every schema read, for the build
//   bufsc --check OLD.bufs NEW.bufs [-I DIR]...     exits 1 if NEW cannot read OLD's
//                                              messages or the other way round
//   bufsc --layout SCHEMA.bufs [-I DIR]...         prints every table's wire layout

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "generators.hpp"
#include "schema.hpp"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: bufsc [-o DIR] [-I DIR]... [--ts] [--cs] [--py] [--no-cpp] [--runtime MODULE] [--depfile FILE] SCHEMA.bufs...\n"
                 "       bufsc --check OLD.bufs NEW.bufs [-I DIR]...\n"
                 "       bufsc --layout SCHEMA.bufs [-I DIR]...\n");
}

void printLayout(const bufsc::Schema& schema) {
    for (const auto& s : schema.structs)
        std::printf("struct %s: %u bytes, align %u, typeId 0x%08x\n", s->qualified().c_str(), s->size, s->align,
                    bufsc::typeIdOf(s->qualified()));
    for (const auto& t : schema.tables) {
        std::printf("table %s: data %u bytes, %u refs, typeId 0x%08x\n", t->qualified().c_str(), t->dataBytes,
                    t->refCount, bufsc::typeIdOf(t->qualified()));
        for (const bufsc::Field& f : t->fields) {
            if (f.refIndex >= 0)
                std::printf("  %-28s ref %d%s\n", f.name.c_str(), f.refIndex, f.deprecated ? " (deprecated)" : "");
            else if (f.type.base == bufsc::Base::Bool)
                std::printf("  %-28s bit %u%s\n", f.name.c_str(), f.bitOffset, f.deprecated ? " (deprecated)" : "");
            else
                std::printf("  %-28s byte %u%s\n", f.name.c_str(), f.bitOffset / 8,
                            f.deprecated ? " (deprecated)" : "");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string outDir = ".";
    std::string depfile;
    std::vector<std::string> includeDirs;
    std::vector<std::string> files;
    bool check = false;
    bool layout = false;
    bool ts = false, cs = false, py = false, noCpp = false;
    std::string runtime;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (arg == "--depfile" && i + 1 < argc) {
            depfile = argv[++i];
        } else if (arg == "-I" && i + 1 < argc) {
            includeDirs.push_back(argv[++i]);
        } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            includeDirs.push_back(arg.substr(2));
        } else if (arg == "--check") {
            check = true;
        } else if (arg == "--layout") {
            layout = true;
        } else if (arg == "--ts") {
            ts = true;
        } else if (arg == "--cs") {
            cs = true;
        } else if (arg == "--py") {
            py = true;
        } else if (arg == "--no-cpp") {
            noCpp = true;
        } else if ((arg == "--runtime" || arg == "--ts-runtime") && i + 1 < argc) {
            runtime = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "bufsc: unknown option %s\n", arg.c_str());
            usage();
            return 2;
        } else {
            files.push_back(arg);
        }
    }
    if (files.empty() || (check && files.size() != 2) || (layout && files.size() != 1) ||
        (!depfile.empty() && files.size() != 1)) {
        usage();
        return 2;
    }
    try {
        if (check) {
            const bufsc::Schema older = bufsc::load(files[0], includeDirs);
            const bufsc::Schema newer = bufsc::load(files[1], includeDirs);
            const bool forward = bufsc::checkCompatible(older, newer);
            if (forward)
                std::printf("compatible\n");
            return forward ? 0 : 1;
        }
        if (layout) {
            printLayout(bufsc::load(files[0], includeDirs));
            return 0;
        }
        for (const std::string& file : files) {
            const bufsc::Schema schema = bufsc::load(file, includeDirs);
            std::string output = file;
            if (!noCpp)
                output = bufsc::generateCpp(schema, outDir);
            bufsc::GenOptions options;
            options.outDir = outDir;
            options.runtime = runtime;
            if (ts)
                bufsc::generateTypeScript(schema, options);
            if (cs)
                bufsc::generateCSharp(schema, options);
            if (py)
                bufsc::generatePython(schema, options);
            if (!depfile.empty()) {
                std::FILE* out = std::fopen(depfile.c_str(), "w");
                if (!out) {
                    std::fprintf(stderr, "bufsc: cannot write %s\n", depfile.c_str());
                    return 1;
                }
                std::fprintf(out, "%s:", output.c_str());
                for (const std::string& read : schema.files)
                    std::fprintf(out, " %s", read.c_str());
                std::fprintf(out, "\n");
                std::fclose(out);
            }
        }
    } catch (const bufsc::SchemaError& error) {
        std::fprintf(stderr, "bufsc: %s\n", error.what());
        return 1;
    }
    return 0;
}
