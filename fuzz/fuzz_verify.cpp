// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The verifier is what stands between an untrusted message and the generated accessors: whatever it accepts,
// every accessor must then stay inside the message. This target feeds arbitrary bytes to verify<T>() and, for
// what passes, reads every field kind (scalars, strings, vectors of every element kind, nested and chained tables)
// so a sanitizer reports any access outside the buffer.

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "bufs/bufs.hpp"
#include "everything.bufs.hpp"

namespace ev = test::everything;

namespace {
volatile uint64_t g_sink;

template <class Vec>
void touch(const Vec& vec) {
    for (size_t i = 0; i < vec.size(); ++i)
        g_sink += static_cast<uint64_t>(vec[i] != vec[i] ? 1 : 2);
}

void touchChild(ev::Child::Reader child) {
    for (int depth = 0; depth < 70 && child.hasName(); ++depth) {  // `next` chains forward only
        g_sink += child.name().size();
        g_sink += static_cast<uint64_t>(child.score());
        child = child.next();
    }
}

void touchEverything(ev::Everything::Reader e) {
    g_sink += e.name().size() + e.emptyName().size() + e.u64() + static_cast<uint64_t>(e.f64());
    touch(e.floats());
    touch(e.doubles());
    touch(e.longs());
    for (size_t i = 0; i < e.bools().size(); ++i)
        g_sink += e.bools()[i];
    for (auto color : e.colors())
        g_sink += static_cast<uint64_t>(color);
    for (const auto& p : e.points())
        g_sink += static_cast<uint64_t>(p.x + p.y);
    for (const auto& m : e.mixeds())
        g_sink += m.flag + static_cast<uint64_t>(m.value);
    for (std::string_view name : e.names())
        g_sink += name.size() ? static_cast<unsigned char>(name[name.size() - 1]) : 0;
    for (ev::Child::Reader child : e.children())
        touchChild(child);
    touchChild(e.child());
    for (size_t i = 0; i < e.bytes().size(); ++i)
        g_sink += e.bytes()[i];
    g_sink += e.embedding().values[7] > 0 ? 1 : 0;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Exact-size, 8-aligned: reading one byte past the input is reported by AddressSanitizer.
    uint8_t* message = new (std::align_val_t(8)) uint8_t[size ? size : 1];
    if (size)
        std::memcpy(message, data, size);
    bufs::VerifyOptions options;
    options.maxObjects = 1u << 14;  // keeps a hostile DAG from making one input slow
    if (bufs::verify<ev::Everything>(message, size, options))
        touchEverything(bufs::root<ev::Everything>(message));
    if (bufs::verify<ev::Child>(message, size, options))
        touchChild(bufs::root<ev::Child>(message));
    ::operator delete[](message, std::align_val_t(8));
    return 0;
}

std::vector<std::string> fuzzSeeds() {
    std::vector<std::string> seeds;
    {
        bufs::MessageBuilder mb;
        auto e = mb.initRoot<ev::Everything>();
        e.setName("seed");
        e.setU64(42);
        auto floats = e.initFloats(3);
        for (uint32_t i = 0; i < 3; ++i)
            floats.set(i, 1.5f * i);
        auto names = e.initNames(2);
        names.set(0, "a");
        names.set(1, "bc");
        auto children = e.initChildren(2);
        children.init(0).setName("one");
        auto second = children.init(1);
        second.setName("two");
        second.initNext().setName("three");
        e.initBytes(5);
        auto message = mb.release();
        seeds.emplace_back(reinterpret_cast<const char*>(message.data()), message.size());
    }
    {
        bufs::MessageBuilder mb;
        auto c = mb.initRoot<ev::Child>();
        c.setName("child");
        c.initNext().setName("next");
        auto message = mb.release();
        seeds.emplace_back(reinterpret_cast<const char*>(message.data()), message.size());
    }
    return seeds;
}
