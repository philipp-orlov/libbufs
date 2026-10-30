// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Shared by every benchmark translation unit: the logical messages (the same
// content in every format), the codec interface each format implements, the
// heap and RSS probes, and the long-run and multi-threaded harnesses.
//
// Each format lives in its own translation unit because bufsc and flatc both
// generate `namespace bench` from the same bench.bufs. Timed loops run inside
// those units, so the only indirect call is one per batch.

#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace bd {

// ------------------------------------------------------------ the messages --

struct BoxData {
    float x, y, w, h;
};

struct ItemData {
    std::string label, category, subcategory;
    float score = 0, categoryScore = 0, subcategoryScore = 0, itemScore = 0, exactScore = 0, logProb = 0;
    bool gapped = false, provided = false;
    uint8_t verdict = 0; // 0 accept, 1 partial, 2 reject
    int32_t rows = 1;
    uint32_t flags = 0;
    int32_t splitAt = 0;
    std::vector<float> partScores;
    std::vector<BoxData> partBoxes;
    std::vector<uint8_t> partRows;
    std::vector<float> features;
    BoxData bounds{};
    // Not owned: points into blobBytes() so a workload can vary it for free.
    const uint8_t* blob = nullptr;
    size_t blobSize = 0;
};

struct ReportData {
    uint64_t queryId = 0;
    int32_t width = 0, height = 0;
    float prepareTime = 0, processTime = 0;
    std::vector<ItemData> items; // the first itemCount are sent
    size_t itemCount = 0;
};

struct QueryData {
    uint64_t queryId = 0;
    std::string source;
    int32_t width = 0, height = 0, stride = 0;
    bool exact = false;
    float threshold = 0, minScore = 0;
    uint8_t encoding = 0;
    uint32_t slot = 0;
    int64_t timestampUs = 0;
    uint32_t clientId = 0, flags = 0;
};

// The struct-only message: every field fixed size.
struct FrameData {
    uint64_t queryId = 0;
    int64_t timestampUs = 0;
    uint32_t slot = 0;
    int32_t width = 0, height = 0, stride = 0;
    uint32_t clientId = 0;
    uint8_t encoding = 0;
    bool exact = false;
    float threshold = 0, minScore = 0;
};

struct DetectionData {
    float x1, y1, x2, y2, score;
    int32_t classId;
};

struct DetectionsData {
    uint64_t queryId = 0;
    std::vector<DetectionData> items; // the first count are sent
    size_t count = 0;
};

enum class Type : uint8_t { Query, Report, Frame, Detections };

struct Input {
    Type type = Type::Query;
    const QueryData* query = nullptr;
    const ReportData* report = nullptr;
    const FrameData* frame = nullptr;
    const DetectionsData* detections = nullptr;
};

// 4 MiB of random bytes that every item blob points into (incompressible,
// like already-compressed data).
inline const std::vector<uint8_t>& blobBytes() {
    static const std::vector<uint8_t> bytes = [] {
        std::vector<uint8_t> b(size_t{4} << 20);
        std::mt19937_64 random(42);
        for (size_t i = 0; i < b.size(); i += 8) {
            const uint64_t v = random();
            std::memcpy(b.data() + i, &v, 8);
        }
        return b;
    }();
    return bytes;
}

inline ItemData makeItem(std::mt19937& random, size_t index) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    static const char* const labels[] = {"alpha-1", "bravo-2", "delta-3", "gamma-4", "omega-5", "sigma-6"};
    static const char* const categories[] = {"A1", "B2", "C3", "D4"};
    static const char* const subcategories[] = {"lo", "mid", "hi", "ok"};
    ItemData p;
    p.label = labels[index % 6];
    if (index % 2)
        p.label += "x"; // 7 and 8 characters
    p.category = categories[index % 4];
    p.subcategory = subcategories[index % 4];
    p.score = unit(random);
    p.categoryScore = unit(random);
    p.subcategoryScore = unit(random);
    p.itemScore = unit(random);
    p.exactScore = unit(random);
    p.logProb = -unit(random) * 3;
    p.gapped = index % 3 == 0;
    p.provided = false;
    p.verdict = static_cast<uint8_t>(index % 3);
    p.rows = 1 + static_cast<int32_t>(index % 2);
    p.flags = 0x15u;
    p.splitAt = 3;
    for (int i = 0; i < 8; ++i) {
        p.partScores.push_back(0.9f + unit(random) / 10);
        p.partBoxes.push_back(BoxData{i * 0.12f, 0.1f, 0.11f, 0.8f});
        p.partRows.push_back(static_cast<uint8_t>(i < 3 ? 0 : 1));
    }
    for (int i = 0; i < 128; ++i)
        p.features.push_back(unit(random) * 2 - 1);
    p.bounds = BoxData{0.41f, 0.63f, 0.12f, 0.05f};
    p.blob = blobBytes().data() + index * 8192;
    p.blobSize = 0;
    return p;
}

