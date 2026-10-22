// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libbufs round trip, defaults, fixed buffers, growth, large messages,
// struct roots, the verifier under corruption, and heap traffic.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include "everything.bufs.hpp"
#include "tricky.bufs.hpp"

namespace {

std::atomic<size_t> allocations{0};
int failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

size_t align8or(size_t n) { return n ? (n + 7) / 8 * 8 : 8; }

namespace ev = test::everything;
namespace common = test::common;

void fill(ev::Everything::Builder e) {
    e.setB1(true);
    e.setI8(-7);
    e.setU8(7);
    e.setB2(false);
    e.setI16(-12345);
    e.setU16(54321);
    e.setI32(-7654321);
    e.setU32(3000000000u);
    e.setI64(-123456789012345);
    e.setU64(17000000000000000000ull);
    e.setF32(3.25f);
    e.setF64(-6.5);
    e.setNanDefault(0.0f);
    e.setColor(ev::Color::Green);
    e.setBig(ev::Big::Small);
    ev::Mixed mixed;
    mixed.flag = 9;
    mixed.value = 1.0 / 3.0;
    mixed.small = -2;
    mixed.color = ev::Color::Red;
    mixed.at = ev::Point{1.5f, -2.5f};
    e.setMixed(mixed);
    e.setPoint(ev::Point{10.0f, 20.0f});
    ev::Embedding embedding;
    for (size_t i = 0; i < embedding.values.size(); ++i)
        embedding.values[i] = static_cast<float>(i) * 0.25f;
    embedding.code = {1, 2, 3};
    e.setEmbedding(embedding);
    e.setName("ABC123");
    e.setEmptyName("");
    const float floats[] = {1.0f, 2.0f, 3.0f};
    e.setFloats(floats, 3);
    auto doubles = e.initDoubles(2);
    doubles.set(0, 0.5);
    doubles.set(1, -0.5);
    const int64_t longs[] = {-1, 0, 1, INT64_MAX};
    e.setLongs(longs, 4);
    const bool bools[] = {true, false, true};
    e.setBools(bools, 3);
    const ev::Color colors[] = {ev::Color::Blue, ev::Color::Red};
    e.setColors(colors, 2);
    const ev::Point points[] = {{1, 2}, {3, 4}, {5, 6}};
    e.setPoints(points, 3);
    auto mixeds = e.initMixeds(2);
    mixeds.set(0, mixed);
    mixed.value = 2.0;
    mixeds.set(1, mixed);
    auto names = e.initNames(3);
    names.set(0, "first");
    names.set(2, "third"); // element 1 stays null: reads as ""
    auto children = e.initChildren(2);
    for (int i = 0; i < 2; ++i) {
        ev::Child::Builder c = children.init(static_cast<size_t>(i));
        c.setName(i == 0 ? "zero" : "one");
        c.setScore(static_cast<float>(i));
        c.initNext().setName("grandchild");
    }
    e.initChild().setScore(9.0f);
    std::vector<uint8_t> bytes(1000);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(i * 7);
    e.setBytes(bytes.data(), bytes.size());
    e.setB3(true);
    common::Shared::Builder shared = e.initShared();
    shared.setId(42);
    shared.setLabel("shared");
    e.initShareds(1).init(0).setId(43);
}

void checkFilled(ev::Everything::Reader e) {
    CHECK(e.b1());
    CHECK(e.i8() == -7);
    CHECK(e.u8() == 7);
    CHECK(!e.b2());
    CHECK(e.i16() == -12345);
    CHECK(e.u16() == 54321);
    CHECK(e.i32() == -7654321);
    CHECK(e.u32() == 3000000000u);
    CHECK(e.i64() == -123456789012345);
    CHECK(e.u64() == 17000000000000000000ull);
    CHECK(e.f32() == 3.25f);
    CHECK(e.f64() == -6.5);
    CHECK(e.nanDefault() == 0.0f);
    CHECK(e.color() == ev::Color::Green);
    CHECK(e.big() == ev::Big::Small);
    CHECK(e.mixed().flag == 9 && e.mixed().value == 1.0 / 3.0 && e.mixed().small == -2);
    CHECK(e.mixed().color == ev::Color::Red && e.mixed().at.x == 1.5f && e.mixed().at.y == -2.5f);
    CHECK(e.point().x == 10.0f && e.point().y == 20.0f);
    CHECK(e.embedding().values[3] == 0.75f && e.embedding().code[2] == 3);
    CHECK(e.name() == "ABC123" && e.hasName());
    CHECK(e.name().data()[e.name().size()] == '\0');
    CHECK(e.emptyName().empty() && e.hasEmptyName());
    CHECK(e.floats().size() == 3 && e.floats()[2] == 3.0f);
    float sum = 0;
    for (float f : e.floats())
        sum += f;
    CHECK(sum == 6.0f);
    CHECK(e.doubles().size() == 2 && e.doubles()[1] == -0.5);
    CHECK(reinterpret_cast<uintptr_t>(e.doubles().data()) % 8 == 0);
    CHECK(e.longs().size() == 4 && e.longs()[3] == INT64_MAX);
    CHECK(reinterpret_cast<uintptr_t>(e.longs().data()) % 8 == 0);
    CHECK(e.bools().size() == 3 && e.bools()[0] && !e.bools()[1] && e.bools()[2]);
    CHECK(e.colors().size() == 2 && e.colors()[0] == ev::Color::Blue);
    CHECK(e.points().size() == 3 && e.points()[2].y == 6.0f);
    CHECK(e.mixeds().size() == 2 && e.mixeds()[1].value == 2.0);
    CHECK(reinterpret_cast<uintptr_t>(e.mixeds().data()) % 8 == 0);
    CHECK(e.names().size() == 3 && e.names()[0] == "first" && e.names()[1].empty() && e.names()[2] == "third");
    int named = 0;
    for (std::string_view n : e.names())
        named += n.empty() ? 0 : 1;
    CHECK(named == 2);
    CHECK(e.children().size() == 2);
    CHECK(e.children()[1].name() == "one" && e.children()[1].score() == 1.0f);
    CHECK(e.children()[0].next().name() == "grandchild" && !e.children()[0].next().hasNext());
    int kids = 0;
    for (ev::Child::Reader c : e.children())
        kids += c ? 1 : 0;
    CHECK(kids == 2);
    CHECK(e.child().score() == 9.0f && !e.child().hasName());
    CHECK(e.bytes().size() == 1000 && e.bytes()[999] == static_cast<uint8_t>(999 * 7));
    CHECK(e.b3());
    CHECK(e.shared().id() == 42 && e.shared().label() == "shared");
    CHECK(e.shareds().size() == 1 && e.shareds()[0].id() == 43 && e.shareds()[0].label().empty());
}

void roundTrip() {
    bufs::MessageBuilder mb;
    fill(mb.initRoot<ev::Everything>());
    const bufs::Message message = mb.release();
    CHECK(message.size() % 8 == 0);
    CHECK(message.typeId() == ev::Everything::typeId);
    CHECK(bufs::typeOf(message.data(), message.size()) == ev::Everything::typeId);
    CHECK(bufs::verify<ev::Everything>(message.data(), message.size()));
    CHECK(!bufs::verify<ev::Child>(message.data(), message.size()));
    checkFilled(bufs::root<ev::Everything>(message.data()));
    std::printf("  everything: %zu bytes\n", message.size());
}

void defaults() {
    bufs::MessageBuilder mb;
    mb.initRoot<ev::Everything>();
    const auto [data, size] = mb.finish();
    CHECK(bufs::verify<ev::Everything>(data, size));
    const ev::Everything::Reader e = bufs::root<ev::Everything>(data);
    CHECK(!e.b1() && e.b2() && !e.b3());
    CHECK(e.i8() == -3 && e.u8() == 200 && e.i16() == -1000 && e.u16() == 60000);
    CHECK(e.i32() == -100000 && e.u32() == 4000000000u && e.i64() == -5000000000);
    CHECK(e.u64() == 18000000000000000000ull);
    CHECK(e.f32() == 1.5f && e.f64() == -2.25 && std::isnan(e.nanDefault()));
    CHECK(e.color() == ev::Color::Blue && e.big() == ev::Big::Large);
    CHECK(e.point().x == 0 && e.mixed().value == 0);
    CHECK(e.name().empty() && !e.hasName() && e.floats().empty() && !e.floats() && !e.child());
    CHECK(e.child().score() == 0.5f); // a null child reads as defaults

    // A table written by an older schema: only the 4-byte header, no refs.
    alignas(8) uint8_t old[16] = {};
    bufs::store<uint32_t>(old, 16);
    bufs::store<uint32_t>(old + 4, ev::Everything::typeId);
    bufs::store<uint16_t>(old + 8, 4);
    CHECK(bufs::verify<ev::Everything>(old, sizeof old));
    const ev::Everything::Reader short_ = bufs::root<ev::Everything>(old);
    CHECK(short_.u8() == 200 && short_.b2() && short_.color() == ev::Color::Blue && short_.names().empty());
    CHECK(short_.embedding().values[0] == 0.0f);

    // Setting a default stores zeros: the default costs nothing to send.
    bufs::MessageBuilder again;
    ev::Everything::Builder b = again.initRoot<ev::Everything>();
    b.setU8(200);
    b.setColor(ev::Color::Blue);
    b.setB2(true);
    const auto [data2, size2] = again.finish();
    CHECK(size2 == size && std::memcmp(data, data2, size) == 0);
}

void fixedBuffer() {
    alignas(8) uint8_t memory[4096];
    bufs::MessageBuilder mb(memory, sizeof memory);
    fill(mb.initRoot<ev::Everything>());
    const auto [data, size] = mb.finish();
    CHECK(data == memory);
    CHECK(bufs::verify<ev::Everything>(data, size));
    checkFilled(bufs::root<ev::Everything>(data));

    alignas(8) uint8_t tiny[256];
    bufs::MessageBuilder small(tiny, sizeof tiny);
    bool threw = false;
    try {
        fill(small.initRoot<ev::Everything>());
    } catch (const bufs::CapacityError&) {
        threw = true;
    }
    CHECK(threw);
}

void growth() {
    bufs::BufferPool pool;
    bufs::MessageBuilder mb(pool, 64);
    fill(mb.initRoot<ev::Everything>());
    const bufs::Message message = mb.release();
    CHECK(bufs::verify<ev::Everything>(message.data(), message.size()));
    checkFilled(bufs::root<ev::Everything>(message.data()));
}

void largeMessage() {
    const size_t big = size_t{64} << 20;
    std::vector<uint8_t> payload(big);
    for (size_t i = 0; i < big; i += 4096)
        payload[i] = static_cast<uint8_t>(i >> 12);
    bufs::BufferPool pool;
    {
        bufs::MessageBuilder mb(pool);
        ev::Everything::Builder e = mb.initRoot<ev::Everything>();
        e.setName("large");
        e.setBytes(payload.data(), payload.size());
        const bufs::Message message = mb.release();
        CHECK(message.size() > big);
        CHECK(bufs::verify<ev::Everything>(message.data(), message.size()));
        const ev::Everything::Reader r = bufs::root<ev::Everything>(message.data());
        CHECK(r.bytes().size() == big && r.bytes()[big - 4096] == static_cast<uint8_t>((big - 4096) >> 12));
        CHECK(r.name() == "large");
        std::printf("  large: %zu MiB message\n", message.size() >> 20);
    }
    CHECK(pool.stats().blocksInUse == 0);
}

void structRoot() {
    bufs::MessageBuilder mb;
    ev::Mixed& m = mb.initRootStruct<ev::Mixed>();
    m.value = 4.5;
    m.at.y = 1.0f;
    const auto [data, size] = mb.finish();
    CHECK(size == bufs::kHeaderBytes + sizeof(ev::Mixed));
    CHECK(bufs::verifyStruct<ev::Mixed>(data, size));
    CHECK(!bufs::verifyStruct<ev::Point>(data, size));
    CHECK(bufs::rootStruct<ev::Mixed>(data).value == 4.5 && bufs::rootStruct<ev::Mixed>(data).at.y == 1.0f);
}

// Every truncation and many random corruptions: the verifier either rejects
// the message or every accessor stays in bounds (AddressSanitizer checks the
// latter in the sanitizer build).
void corruption() {
    bufs::MessageBuilder mb;
    fill(mb.initRoot<ev::Everything>());
    const auto [data, size] = mb.finish();
    std::vector<uint8_t> original(data, data + size);

    size_t rejected = 0, accepted = 0;
    for (size_t cut = 0; cut < size; cut += 8) {
        void* copy = std::aligned_alloc(8, align8or(cut));
        std::memcpy(copy, original.data(), cut);
        if (bufs::verify<ev::Everything>(copy, cut))
            ++accepted;
        else
            ++rejected;
        std::free(copy);
    }
    CHECK(accepted == 0);

    std::mt19937 random(1);
    std::vector<uint8_t> work(size);
    accepted = rejected = 0;
    for (int round = 0; round < 20000; ++round) {
        uint8_t* copy = static_cast<uint8_t*>(std::aligned_alloc(8, size));
        std::memcpy(copy, original.data(), size);
        const int flips = 1 + static_cast<int>(random() % 4);
        for (int f = 0; f < flips; ++f)
            copy[random() % size] ^= static_cast<uint8_t>(1u << (random() % 8));
        if (bufs::verify<ev::Everything>(copy, size)) {
            ++accepted;
            const ev::Everything::Reader e = bufs::root<ev::Everything>(copy);
            size_t touched = e.name().size() + e.floats().size() + e.names().size() + e.bytes().size();
            for (std::string_view n : e.names())
                touched += n.size();
            for (ev::Child::Reader c : e.children())
                touched += c.name().size() + c.next().name().size();
            touched += e.child().name().size() + e.shared().label().size();
            for (common::Shared::Reader s : e.shareds())
                touched += s.label().size();
            (void)touched;
        } else {
            ++rejected;
        }
        std::free(copy);
    }
    std::printf("  corruption: %zu rejected, %zu accepted and read safely\n", rejected, accepted);
}

// Names that are C++ keywords, a struct declared after its user, struct
// bools stored as bytes, a table shadowing an outer enum, exact float defaults.
void trickyNames() {
    bufs::MessageBuilder mb;
    tricky::Keywords::Builder k = mb.initRoot<tricky::Keywords>();
    k.setTable(1);
    k.setNew(2);
    k.setDecltype(3.5f);
    k.setClass("class");
    k.setOp(tricky::Op::new_);
    k.initKind().setX(7);
    tricky::Outer outer;
    outer.inner.x = 5;
    outer.inner.ready = 1;
    outer.flag = 0x40; // any byte is a defined value; non-zero means true
    k.setOuter(outer);
    const auto [data, size] = mb.finish();
    CHECK(bufs::verify<tricky::Keywords>(data, size));
    const tricky::Keywords::Reader r = bufs::root<tricky::Keywords>(data);
    CHECK(r.table() == 1 && r.new_() == 2 && r.decltype_() == 3.5f && r.class_() == "class");
    CHECK(r.op() == tricky::Op::new_ && std::string(tricky::enumName(r.op())) == "new");
    CHECK(r.kind().x() == 7);
    CHECK(r.outer().inner.x == 5 && r.outer().inner.ready && r.outer().flag != 0);
    CHECK(std::isnan(r.notANumber()) && std::isinf(r.infinite()) && r.infinite() < 0);
    CHECK(r.tiny() == 1e-9 && r.third() == 0.333333333f && r.hex() == 0xFFFFFFFFu);

    bufs::MessageBuilder empty;
    empty.initRoot<tricky::Keywords>();
    const auto [d2, s2] = empty.finish();
    const tricky::Keywords::Reader e = bufs::root<tricky::Keywords>(d2);
    CHECK(e.op() == tricky::Op::delete_ && e.tiny() == 1e-9 && std::isnan(e.notANumber()));
}

// Many tables sharing one vector of null references: the verifier charges
// every slot, so it gives up early instead of walking K x C slots.
void verifierBudget() {
    constexpr size_t kTables = 200000, kSlots = 200000;
    bufs::BufferPool pool;
    bufs::MessageBuilder mb(pool);
    const tricky::Node::Builder root = mb.initRoot<tricky::Node>();
    const uint32_t kids = mb.allocateVector(kTables, 4, 4);
    mb.setRef(bufs::MessageBuilder::refSlot(root.bufsOffset(), tricky::Node::kDataBytes, 0), kids);
    const uint32_t shared = mb.allocateTable(tricky::Node::kDataBytes, tricky::Node::kRefCount);
    const uint32_t nulls = mb.allocateVector(kSlots, 4, 4);
    mb.setRef(bufs::MessageBuilder::refSlot(shared, tricky::Node::kDataBytes, 0), nulls);
    for (size_t i = 0; i < kTables; ++i)
        mb.setRef(kids + 4 + static_cast<uint32_t>(4 * i), shared);
    const auto [data, size] = mb.finish();
    const auto start = std::chrono::steady_clock::now();
    const bool accepted = bufs::verify<tricky::Node>(data, size);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("  shared null vectors: %zu KB rejected in %.2f ms\n", size >> 10, ms);
    CHECK(!accepted);
    CHECK(ms < 1000);
}

// Eight threads building messages, half of them freed by another thread, with
// outliers of a few MiB: every block comes back, idle outliers keep no pages.
void poolThreads() {
    bufs::BufferPool pool;
    std::mutex mutex;
    std::deque<bufs::Message> handoff;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 5000; ++i) {
                bufs::MessageBuilder mb(pool);
                ev::Everything::Builder e = mb.initRoot<ev::Everything>();
                e.initBytes(static_cast<size_t>(100) << ((i + t) % 8));
                if (i % 1000 == 0)
                    e.initFloats(size_t{1} << 20); // 4 MiB
                bufs::Message m = mb.release();
                std::lock_guard<std::mutex> lock(mutex);
                if (i % 2)
                    handoff.push_back(std::move(m));
                else if (!handoff.empty())
                    handoff.pop_front();
            }
        });
    for (std::thread& t : threads)
        t.join();
    handoff.clear();
    const bufs::BufferPool::Stats s = pool.stats();
    std::printf("  8 threads: %zu blocks in use after, %zu MiB mapped, %zu MiB idle large\n", s.blocksInUse,
                s.mappedBytes >> 20, s.retainedBytes >> 20);
    CHECK(s.blocksInUse == 0);
    CHECK(s.retainedBytes <= (size_t{64} << 20));
}

