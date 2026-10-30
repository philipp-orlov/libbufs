// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Pooled message memory. Blocks come in power-of-two size classes and are
// never taken from the malloc heap:
//
// * Small classes (256 B .. 64 KiB) are carved out of slabs that hold blocks
//   of one size only. A slab is mapped once and kept, so the pool's memory is
//   bounded by the peak number of blocks in use per class and never
//   fragments: every hole a block leaves is exactly the size of the next block
//   of that class.
// * Large classes (128 KiB .. 64 MiB) are mapped one block per mapping. A few
//   idle ones per class are kept (Options). One that stays idle for
//   Options::releasePagesAfter gives its pages back, so outliers cost no
//   resident memory once they stop coming; past the limits a released block
//   is unmapped at once.
// * Anything above the largest class is mapped and unmapped per use.
//
// Every class is split into 16 shards with their own lock and free list, and a
// thread uses one shard, so threads building messages at the same time do not
// contend. After a warm-up in which every small class has seen its peak,
// acquire() and release() make no system call and touch no allocator.

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace bufs {

class BufferPool;

// One pooled block, returned to its pool when destroyed. Move-only.
class Buffer {
public:
    Buffer() = default;
    Buffer(BufferPool* pool, uint8_t* data, size_t capacity) : pool_(pool), data_(data), capacity_(capacity) {}
    ~Buffer() { reset(); }
    Buffer(Buffer&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)), data_(std::exchange(other.data_, nullptr)),
          capacity_(std::exchange(other.capacity_, 0)) {}
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            reset();
            pool_ = std::exchange(other.pool_, nullptr);
            data_ = std::exchange(other.data_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    uint8_t* data() const { return data_; }
    size_t capacity() const { return capacity_; }
    explicit operator bool() const { return data_ != nullptr; }

    inline void reset() noexcept;

private:
    BufferPool* pool_ = nullptr;
    uint8_t* data_ = nullptr;
    size_t capacity_ = 0;
};

class BufferPool {
public:
    struct Options {
        // Idle large blocks kept for reuse, in total; more are unmapped on release.
        size_t maxRetainedLargeBytes = size_t{64} << 20;
        // Idle blocks kept per large class.
        size_t maxRetainedPerLargeClass = 16;
        // An idle large block that stays unused this long gives its pages back
        // (MADV_DONTNEED) and keeps only its address range: while outliers keep
        // coming their blocks stay warm, and once they stop the memory returns
        // to the OS. Checked on large-block traffic and every 4096 releases.
        std::chrono::milliseconds releasePagesAfter{250};
        // Size of one slab for the small classes.
        size_t slabBytes = size_t{256} << 10;
    };

    struct Stats {
        size_t mappedBytes = 0;   // currently mapped from the OS, slabs and large blocks
        size_t retainedBytes = 0; // idle large blocks kept for reuse
        size_t blocksInUse = 0;
        uint64_t mapCalls = 0;    // mmap() calls since the pool was made
        uint64_t unmapCalls = 0;
    };

    static constexpr size_t kMinClassBytes = 256;
    static constexpr size_t kMaxSmallBytes = size_t{64} << 10;
    static constexpr size_t kMaxClassBytes = size_t{64} << 20;

    BufferPool() : BufferPool(Options{}) {}
    explicit BufferPool(Options options) : options_(options) {
        options_.slabBytes = std::max(options_.slabBytes, kMaxSmallBytes);
        for (int i = 0; i < kClasses; ++i) {
            classes_[static_cast<size_t>(i)].bytes = size_t{1} << (i + kMinShift);
            classes_[static_cast<size_t>(i)].small = classes_[static_cast<size_t>(i)].bytes <= kMaxSmallBytes;
        }
    }
    ~BufferPool() {
        for (Class& c : classes_) {
            if (c.small)
                continue;
            for (Shard& shard : c.shards)
                for (FreeBlock* b = shard.free; b;) {
                    FreeBlock* next = b->next;
                    ::munmap(b, c.bytes);
                    b = next;
                }
        }
        for (Slab* s = slabs_; s;) {
            Slab* next = s->next;
            ::munmap(s->memory, s->bytes);
            delete s;
            s = next;
        }
    }
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // The process-wide pool; lives until exit.
    static BufferPool& global() {
        static BufferPool* pool = new BufferPool();
        return *pool;
    }

    // A block of at least `minBytes`, 64-byte aligned. Its contents are
    // whatever the previous user left. Throws std::bad_alloc when the OS
    // refuses the memory.
    Buffer acquire(size_t minBytes) {
        const size_t want = std::max(minBytes, kMinClassBytes);
        if (want > kMaxClassBytes) {
            const size_t bytes = roundUp(want, kPage);
            uint8_t* p = map(bytes);
            hugeInUse_.fetch_add(1, std::memory_order_relaxed);
            return Buffer(this, p, bytes);
        }
        Class& c = classes_[static_cast<size_t>(classIndex(want))];
        Shard& own = c.shards[shardOfThisThread()];
        uint8_t* p = nullptr;
        {
            std::lock_guard<std::mutex> lock(own.mutex);
            p = pop(own);
            if (p)
                bump(own.inUse, 1);
        }
        if (!p)
            p = steal(c, own);
        if (p) {
            if (!c.small) {
                unretain(c);
                releaseIdlePages(nowNs());
            }
            return Buffer(this, p, c.bytes);
        }
        if (c.small) {
            std::lock_guard<std::mutex> lock(c.carveMutex);
            p = carve(c);
        } else {
            p = map(c.bytes);
        }
        std::lock_guard<std::mutex> lock(own.mutex);
        bump(own.inUse, 1);
        return Buffer(this, p, c.bytes);
    }

    // Returns a block taken from acquire(). Prefer letting the Buffer do it.
    void release(uint8_t* data, size_t capacity) noexcept {
        if (capacity > kMaxClassBytes) {
            hugeInUse_.fetch_sub(1, std::memory_order_relaxed);
            unmap(data, capacity);
            return;
        }
        Class& c = classes_[static_cast<size_t>(classIndex(capacity))];
        Shard& own = c.shards[shardOfThisThread()];
        if (!c.small) {
            const int64_t now = nowNs();
            if (!retain(c)) {
                unmap(data, c.bytes);
                std::lock_guard<std::mutex> lock(own.mutex);
                bump(own.inUse, -1);
            } else {
                FreeBlock* b = reinterpret_cast<FreeBlock*>(data);
                b->idleSinceNs = now;
                b->pagesReleased = false;
                std::lock_guard<std::mutex> lock(own.mutex);
                push(own, data);
                bump(own.inUse, -1);
            }
            releaseIdlePages(now);
            return;
        }
        bool check = false;
        {
            std::lock_guard<std::mutex> lock(own.mutex);
            push(own, data);
            bump(own.inUse, -1);
            check = (++own.releases & 4095) == 0;
        }
        if (check && retainedBytes_.load(std::memory_order_relaxed) > 0)
            releaseIdlePages(nowNs());
    }

    // Unmaps every idle large block. Small-class slabs stay mapped.
    void trim() noexcept {
        for (Class& c : classes_) {
            if (c.small)
                continue;
            for (Shard& shard : c.shards) {
                FreeBlock* list = nullptr;
                {
                    std::lock_guard<std::mutex> lock(shard.mutex);
                    list = std::exchange(shard.free, nullptr);
                    shard.idle.store(0, std::memory_order_relaxed);
                }
                while (list) {
                    FreeBlock* next = list->next;
                    c.retained.fetch_sub(1, std::memory_order_relaxed);
                    retainedBytes_.fetch_sub(c.bytes, std::memory_order_relaxed);
                    unmap(reinterpret_cast<uint8_t*>(list), c.bytes);
                    list = next;
                }
            }
        }
    }

    Stats stats() const {
        Stats s;
        s.mappedBytes = mappedBytes_.load(std::memory_order_relaxed);
        s.retainedBytes = retainedBytes_.load(std::memory_order_relaxed);
        int64_t inUse = static_cast<int64_t>(hugeInUse_.load(std::memory_order_relaxed));
        for (const Class& c : classes_)
            for (const Shard& shard : c.shards)
                inUse += shard.inUse.load(std::memory_order_relaxed);
        s.blocksInUse = static_cast<size_t>(inUse);
        s.mapCalls = mapCalls_.load(std::memory_order_relaxed);
        s.unmapCalls = unmapCalls_.load(std::memory_order_relaxed);
        return s;
    }

    // The capacity acquire(bytes) hands out.
    static size_t classBytes(size_t bytes) {
        const size_t want = std::max(bytes, kMinClassBytes);
        return want > kMaxClassBytes ? roundUp(want, kPage) : size_t{1} << (classIndex(want) + kMinShift);
    }

private:
    static constexpr size_t kPage = 4096;
    static constexpr int kMinShift = 8; // 256 B
    static constexpr int kClasses = 19; // 256 B .. 64 MiB
    // Each class is split into shards, one per thread round robin, so threads
    // do not share a lock or a cache line. A shard that runs dry takes a batch
    // from another: with one thread building and another freeing, blocks move
    // kStealBatch at a time, not one by one.
    static constexpr size_t kShards = 16;
    static constexpr size_t kStealBatch = 32;

    struct FreeBlock {
        FreeBlock* next;
        int64_t idleSinceNs; // large classes only
        bool pagesReleased;  // large classes only
    };
    struct Slab {
        uint8_t* memory;
        size_t bytes;
        Slab* next;
    };
    struct alignas(64) Shard {
        std::mutex mutex;
        FreeBlock* free = nullptr;
        std::atomic<size_t> idle{0}; // blocks on `free`; read without the lock as a hint
        std::atomic<int64_t> inUse{0}; // acquired here less released here; summed by stats()
        uint32_t releases = 0;         // drives the periodic idle-page check
    };
    struct Class {
        std::array<Shard, kShards> shards;
        std::mutex carveMutex;
        uint8_t* cursor = nullptr; // next uncarved block of the newest slab
        uint8_t* limit = nullptr;
        std::atomic<size_t> retained{0}; // idle large blocks of this class
        size_t bytes = 0;
        bool small = false;
    };

    static size_t roundUp(size_t value, size_t to) { return (value + to - 1) / to * to; }

    static int classIndex(size_t bytes) {
        int shift = kMinShift;
        while ((size_t{1} << shift) < bytes)
            ++shift;
        return shift - kMinShift;
    }

    static size_t shardOfThisThread() {
        static std::atomic<size_t> next{0};
        thread_local const size_t mine = next.fetch_add(1, std::memory_order_relaxed) % kShards;
        return mine;
    }

    // A shard's counters change only under its lock, so a plain load and
    // store does; they are atomic so stats() and steal() can read them
    // without the lock.
    template <class T>
    static void bump(std::atomic<T>& counter, int64_t by) {
        counter.store(static_cast<T>(static_cast<int64_t>(counter.load(std::memory_order_relaxed)) + by),
                      std::memory_order_relaxed);
    }

    // Called with the shard locked.
    static uint8_t* pop(Shard& shard) {
        FreeBlock* b = shard.free;
        if (!b)
            return nullptr;
        shard.free = b->next;
        bump(shard.idle, -1);
        return reinterpret_cast<uint8_t*>(b);
    }

    static void push(Shard& shard, uint8_t* data) {
        FreeBlock* b = reinterpret_cast<FreeBlock*>(data);
        b->next = shard.free;
        shard.free = b;
        bump(shard.idle, 1);
    }

    // Takes up to kStealBatch idle blocks from another shard: one for the
    // caller, the rest into `own`.
    static uint8_t* steal(Class& c, Shard& own) {
        const size_t start = static_cast<size_t>(&own - c.shards.data());
        for (size_t i = 1; i < kShards; ++i) {
            Shard& other = c.shards[(start + i) % kShards];
            if (other.idle.load(std::memory_order_relaxed) == 0)
                continue;
            FreeBlock* first = nullptr;
            FreeBlock* last = nullptr;
            size_t taken = 0;
            {
                std::lock_guard<std::mutex> lock(other.mutex);
                first = other.free;
                for (FreeBlock* b = first; b && taken < kStealBatch; b = b->next) {
                    last = b;
                    ++taken;
                }
                if (!taken)
                    continue;
                other.free = last->next;
                bump(other.idle, -static_cast<int64_t>(taken));
            }
            std::lock_guard<std::mutex> lock(own.mutex);
            if (taken > 1) {
                last->next = own.free;
                own.free = first->next;
                bump(own.idle, static_cast<int64_t>(taken) - 1);
            }
            bump(own.inUse, 1);
            return reinterpret_cast<uint8_t*>(first);
        }
        return nullptr;
    }

    // Whether an idle large block is kept: within the per-class and total limits.
    bool retain(Class& c) noexcept {
        if (c.retained.fetch_add(1, std::memory_order_relaxed) >= options_.maxRetainedPerLargeClass) {
            c.retained.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        if (retainedBytes_.fetch_add(c.bytes, std::memory_order_relaxed) + c.bytes > options_.maxRetainedLargeBytes) {
            retainedBytes_.fetch_sub(c.bytes, std::memory_order_relaxed);
            c.retained.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // A retained large block is in use again.
    void unretain(Class& c) noexcept {
        c.retained.fetch_sub(1, std::memory_order_relaxed);
        retainedBytes_.fetch_sub(c.bytes, std::memory_order_relaxed);
    }

    static int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Gives back the pages of every large block idle for releasePagesAfter.
    // The first page stays: it holds the free-list link.
    void releaseIdlePages(int64_t now) noexcept {
        const int64_t after = std::chrono::duration_cast<std::chrono::nanoseconds>(options_.releasePagesAfter).count();
        int64_t expected = lastIdleCheckNs_.load(std::memory_order_relaxed);
        if (now - expected < after / 4 ||
            !lastIdleCheckNs_.compare_exchange_strong(expected, now, std::memory_order_relaxed))
            return;
        for (Class& c : classes_) {
            if (c.small || c.retained.load(std::memory_order_relaxed) == 0)
                continue;
            for (Shard& shard : c.shards) {
                if (shard.idle.load(std::memory_order_relaxed) == 0)
                    continue;
                std::lock_guard<std::mutex> lock(shard.mutex);
                for (FreeBlock* b = shard.free; b; b = b->next) {
                    if (b->pagesReleased || now - b->idleSinceNs < after)
                        continue;
                    ::madvise(reinterpret_cast<uint8_t*>(b) + kPage, c.bytes - kPage, MADV_DONTNEED);
                    b->pagesReleased = true;
                }
            }
        }
    }

    uint8_t* map(size_t bytes) {
        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            throw std::bad_alloc();
        mapCalls_.fetch_add(1, std::memory_order_relaxed);
        mappedBytes_.fetch_add(bytes, std::memory_order_relaxed);
        return static_cast<uint8_t*>(p);
    }

    void unmap(uint8_t* p, size_t bytes) noexcept {
        ::munmap(p, bytes);
        unmapCalls_.fetch_add(1, std::memory_order_relaxed);
        mappedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }

    // Called with c.carveMutex held.
    uint8_t* carve(Class& c) {
        if (c.cursor == c.limit) {
            const size_t bytes = options_.slabBytes;
            uint8_t* memory = map(bytes);
            {
                std::lock_guard<std::mutex> lock(slabsMutex_);
                slabs_ = new Slab{memory, bytes, slabs_};
            }
            c.cursor = memory;
            c.limit = memory + bytes / c.bytes * c.bytes;
        }
        uint8_t* p = c.cursor;
        c.cursor += c.bytes;
        return p;
    }

    Options options_;
    std::array<Class, kClasses> classes_;
    std::mutex slabsMutex_;
    Slab* slabs_ = nullptr;
    std::atomic<size_t> mappedBytes_{0};
    std::atomic<size_t> retainedBytes_{0};
    std::atomic<size_t> hugeInUse_{0};
    std::atomic<int64_t> lastIdleCheckNs_{0};
    std::atomic<uint64_t> mapCalls_{0};
    std::atomic<uint64_t> unmapCalls_{0};
};

inline void Buffer::reset() noexcept {
    if (data_)
        pool_->release(data_, capacity_);
    pool_ = nullptr;
    data_ = nullptr;
    capacity_ = 0;
}

} // namespace bufs