// `items` items, each with a `blobSize`-byte blob (0 for none).
inline ReportData makeReport(size_t items, size_t blobSize, size_t capacity = 0) {
    std::mt19937 random(7);
    ReportData r;
    r.queryId = 0x1234567890ull;
    r.width = 1920;
    r.height = 1080;
    r.prepareTime = 1830.0f;
    r.processTime = 4210.0f;
    for (size_t i = 0; i < std::max(items, capacity); ++i) {
        r.items.push_back(makeItem(random, i));
        r.items.back().blobSize = blobSize;
    }
    r.itemCount = items;
    return r;
}

inline QueryData makeQuery() {
    QueryData r;
    r.queryId = 0x1234567890ull;
    r.source = "/bench-source-471"; // 17 characters
    r.width = 1920;
    r.height = 1080;
    r.stride = 5760;
    r.exact = false;
    r.threshold = 0.35f;
    r.minScore = 0.6f;
    r.encoding = 1;
    r.slot = 3;
    r.timestampUs = 1792636206115000;
    r.clientId = 17;
    r.flags = 0x5;
    return r;
}

inline FrameData makeFrame() {
    FrameData f;
    f.queryId = 0x1234567890ull;
    f.timestampUs = 1792636206115000;
    f.slot = 3;
    f.width = 1920;
    f.height = 1080;
    f.stride = 5760;
    f.clientId = 17;
    f.encoding = 1;
    f.exact = false;
    f.threshold = 0.35f;
    f.minScore = 0.6f;
    return f;
}

inline DetectionsData makeDetections(size_t count, size_t capacity = 0) {
    std::mt19937 random(9);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    DetectionsData d;
    d.queryId = 0x1234567890ull;
    for (size_t i = 0; i < std::max(count, capacity); ++i) {
        const float x = unit(random), y = unit(random);
        d.items.push_back(DetectionData{x, y, x + 0.1f, y + 0.2f, unit(random), static_cast<int32_t>(i % 7)});
    }
    d.count = count;
    return d;
}

// ------------------------------------------------------------- the codecs --

struct LongRunConfig {
    uint64_t cycles = 5000000;
    uint32_t inFlight = 8;       // messages kept alive between build and read
    uint32_t outlierEvery = 2000; // one 1-4 MiB message per this many, on average
    uint64_t seed = 1;
};

struct LongRunResult {
    uint64_t cycles = 0;
    double seconds = 0;
    size_t rssStart = 0, rssWarm = 0, rssMid = 0, rssEnd = 0, rssAfterDrain = 0, rssPeak = 0;
    uint64_t outliers = 0;
    uint64_t checksum = 0;
    std::string note;
};

struct MtConfig {
    uint32_t threads = 8;
    uint64_t cyclesPerThread = 500000;
    bool handoff = false; // messages are read and freed by the next thread
    uint32_t outlierEvery = 2000;
    uint32_t variant = 0; // index into Codec::mtVariants()
};

struct MtResult {
    uint64_t messages = 0;
    double seconds = 0;
    size_t rssEnd = 0, rssPeak = 0;
    uint64_t checksum = 0;
    std::string note;
};

class Codec {
public:
    virtual ~Codec() = default;
    virtual std::string name() const = 0;
    // One message's bytes.
    virtual std::vector<uint8_t> encode(const Input& in) = 0;
    // Builds `in` n times, reusing one builder.
    virtual uint64_t buildReuse(const Input& in, uint64_t n) = 0;
    // Builds `in` n times with a fresh builder each time; each finished message
    // lives until the next is built.
    virtual uint64_t buildFresh(const Input& in, uint64_t n) = 0;
    // n times; the message is 8-aligned. verify() returns how many passed.
    virtual uint64_t verify(Type type, const uint8_t* data, size_t size, uint64_t n) = 0;
    virtual uint64_t read(Type type, const uint8_t* data, size_t size, uint64_t n) = 0;
    virtual uint64_t decode(Type type, const uint8_t* data, size_t size, uint64_t n) = 0;
    // False for a row that shares another row's encoder (json/jsonlite) and
    // so has no build, long-run or multi-threaded numbers of its own.
    virtual bool ownsEncoder() const { return true; }
    virtual LongRunResult longRun(const LongRunConfig& config) = 0;
    virtual MtResult multiThread(const MtConfig& config) = 0;
    // Names of the ways multiThread() can run (MtConfig::variant).
    virtual std::vector<std::string> mtVariants() const { return {""}; }
};