// An idle outlier block keeps its pages while it may be reused soon and gives
// them back once it has been idle for releasePagesAfter.
size_t residentPages(const uint8_t* p, size_t bytes) {
    std::vector<unsigned char> pages(bytes / 4096);
    if (::mincore(const_cast<uint8_t*>(p), bytes, pages.data()) != 0)
        return 0;
    size_t resident = 0;
    for (unsigned char page : pages)
        resident += page & 1;
    return resident;
}

void idlePagesReleased() {
    bufs::BufferPool::Options options;
    options.releasePagesAfter = std::chrono::milliseconds(20);
    bufs::BufferPool pool(options);
    const size_t bytes = size_t{8} << 20;
    const uint8_t* address = nullptr;
    {
        bufs::Buffer block = pool.acquire(bytes);
        std::memset(block.data(), 1, block.capacity());
        address = block.data();
    }
    const size_t warm = residentPages(address, bytes);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    pool.acquire(size_t{256} << 10); // large-block traffic runs the idle check
    const size_t cold = residentPages(address, bytes);
    std::printf("  idle 8 MiB block: %zu resident pages while warm, %zu after idling\n", warm, cold);
    CHECK(warm == bytes / 4096);
    CHECK(cold <= 1);
    CHECK(pool.stats().retainedBytes >= bytes); // still mapped for reuse
}

