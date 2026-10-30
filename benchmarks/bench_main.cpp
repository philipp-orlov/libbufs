// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libbufs vs FlatBuffers vs a JSON baseline, on a small set of request,
// report, struct-only and vector-of-structs messages. See README.md.
//
//   bufs_bench [--quick] [--only sizes,speed,heap,longrun,mt]
//              [--formats libbufs,flatbuffers,json] [--cycles N]
//              [--threads N] [--mt-cycles N] [--batches N] [--batch-ms N]

#include <sys/utsname.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "bench_common.hpp"

#ifdef BENCH_HAVE_FLATBUFFERS
const char* flatbuffersVersion();
#endif

namespace {

using bd::Codec;
using bd::Input;
using bd::Type;

struct Options {
    bool quick = false;
    std::vector<std::string> only;
    std::vector<std::string> formats;
    uint64_t cycles = 5000000;
    uint32_t threads = 8;
    uint64_t mtCycles = 400000;
    int batches = 15;
    double batchSeconds = 0.02;
    uint32_t jsonDivisor = 10;

    bool wants(const char* section) const {
        return only.empty() || std::find(only.begin(), only.end(), section) != only.end();
    }
};

struct Case {
    const char* name;
    Input in;
};

volatile uint64_t sink;

struct Stat {
    double median = 0, min = 0, max = 0;
};

// ns per operation: run(n) does n operations. Calibrates n so that a batch
// takes about batchSeconds, then times `batches` batches.
Stat measure(const std::function<uint64_t(uint64_t)>& run, const Options& o) {
    uint64_t n = 1;
    double t = 0;
    for (;;) {
        const double start = bd::now();
        sink = sink + run(n);
        t = bd::now() - start;
        if (t > o.batchSeconds / 4 || n >= (uint64_t{1} << 32))
            break;
        n *= 4;
    }
    n = std::max<uint64_t>(1, static_cast<uint64_t>(static_cast<double>(n) * o.batchSeconds / t));
    std::vector<double> perOp;
    for (int b = 0; b < o.batches; ++b) {
        const double start = bd::now();
        sink = sink + run(n);
        perOp.push_back((bd::now() - start) / static_cast<double>(n) * 1e9);
    }
    std::sort(perOp.begin(), perOp.end());
    return Stat{perOp[perOp.size() / 2], perOp.front(), perOp.back()};
}

struct Heap {
    double allocations = 0, bytes = 0;
};

Heap heapPer(const std::function<uint64_t(uint64_t)>& run, uint64_t n) {
    sink = sink + run(n); // warm: pools and caches at their steady state
    bd::heap::allocations = 0;
    bd::heap::bytes = 0;
    bd::heap::counting = true;
    sink = sink + run(n);
    bd::heap::counting = false;
    return Heap{static_cast<double>(bd::heap::allocations.load()) / static_cast<double>(n),
                static_cast<double>(bd::heap::bytes.load()) / static_cast<double>(n)};
}

std::string cell(const Stat& s) {
    char text[64];
    if (s.median < 100)
        std::snprintf(text, sizeof text, "%.1f (%.1f-%.1f)", s.median, s.min, s.max);
    else
        std::snprintf(text, sizeof text, "%.0f (%.0f-%.0f)", s.median, s.min, s.max);
    return text;
}

std::string mib(size_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", static_cast<double>(bytes) / 1048576.0);
    return text;
}

constexpr int kNameWidth = 16;
constexpr int kCellWidth = 22;

void header(const char* title, const std::vector<Case>& cases) {
    std::printf("\n%s\n%-*s", title, kNameWidth, "format");
    for (const Case& c : cases)
        std::printf(" %*s", kCellWidth, c.name);
    std::printf("\n");
}

void hostLine(const Options& o, const std::vector<int>& cpus) {
    std::string model = "?";
    if (FILE* f = std::fopen("/proc/cpuinfo", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "model name", 10) == 0) {
                const char* colon = std::strchr(line, ':');
                model = colon ? colon + 2 : line;
                model.erase(model.find_last_not_of("\n ") + 1);
                break;
            }
        }
        std::fclose(f);
    }
    utsname u{};
    ::uname(&u);
    char date[64];
    const std::time_t t = std::time(nullptr);
    std::strftime(date, sizeof date, "%Y-%m-%d %H:%M UTC", std::gmtime(&t));
    std::printf("bufs_bench  %s\n", date);
    std::printf("host: %s, %ld online CPUs, %s %s; compiler: %s\n", model.c_str(), ::sysconf(_SC_NPROCESSORS_ONLN),
                u.sysname, u.release, __VERSION__);
    std::printf("allowed CPUs:");
    for (int cpu : cpus)
        std::printf(" %d", cpu);
    std::printf("  (single-threaded parts pinned to CPU %d)\n", cpus.empty() ? -1 : cpus.front());