std::unique_ptr<Codec> makeBufsCodec();
std::unique_ptr<Codec> makeFlatBuffersCodec(); // nullptr without FlatBuffers
std::unique_ptr<Codec> makeJsonCodec();        // nullptr without libhttp's json
std::unique_ptr<Codec> makeJsonLiteCodec();    // libhttp encode, JsonLite decode

// ---------------------------------------------------------- heap and RSS --

namespace heap {
// Counted only while counting is on (bench_alloc.cpp interposes malloc and
// friends and replaces operator new).
extern std::atomic<bool> counting;
extern std::atomic<uint64_t> allocations; // malloc/calloc/realloc/memalign calls
extern std::atomic<uint64_t> bytes;
extern std::atomic<uint64_t> newCalls; // operator new calls (also counted in allocations)
} // namespace heap

inline size_t rssBytes() {
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        std::fclose(f);
    }
    return static_cast<size_t>(resident) * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

// VmHWM: the peak resident set.
inline size_t peakRssBytes() {
    size_t kb = 0;
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "VmHWM:", 6) == 0) {
                kb = std::strtoull(line + 6, nullptr, 10);
                break;
            }
        }
        std::fclose(f);
    }
    return kb * 1024;
}

inline double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The CPUs this process may run on (taskset), in order.
inline std::vector<int> allowedCpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    std::vector<int> cpus;
    if (::sched_getaffinity(0, sizeof set, &set) == 0) {
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set))
                cpus.push_back(i);
    }
    return cpus;
}

inline void pinTo(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    ::sched_setaffinity(0, sizeof set, &set);
}

// ------------------------------------------------------- random workloads --

// The long-run mix: queries, reports with 0-4 items and sometimes 1-16 KiB
// item blobs, detection lists, struct-only frames, and now and then a report
// carrying a 1-4 MiB blob.
class Workload {
public:
    Workload(uint64_t seed, uint32_t outlierEvery)
        : random_(static_cast<uint32_t>(seed)), outlierEvery_(outlierEvery), query_(makeQuery()),
          report_(makeReport(0, 0, 4)), frame_(makeFrame()), detections_(makeDetections(0, 64)) {}

    Input next() {
        Input in;
        const uint32_t pick = random_() % 100;
        if (outlierEvery_ && random_() % outlierEvery_ == 0) {
            report_.itemCount = 1;
            report_.items[0].blobSize = (size_t{1} << 20) + random_() % (size_t{3} << 20);
            report_.items[0].blob = blobBytes().data();
            ++outliers_;
            in.type = Type::Report;
            in.report = &report_;
        } else if (pick < 35) {
            query_.queryId = random_();
            in.type = Type::Query;
            in.query = &query_;
        } else if (pick < 85) {
            report_.itemCount = random_() % 5;
            for (size_t i = 0; i < report_.itemCount; ++i) {
                ItemData& p = report_.items[i];
                p.blobSize = random_() % 10 < 3 ? 1024 + random_() % (15 * 1024) : 0;
                p.blob = blobBytes().data() + (random_() % 256) * 4096;
            }
            report_.queryId = random_();
            in.type = Type::Report;
            in.report = &report_;
        } else if (pick < 95) {
            detections_.count = random_() % 65;
            in.type = Type::Detections;
            in.detections = &detections_;
        } else {
            frame_.queryId = random_();
            in.type = Type::Frame;
            in.frame = &frame_;
        }
        return in;
    }

    uint64_t outliers() const { return outliers_; }

private:
    std::mt19937 random_;
    uint32_t outlierEvery_;
    QueryData query_;
    ReportData report_;
    FrameData frame_;
    DetectionsData detections_;
    uint64_t outliers_ = 0;
};

