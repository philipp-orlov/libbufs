// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The shared-memory transport: a producer process builds messages straight
// into ring slots and a consumer process reads them in place, across wrap-
// arounds, with futex waits on both sides. Also: a full ring, a message too
// large for the ring, and a corrupted ring.

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "bufs/shm.hpp"
#include "ring.bufs.hpp"

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using test::ring::Packet;
using test::ring::Stop;
using namespace std::chrono_literals;

constexpr uint64_t kMessages = 200000;

std::string uniqueName(const char* what) { return "/bufs-test-" + std::string(what) + "-" + std::to_string(::getpid()); }

size_t payloadBytes(uint64_t seq) { return (seq * 37) % 3000; }

// Child: opens the channel and sends kMessages packets, then a Stop; waits
// for room when the ring is full. Exits non-zero on any error.
int producer(const std::string& name) {
    try {
        bufs::shm::Channel channel = bufs::shm::Channel::open(name);
        bufs::shm::Ring& ring = channel.send();
        for (uint64_t seq = 0; seq < kMessages; ++seq) {
            const size_t want = 64 + payloadBytes(seq) + 32;
            uint8_t* slot = ring.reserve(want, -1ns);
            bufs::MessageBuilder mb(slot, want);
            Packet::Builder p = mb.initRoot<Packet>();
            p.setSeq(seq);
            bufs::ListBuilder<uint8_t> payload = p.initPayload(payloadBytes(seq));
            for (size_t i = 0; i < payload.size(); ++i)
                payload.data()[i] = static_cast<uint8_t>(seq + i);
            if (seq % 1000 == 0)
                p.setNote("checkpoint");
            ring.commit(mb.finish().second);
        }
        uint8_t* slot = ring.reserve(64, -1ns);
        bufs::MessageBuilder mb(slot, 64);
        mb.initRoot<Stop>().setCount(kMessages);
        ring.commit(mb.finish().second);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "producer: %s\n", e.what());
        return 1;
    }
}

