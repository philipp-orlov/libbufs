// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libbufs as a benchmark codec.

#include <cstdlib>

#include "bench_bufs.hpp"

namespace {

using bd::Input;
using bd::Type;

class BufsCodec : public bd::Codec {
public:
    std::string name() const override { return "libbufs"; }

    std::vector<uint8_t> encode(const Input& in) override {
        bufs::MessageBuilder mb(pool_);
        bufsbench::build(mb, in);
        const auto [data, size] = mb.finish();
        return std::vector<uint8_t>(data, data + size);
    }

    // One builder, cleared between messages: its block is reused.
    uint64_t buildReuse(const Input& in, uint64_t n) override {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            builder_.clear();
            bufsbench::build(builder_, *bd::escape(&in));
            sum += builder_.finish().second;
            bd::clobber();
        }
        return sum;
    }

    // A new MessageBuilder per message, released into a pooled Message.
    uint64_t buildFresh(const Input& in, uint64_t n) override {
        uint64_t sum = 0;
        bufs::Message last;
        for (uint64_t i = 0; i < n; ++i) {
            bufs::MessageBuilder mb(pool_);
            bufsbench::build(mb, *bd::escape(&in));
            last = mb.release();
            sum += last.size();
            bd::clobber();
        }
        return sum;
    }

    uint64_t verify(Type type, const uint8_t* data, size_t size, uint64_t n) override {
        uint64_t ok = 0;
        for (uint64_t i = 0; i < n; ++i) {
            ok += bufsbench::verify(type, bd::escape(data), size);
            bd::clobber();
        }
        return ok;
    }

    uint64_t read(Type type, const uint8_t* data, size_t, uint64_t n) override {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            sum += bufsbench::read(type, bd::escape(data));
            bd::clobber();
        }
        return sum;
    }

    uint64_t decode(Type type, const uint8_t* data, size_t size, uint64_t n) override {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t* p = bd::escape(data);
            if (bufsbench::verify(type, p, size))
                sum += bufsbench::read(type, p);
            bd::clobber();
        }
        return sum;
    }

    bd::LongRunResult longRun(const bd::LongRunConfig& config) override {
        bufs::BufferPool pool;
        auto result = bd::runLongRun<bufs::Message>(
            config,
            [&](const Input& in) {
                bufs::MessageBuilder mb(pool);
                bufsbench::build(mb, in);
                return mb.release();
            },
            [](Type type, bufs::Message& m) { return consume(type, m); });
        result.note = poolNote(pool);
        return result;
    }

    std::vector<std::string> mtVariants() const override { return {"shared pool", "pool/thread"}; }

    // Variant 0: one pool shared by every thread, as BufferPool::global() is.
    // Variant 1: a pool per thread (messages handed off still return to the
    // pool they came from).
    bd::MtResult multiThread(const bd::MtConfig& config) override {
        std::vector<std::unique_ptr<bufs::BufferPool>> pools;
        for (uint32_t i = 0; i < (config.variant == 0 ? 1 : config.threads); ++i)
            pools.push_back(std::make_unique<bufs::BufferPool>());
        std::atomic<uint32_t> next{0};
        auto result = bd::runMultiThread<bufs::Message>(
            config,
            [&](const Input& in) {
                thread_local bufs::BufferPool* pool = nullptr;
                if (!pool)
                    pool = pools[next.fetch_add(1) % pools.size()].get();
                bufs::MessageBuilder mb(*pool);
                bufsbench::build(mb, in);
                return mb.release();
            },
            [](Type type, bufs::Message& m) { return consume(type, m); });
        bufs::BufferPool::Stats total;
        for (const auto& pool : pools) {
            const bufs::BufferPool::Stats s = pool->stats();
            total.mappedBytes += s.mappedBytes;
            total.retainedBytes += s.retainedBytes;
            total.mapCalls += s.mapCalls;
            total.unmapCalls += s.unmapCalls;
        }
        result.note = statsNote(total);
        return result;
    }

private:
    static uint64_t consume(Type type, bufs::Message& m) {
        if (!bufsbench::verify(type, m.data(), m.size())) {
            std::fprintf(stderr, "libbufs: a message failed to verify\n");
            std::abort();
        }
        const uint64_t sum = bufsbench::read(type, m.data());
        m.reset();
        return sum;
    }

    static std::string poolNote(const bufs::BufferPool& pool) { return statsNote(pool.stats()); }

    static std::string statsNote(const bufs::BufferPool::Stats& s) {
        char text[160];
        std::snprintf(text, sizeof text, "pool: %.1f MiB mapped (%.1f MiB idle large), %llu mmap, %llu munmap",
                      s.mappedBytes / 1048576.0, s.retainedBytes / 1048576.0,
                      static_cast<unsigned long long>(s.mapCalls), static_cast<unsigned long long>(s.unmapCalls));
        return text;
    }

    bufs::BufferPool pool_;
    bufs::MessageBuilder builder_{pool_};
};

} // namespace

std::unique_ptr<bd::Codec> bd::makeBufsCodec() { return std::make_unique<BufsCodec>(); }
