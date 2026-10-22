// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Built twice: against evolution_v1.bufs (BUFS_EVOLUTION_VERSION 1) and
// evolution_v2.bufs (2). Each writes a message with its own schema and reads
// the other's, so old readers read new messages and new readers old ones.
//
//   bufs_test_evolution_vN write FILE
//   bufs_test_evolution_vN read FILE

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if BUFS_EVOLUTION_VERSION == 1
#include "evolution_v1.bufs.hpp"
#else
#include "evolution_v2.bufs.hpp"
#endif

using test::evolution::Kind;
using test::evolution::Person;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

int write(const char* path) {
    bufs::MessageBuilder mb;
    Person::Builder p = mb.initRoot<Person>();
    p.setName("Ada");
    p.setAge(41);
#if BUFS_EVOLUTION_VERSION == 1
    p.setKind(Kind::B);
    p.initTags(1).set(0, "first");
#else
    p.setKind(Kind::C);
    p.setHeight(1.6f);
    p.setVip(true);
    auto pets = p.initPets(2);
    pets.init(0).setName("Rex");
    pets.init(1).setLegs(3);
    p.setNickname("Countess");
#endif
    const auto [data, size] = mb.finish();
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out ? 0 : 1;
}

int read(const char* path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    alignas(8) static uint8_t message[4096];
    if (bytes.empty() || bytes.size() > sizeof message) {
        std::printf("FAILED: cannot read %s\n", path);
        return 1;
    }
    std::memcpy(message, bytes.data(), bytes.size());
    CHECK(bufs::verify<Person>(message, bytes.size()));
    const Person::Reader p = bufs::root<Person>(message);
    CHECK(p.name() == "Ada");
    CHECK(p.age() == 41);
#if BUFS_EVOLUTION_VERSION == 1
    // Written by v2: kind C is unknown here but survives as its number; the
    // deprecated tags are absent; v2's new fields are skipped.
    CHECK(static_cast<int>(p.kind()) == 2);
    CHECK(p.tags().empty());
#else
    // Written by v1: the fields v1 did not have read as their defaults.
    CHECK(p.kind() == Kind::B);
    CHECK(p.height() == 1.75f);
    CHECK(!p.vip());
    CHECK(p.pets().empty() && !p.hasNickname());
#endif
    if (failures == 0)
        std::printf("v%d read the other version's message\n", BUFS_EVOLUTION_VERSION);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "write") == 0)
        return write(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "read") == 0)
        return read(argv[2]);
    std::fprintf(stderr, "usage: %s write|read FILE\n", argv[0]);
    return 2;
}