// One long run: build into a FIFO of `inFlight` live messages; each slot's old
// message is verified, read and freed before the slot takes a new one.
// Build(const Input&) -> Message; Consume(Type, Message&) -> uint64_t.
template <class Message, class Build, class Consume>
LongRunResult runLongRun(const LongRunConfig& config, Build build, Consume consume) {
    struct Slot {
        Message message{};
        Type type = Type::Query;
        bool live = false;
    };
    LongRunResult result;
    Workload workload(config.seed, config.outlierEvery);
    std::vector<Slot> slots(config.inFlight);
    result.rssStart = rssBytes();
    const uint64_t warm = std::max<uint64_t>(config.cycles / 100, 1);
    const double start = now();
    for (uint64_t cycle = 0; cycle < config.cycles; ++cycle) {
        Slot& slot = slots[cycle % slots.size()];
        if (slot.live)
            result.checksum += consume(slot.type, slot.message);
        const Input in = workload.next();
        slot.message = build(in);
        slot.type = in.type;
        slot.live = true;
        if (cycle + 1 == warm)
            result.rssWarm = rssBytes();
        else if (cycle + 1 == config.cycles / 2)
            result.rssMid = rssBytes();
    }
    result.rssEnd = rssBytes();
    for (Slot& slot : slots) {
        if (slot.live)
            result.checksum += consume(slot.type, slot.message);
        slot.message = Message{};
    }
    result.seconds = now() - start;
    result.rssAfterDrain = rssBytes();
    result.rssPeak = peakRssBytes();
    result.cycles = config.cycles;
    result.outliers = workload.outliers();
    return result;
}

// `threads` threads each running the long-run loop at once, or, with handoff,
// each passing what it built to the next thread, which reads and frees it.
template <class Message, class Build, class Consume>
MtResult runMultiThread(const MtConfig& config, Build build, Consume consume) {
    struct Item {
        Message message{};
        Type type = Type::Query;
    };
    struct Mailbox {
        std::mutex mutex;
        std::vector<Item> items;
    };
    const std::vector<int> cpus = allowedCpus();
    std::vector<Mailbox> mailboxes(config.threads);
    std::atomic<uint64_t> checksum{0};
    std::atomic<uint32_t> producing{config.threads};
    std::atomic<uint32_t> ready{0};
    std::atomic<bool> go{false};
    auto worker = [&](uint32_t index) {
        if (!cpus.empty())
            pinTo(cpus[index % cpus.size()]);
        Workload workload(1000 + index, config.outlierEvery);
        uint64_t sum = 0;
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) {
        }
        if (!config.handoff) {
            std::vector<Item> slots(8);
            std::vector<bool> live(slots.size(), false);
            for (uint64_t cycle = 0; cycle < config.cyclesPerThread; ++cycle) {
                const size_t s = cycle % slots.size();
                if (live[s])
                    sum += consume(slots[s].type, slots[s].message);
                const Input in = workload.next();
                slots[s].message = build(in);
                slots[s].type = in.type;
                live[s] = true;
            }
            for (size_t s = 0; s < slots.size(); ++s)
                if (live[s])
                    sum += consume(slots[s].type, slots[s].message);
        } else {
            Mailbox& next = mailboxes[(index + 1) % config.threads];
            Mailbox& mine = mailboxes[index];
            std::vector<Item> batch, inbox;
            auto drain = [&] {
                {
                    std::lock_guard<std::mutex> lock(mine.mutex);
                    inbox.swap(mine.items);
                }
                for (Item& item : inbox)
                    sum += consume(item.type, item.message);
                inbox.clear();
            };
            for (uint64_t cycle = 0; cycle < config.cyclesPerThread; cycle += 8) {
                for (int i = 0; i < 8; ++i) {
                    const Input in = workload.next();
                    batch.push_back(Item{build(in), in.type});
                }
                // At most ~256 messages wait in a mailbox: a full one makes
                // its producer read its own inbox until there is room.
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(next.mutex);
                        if (next.items.size() < 256) {
                            for (Item& item : batch)
                                next.items.push_back(std::move(item));
                            break;
                        }
                    }
                    drain();
                    std::this_thread::yield();
                }
                batch.clear();
                drain();
            }
            producing.fetch_sub(1);
            while (producing.load() != 0)
                drain();
            drain();
        }
        checksum.fetch_add(sum);
    };
    std::vector<std::thread> pool;
    for (uint32_t i = 0; i < config.threads; ++i)
        pool.emplace_back(worker, i);
    while (ready.load() != config.threads) {
    }
    const double start = now();
    go.store(true, std::memory_order_release);
    for (std::thread& t : pool)
        t.join();
    MtResult result;
    result.seconds = now() - start;
    result.messages = config.cyclesPerThread * config.threads;
    result.rssEnd = rssBytes();
    result.rssPeak = peakRssBytes();
    result.checksum = checksum.load();
    return result;
}

// Keeps the compiler from merging or hoisting work across loop iterations:
// every iteration must reload the message and redo the work.
inline void clobber() { asm volatile("" ::: "memory"); }
template <class T>
inline T* escape(T* p) {
    asm volatile("" : "+r"(p) : : "memory");
    return p;
}

// Folds a float into a checksum so reads cannot be optimized away.
inline uint64_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

} // namespace bd