void acrossProcesses() {
    const std::string name = uniqueName("channel");
    bufs::shm::Channel channel = bufs::shm::Channel::create(name, 64 << 10);
    const pid_t child = ::fork();
    if (child == 0)
        ::_exit(producer(name));

    bufs::shm::Ring& ring = channel.receive();
    uint64_t expected = 0;
    uint64_t bytes = 0;
    bool stopped = false;
    const auto start = std::chrono::steady_clock::now();
    while (!stopped) {
        if (!ring.wait(5s)) {
            CHECK(!"producer stalled");
            break;
        }
        const uint8_t* data = nullptr;
        size_t size = 0;
        while (ring.front(data, size)) {
            bytes += size;
            const uint32_t type = bufs::typeOf(data, size);
            if (type == Stop::typeId) {
                CHECK(bufs::verify<Stop>(data, size));
                CHECK(bufs::root<Stop>(data).count() == expected);
                stopped = true;
            } else if (type == Packet::typeId && bufs::verify<Packet>(data, size)) {
                const Packet::Reader p = bufs::root<Packet>(data);
                if (p.seq() != expected || p.payload().size() != payloadBytes(expected) ||
                    (p.payload().size() && p.payload()[p.payload().size() - 1] !=
                                               static_cast<uint8_t>(expected + p.payload().size() - 1)) ||
                    (expected % 1000 == 0) != (p.note() == "checkpoint")) {
                    CHECK(!"packet content");
                    stopped = true;
                }
                ++expected;
            } else {
                CHECK(!"unexpected message");
                stopped = true;
            }
            ring.pop();
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(expected == kMessages);
    std::printf("  %llu messages (%.1f MB) across processes in %.3f s: %.2f M msg/s\n",
                static_cast<unsigned long long>(expected), bytes / 1e6, seconds, expected / seconds / 1e6);
}

void fullAndOversize() {
    alignas(64) static uint8_t memory[bufs::shm::Ring::bytesFor(1024)];
    bufs::shm::Ring ring = bufs::shm::Ring::initialize(memory, 1024);
    CHECK(ring.maxMessageBytes() == 504);
    int pushed = 0;
    const uint8_t block[100] = {};
    while (ring.push(block, sizeof block))
        ++pushed;
    CHECK(pushed == 9); // 9 x 112-byte records in 1024
    CHECK(!ring.reserve(100, 1ms));
    bool threw = false;
    try {
        ring.reserve(505);
    } catch (const bufs::CapacityError&) {
        threw = true;
    }
    CHECK(threw);
    const uint8_t* data = nullptr;
    size_t size = 0;
    for (int i = 0; i < pushed; ++i) {
        CHECK(ring.front(data, size) && size == 100);
        ring.pop();
    }
    CHECK(!ring.front(data, size) && ring.empty());
    CHECK(!ring.wait(1ms));
}

void corrupted() {
    alignas(64) static uint8_t memory[bufs::shm::Ring::bytesFor(1024)];
    bufs::shm::Ring producer = bufs::shm::Ring::initialize(memory, 1024);
    bufs::shm::Ring consumer = bufs::shm::Ring::attach(memory, sizeof memory);
    const uint8_t block[16] = {};
    producer.push(block, sizeof block);
    uint8_t* data = memory + 256;
    bufs::store<uint32_t>(data, 5000); // a length past the ring
    bool threw = false;
    const uint8_t* front = nullptr;
    size_t size = 0;
    try {
        consumer.front(front, size);
    } catch (const bufs::shm::CorruptRing&) {
        threw = true;
    }
    CHECK(threw);

    bufs::store<uint64_t>(memory + 64, 1u << 20); // writePos far ahead of readPos
    threw = false;
    try {
        consumer.front(front, size);
    } catch (const bufs::shm::CorruptRing&) {
        threw = true;
    }
    CHECK(threw);

    // Equal misalignment: the difference between the positions is still a
    // multiple of 8, but each position on its own is not.
    bufs::store<uint64_t>(memory + 64, 263); // writePos
    bufs::store<uint64_t>(memory + 128, 255); // readPos
    threw = false;
    try {
        consumer.front(front, size);
    } catch (const bufs::shm::CorruptRing&) {
        threw = true;
    }
    CHECK(threw);

    bufs::store<uint64_t>(memory + 64, 255); // writePos, misaligned on its own
    bufs::store<uint64_t>(memory + 128, 64); // readPos
    threw = false;
    try {
        producer.reserve(16);
    } catch (const bufs::shm::CorruptRing&) {
        threw = true;
    }
    CHECK(threw);

    bufs::store<uint64_t>(memory + 8, 3000); // not a power of two
    threw = false;
    try {
        bufs::shm::Ring::attach(memory, sizeof memory);
    } catch (const bufs::shm::CorruptRing&) {
        threw = true;
    }
    CHECK(threw);
}

// commit() without a pending reserve() must throw, not rewind the ring.
void commitWithoutReserve() {
    alignas(64) static uint8_t memory[bufs::shm::Ring::bytesFor(256)];
    bufs::shm::Ring ring = bufs::shm::Ring::initialize(memory, 256);
    CHECK(ring.push("hello", 5));
    bool threw = false;
    try {
        ring.commit(0);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);

    CHECK(ring.reserve(8) != nullptr);
    ring.cancel();
    threw = false;
    try {
        ring.commit(0);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);

    const uint8_t* data = nullptr;
    size_t size = 0;
    CHECK(ring.front(data, size) && size == 5 && std::memcmp(data, "hello", 5) == 0);
    ring.pop();
    CHECK(!ring.front(data, size) && ring.empty());
}

void regionLifetime() {
    const std::string name = uniqueName("region");
    {
        bufs::shm::Region owner = bufs::shm::Region::create(name, 4096);
        owner.data()[0] = 42;
        bufs::shm::Region peer = bufs::shm::Region::open(name);
        CHECK(peer.size() == 4096 && peer.data()[0] == 42 && !peer.owner());
        bool threw = false;
        try {
            bufs::shm::Region::create(name, 4096);
        } catch (const std::system_error&) {
            threw = true;
        }
        CHECK(threw);
    }
    bool gone = false;
    try {
        bufs::shm::Region::open(name);
    } catch (const std::system_error&) {
        gone = true;
    }
    CHECK(gone);
}

} // namespace

int main() {
    regionLifetime();
    fullAndOversize();
    corrupted();
    commitWithoutReserve();
    acrossProcesses();
    if (failures == 0)
        std::puts("libbufs shm: region, full ring, corruption and cross-process channel passed");
    return failures == 0 ? 0 : 1;
}