#ifdef BENCH_HAVE_FLATBUFFERS
    std::printf("FlatBuffers %s\n", flatbuffersVersion());
#else
    std::printf("FlatBuffers: not built (configure with -DFLATBUFFERS_DIR=...)\n");
#endif
    std::printf("mode: %s, %d batches of ~%.0f ms per cell; cells are median (min-max) over batches\n",
                o.quick ? "quick" : "full", o.batches, o.batchSeconds * 1e3);
}

void speedTable(const char* title, const std::vector<std::unique_ptr<Codec>>& codecs, const std::vector<Case>& cases,
                const Options& o, const std::function<bool(Codec&)>& applies,
                const std::function<uint64_t(Codec&, const Case&, const std::vector<uint8_t>&, uint64_t)>& op,
                const std::vector<std::vector<std::vector<uint8_t>>>& encoded) {
    header(title, cases);
    for (size_t f = 0; f < codecs.size(); ++f) {
        Codec& codec = *codecs[f];
        if (!applies(codec))
            continue;
        std::printf("%-*s", kNameWidth, codec.name().c_str());
        std::fflush(stdout);
        for (size_t c = 0; c < cases.size(); ++c) {
            const Stat s = measure([&](uint64_t n) { return op(codec, cases[c], encoded[f][c], n); }, o);
            std::printf(" %*s", kCellWidth, cell(s).c_str());
            std::fflush(stdout);
        }
        std::printf("\n");
    }
}

// Runs fn in a child process (fresh heap and RSS), waiting for it.
void inChild(const std::function<void()>& fn) {
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        fn();
        std::fflush(stdout);
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        std::printf("  (child failed, status %d)\n", status);
}

void setAffinity(const std::vector<int>& cpus) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : cpus)
        CPU_SET(cpu, &set);
    ::sched_setaffinity(0, sizeof set, &set);
}

void usage() {
    std::fprintf(stderr,
                 "usage: bufs_bench [--quick] [--only sizes,speed,heap,longrun,mt] [--formats libbufs,flatbuffers,json]\n"
                 "                  [--cycles N]\n"
                 "                  [--threads N] [--mt-cycles N] [--batches N] [--batch-ms N]\n"
                 "Run under taskset: single-threaded parts pin to the first allowed CPU, the\n"
                 "multi-threaded part spreads over the first --threads allowed CPUs.\n");
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--quick") {
            o.quick = true;
            o.cycles = 200000;
            o.mtCycles = 40000;
            o.batches = 5;
            o.batchSeconds = 0.01;
        } else if (arg == "--only") {
            std::string list = value();
            size_t at = 0;
            while (at <= list.size()) {
                const size_t comma = std::min(list.find(',', at), list.size());
                o.only.push_back(list.substr(at, comma - at));
                at = comma + 1;
            }
        } else if (arg == "--formats") {
            std::string list = value();
            size_t at = 0;
            while (at <= list.size()) {
                const size_t comma = std::min(list.find(',', at), list.size());
                o.formats.push_back(list.substr(at, comma - at));
                at = comma + 1;
            }
        } else if (arg == "--cycles") {
            o.cycles = std::stoull(value());
        } else if (arg == "--threads") {
            o.threads = static_cast<uint32_t>(std::stoul(value()));
        } else if (arg == "--mt-cycles") {
            o.mtCycles = std::stoull(value());
        } else if (arg == "--batches") {
            o.batches = std::max(1, std::stoi(value()));
        } else if (arg == "--batch-ms") {
            o.batchSeconds = std::stod(value()) / 1e3;
        } else {
            usage();
            return arg == "-h" || arg == "--help" ? 0 : 2;
        }
    }

    const std::vector<int> cpus = bd::allowedCpus();
    hostLine(o, cpus);
    if (!cpus.empty())
        bd::pinTo(cpus.front());

    const bd::QueryData query = bd::makeQuery();
    const bd::ReportData report1 = bd::makeReport(1, 0), report4 = bd::makeReport(4, 0);
    const bd::ReportData report1Blob = bd::makeReport(1, 8192), report4Blob = bd::makeReport(4, 8192);
    const bd::FrameData frame = bd::makeFrame();
    const bd::DetectionsData detections = bd::makeDetections(16);
    auto queryInput = [](const bd::QueryData& d) {
        Input in;
        in.type = Type::Query;
        in.query = &d;
        return in;
    };
    auto reportInput = [](const bd::ReportData& d) {
        Input in;
        in.type = Type::Report;
        in.report = &d;
        return in;
    };
    Input frameInput;
    frameInput.type = Type::Frame;
    frameInput.frame = &frame;
    Input detectionsInput;
    detectionsInput.type = Type::Detections;
    detectionsInput.detections = &detections;
    const std::vector<Case> cases = {
        {"request", queryInput(query)},
        {"report 1 item", reportInput(report1)},
        {"report 4 items", reportInput(report4)},
        {"report 1 + 8K blob", reportInput(report1Blob)},
        {"report 4 + 8K blob", reportInput(report4Blob)},
        {"frame (struct)", frameInput},
        {"16 detections", detectionsInput},
    };

    std::vector<std::unique_ptr<Codec>> codecs;
    codecs.push_back(bd::makeBufsCodec());