// After a warm-up, building, verifying and reading allocate nothing.
void noHeapAfterWarmUp() {
    bufs::BufferPool pool;
    auto once = [&] {
        bufs::MessageBuilder mb(pool);
        fill(mb.initRoot<ev::Everything>());
        const bufs::Message m = mb.release();
        if (bufs::verify<ev::Everything>(m.data(), m.size()))
            checkFilled(bufs::root<ev::Everything>(m.data()));
    };
    for (int i = 0; i < 10; ++i)
        once();
    const uint64_t maps = pool.stats().mapCalls;
    const size_t before = allocations.load();
    for (int i = 0; i < 10000; ++i)
        once();
    const size_t heap = allocations.load() - before;
    std::printf("  10000 build+verify+read: %zu heap allocations, %llu new mappings\n", heap,
                static_cast<unsigned long long>(pool.stats().mapCalls - maps));
    // fill() itself makes one std::vector per call; the library none.
    CHECK(heap == 10000);
    CHECK(pool.stats().mapCalls == maps);
}


} // namespace

void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
    roundTrip();
    defaults();
    fixedBuffer();
    growth();
    largeMessage();
    structRoot();
    corruption();
    trickyNames();
    verifierBudget();
    poolThreads();
    idlePagesReleased();
    noHeapAfterWarmUp();
    if (failures == 0)
        std::puts("libbufs: round trip, defaults, fixed, growth, large, struct root, verifier and heap passed");
    return failures == 0 ? 0 : 1;
}