#ifdef BENCH_HAVE_FLATBUFFERS
    codecs.push_back(bd::makeFlatBuffersCodec());
#endif
#ifdef BENCH_HAVE_JSON
    codecs.push_back(bd::makeJsonCodec());
    codecs.push_back(bd::makeJsonLiteCodec());
#endif
    if (!o.formats.empty()) {
        codecs.erase(std::remove_if(codecs.begin(), codecs.end(),
                                    [&](const std::unique_ptr<Codec>& c) {
                                        return std::none_of(o.formats.begin(), o.formats.end(),
                                                            [&](const std::string& f) {
                                                                return c->name().rfind(f, 0) == 0;
                                                            });
                                    }),
                     codecs.end());
    }

    // Each format's encoding of each case, copied to 64-byte aligned memory.
    std::vector<std::vector<std::vector<uint8_t>>> encoded(codecs.size());
    std::vector<std::vector<uint8_t*>> aligned(codecs.size());
    std::vector<std::vector<std::unique_ptr<uint8_t, decltype(&std::free)>>> owners(codecs.size());
    for (size_t f = 0; f < codecs.size(); ++f) {
        for (const Case& c : cases) {
            encoded[f].push_back(codecs[f]->encode(c.in));
            const std::vector<uint8_t>& bytes = encoded[f].back();
            uint8_t* p = static_cast<uint8_t*>(std::aligned_alloc(64, (bytes.size() + 63) / 64 * 64 + 64));
            std::memcpy(p, bytes.data(), bytes.size());
            owners[f].emplace_back(p, &std::free);
            aligned[f].push_back(p);
            if (codecs[f]->decode(c.in.type, p, bytes.size(), 1) == 0) {
                std::fprintf(stderr, "%s: %s does not decode\n", codecs[f]->name().c_str(), c.name);
                return 1;
            }
        }
    }

    // The binary formats read the same values, so their checksums must agree.
    for (size_t f = 1; f < codecs.size(); ++f) {
        if (codecs[f]->name().rfind("json", 0) == 0 || codecs[0]->name() != "libbufs")
            continue;
        for (size_t c = 0; c < cases.size(); ++c) {
            const uint64_t a = codecs[0]->read(cases[c].in.type, aligned[0][c], encoded[0][c].size(), 1);
            const uint64_t b = codecs[f]->read(cases[c].in.type, aligned[f][c], encoded[f][c].size(), 1);
            if (a != b) {
                std::fprintf(stderr, "%s reads %s differently from libbufs\n", codecs[f]->name().c_str(),
                             cases[c].name);
                return 1;
            }
        }
    }

    if (o.wants("sizes")) {
        header("Encoded size, bytes", cases);
        for (size_t f = 0; f < codecs.size(); ++f) {
            std::printf("%-*s", kNameWidth, codecs[f]->name().c_str());
            for (size_t c = 0; c < cases.size(); ++c)
                std::printf(" %*zu", kCellWidth, encoded[f][c].size());
            std::printf("\n");
        }
    }

    // The ops below take the original encoding only for its size; they read
    // from the aligned copy.
    auto data = [&](Codec& codec) -> std::vector<uint8_t*>& {
        for (size_t f = 0; f < codecs.size(); ++f)
            if (codecs[f].get() == &codec)
                return aligned[f];
        std::abort();
    };
    auto caseIndex = [&](const Case& c) { return static_cast<size_t>(&c - cases.data()); };
    auto all = [](Codec&) { return true; };
    auto encoders = [](Codec& c) { return c.ownsEncoder(); };

    if (o.wants("speed")) {
        speedTable("Build, ns/message, one builder reused (bufs: clear(); flatbuffers: Clear(); json: tree + dump "
                   "into a recycled string)",
                   codecs, cases, o, encoders,
                   [](Codec& codec, const Case& c, const std::vector<uint8_t>&, uint64_t n) {
                       return codec.buildReuse(c.in, n);
                   },
                   encoded);
        speedTable("Build, ns/message, a new builder per message, finished message kept (bufs: MessageBuilder(pool) "
                   "+ release(); flatbuffers: FlatBufferBuilder + Release(); json: tree + dump())",
                   codecs, cases, o, encoders,
                   [](Codec& codec, const Case& c, const std::vector<uint8_t>&, uint64_t n) {
                       return codec.buildFresh(c.in, n);
                   },
                   encoded);
        speedTable("Verify, ns/message (json: parse)", codecs, cases, o, all,
                   [&](Codec& codec, const Case& c, const std::vector<uint8_t>& bytes, uint64_t n) {
                       return codec.verify(c.in.type, data(codec)[caseIndex(c)], bytes.size(), n);
                   },
                   encoded);
        speedTable("Read every field, ns/message (json: walk the parsed tree, base64-decode blobs)", codecs, cases, o,
                   all,
                   [&](Codec& codec, const Case& c, const std::vector<uint8_t>& bytes, uint64_t n) {
                       return codec.read(c.in.type, data(codec)[caseIndex(c)], bytes.size(), n);
                   },
                   encoded);
        speedTable("Verify + read, ns/message (json: parse + walk)", codecs, cases, o, all,
                   [&](Codec& codec, const Case& c, const std::vector<uint8_t>& bytes, uint64_t n) {
                       return codec.decode(c.in.type, data(codec)[caseIndex(c)], bytes.size(), n);
                   },
                   encoded);
    }

    if (o.wants("heap")) {
        std::printf("\nHeap allocations per message (malloc-level, includes operator new), after warm-up; "
                    "'n/B' = calls/bytes\n%-*s %-12s",
                    kNameWidth, "format", "operation");
        for (const Case& c : cases)
            std::printf(" %*s", 18, c.name);
        std::printf("\n");
        const uint64_t n = 2000;
        for (size_t f = 0; f < codecs.size(); ++f) {
            Codec& codec = *codecs[f];
            struct Row {
                const char* name;
                bool build;
                std::function<uint64_t(const Case&, size_t, uint64_t)> op;
            };
            const Row rows[] = {
                {"build reuse", true, [&](const Case& c, size_t, uint64_t k) { return codec.buildReuse(c.in, k); }},
                {"build fresh", true, [&](const Case& c, size_t, uint64_t k) { return codec.buildFresh(c.in, k); }},
                {"verify+read", false,
                 [&](const Case& c, size_t i, uint64_t k) {
                     return codec.decode(c.in.type, aligned[f][i], encoded[f][i].size(), k);
                 }},
            };
            for (const Row& row : rows) {
                if (row.build && !codec.ownsEncoder())
                    continue;
                std::printf("%-*s %-12s", kNameWidth, codec.name().c_str(), row.name);
                for (size_t i = 0; i < cases.size(); ++i) {
                    const Heap h = heapPer([&](uint64_t k) { return row.op(cases[i], i, k); }, n);
                    char text[48];
                    std::snprintf(text, sizeof text, "%.4g/%.0f", h.allocations, h.bytes);
                    std::printf(" %*s", 18, text);
                }
                std::printf("\n");
            }
        }
        std::printf("(libbufs takes message memory from its mmap'd pool, which malloc counting does not see; "
                    "after warm-up the pool makes no mmap calls either)\n");
    }

    if (o.wants("longrun")) {
        std::printf("\nLong run: build into a FIFO of 8 live messages, verify+read+free each as its slot is reused; "
                    "mix of requests, 0-4 item reports (30%% of items with a 1-16 KiB blob), 0-64 detections, "
                    "frames, and one 1-4 MiB blob report per ~2000. One process per format. RSS in MiB.\n");
        std::printf("%-*s %10s %8s %9s %7s %7s %7s %7s %8s %7s %7s  %s\n", kNameWidth, "format", "cycles", "outliers",
                    "Mcycle/s", "start", "warm", "mid", "end", "end-warm", "drained", "peak", "notes");
        for (auto& codec : codecs) {
            if (!codec->ownsEncoder())
                continue;
            inChild([&] {
                bd::LongRunConfig config;
                config.cycles = codec->name().rfind("json", 0) == 0 ? o.cycles / o.jsonDivisor : o.cycles;
                const bd::LongRunResult r = codec->longRun(config);
                sink = r.checksum;
                std::printf("%-*s %10llu %8llu %9.3f %7s %7s %7s %7s %8s %7s %7s  %s\n", kNameWidth,
                            codec->name().c_str(), static_cast<unsigned long long>(r.cycles),
                            static_cast<unsigned long long>(r.outliers), r.cycles / r.seconds / 1e6,
                            mib(r.rssStart).c_str(), mib(r.rssWarm).c_str(), mib(r.rssMid).c_str(),
                            mib(r.rssEnd).c_str(),
                            (r.rssEnd >= r.rssWarm ? "+" + mib(r.rssEnd - r.rssWarm) : "-" + mib(r.rssWarm - r.rssEnd))
                                .c_str(),
                            mib(r.rssAfterDrain).c_str(), mib(r.rssPeak).c_str(), r.note.c_str());
            });
        }
        if (o.jsonDivisor > 1)
            std::printf("(json runs 1/%u of the cycles: it is that much slower)\n", o.jsonDivisor);
    }

    if (o.wants("mt")) {
        const uint32_t usable = static_cast<uint32_t>(cpus.size());
        std::printf("\nMulti-threaded: %u threads at once, each running the long-run mix (%llu cycles each); "
                    "'handoff' passes every message to the next thread to verify, read and free. %u CPUs allowed%s. "
                    "RSS in MiB.\n",
                    o.threads, static_cast<unsigned long long>(o.mtCycles), usable,
                    usable < o.threads ? " -- FEWER THAN THREADS, throughput is not meaningful" : "");
        std::printf("%-*s %-26s %10s %9s %8s %8s  %s\n", kNameWidth, "format", "mode", "messages", "M msg/s", "RSS end",
                    "peak", "notes");
        for (auto& codec : codecs) {
            if (!codec->ownsEncoder())
                continue;
            const std::vector<std::string> variants = codec->mtVariants();
            for (bool handoff : {false, true}) {
                for (uint32_t variant = 0; variant < variants.size(); ++variant) {
                    inChild([&] {
                        setAffinity(cpus);
                        bd::MtConfig config;
                        config.threads = o.threads;
                        config.cyclesPerThread =
                            (codec->name().rfind("json", 0) == 0 ? o.mtCycles / o.jsonDivisor : o.mtCycles) / 8 * 8;
                        config.handoff = handoff;
                        config.variant = variant;
                        const bd::MtResult r = codec->multiThread(config);
                        sink = r.checksum;
                        std::string mode = handoff ? "handoff" : "independent";
                        if (!variants[variant].empty())
                            mode += ", " + variants[variant];
                        std::printf("%-*s %-26s %10llu %9.3f %8s %8s  %s\n", kNameWidth, codec->name().c_str(),
                                    mode.c_str(), static_cast<unsigned long long>(r.messages),
                                    r.messages / r.seconds / 1e6, mib(r.rssEnd).c_str(), mib(r.rssPeak).c_str(),
                                    r.note.c_str());
                    });
                }
            }
        }
    }
    return 0;
}
