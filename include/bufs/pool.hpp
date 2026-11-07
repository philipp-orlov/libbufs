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

#include "mem/size_class_pool.hpp"
#include "sys/vm.hpp"

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

// The pool itself is libcore's mem::SizeClassPool (sharded slabs with per-thread caches for the small classes,
// per-block mappings with idle-page release for the large ones, misuse detection, an off-heap bookkeeping); this class
// keeps libbufs's interface and its one extra behaviour, blocks above the largest class mapped per use.
class BufferPool {
public:
    struct Options {
        // Idle large blocks kept for reuse, in total; more are unmapped on release.
        size_t maxRetainedLargeBytes = size_t{64} << 20;
        // Idle blocks kept per large class.
        size_t maxRetainedPerLargeClass = 16;
        // An idle large block that stays unused this long gives its pages back (MADV_DONTNEED) and keeps only its
        // address range: while outliers keep coming their blocks stay warm, and once they stop the memory returns
        // to the OS. Checked on large-block releases, at most every 50 ms.
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
    explicit BufferPool(Options options) : pool_(convert(options)) {}
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // The process-wide pool; lives until exit.
    static BufferPool& global() {
        static BufferPool* pool = new BufferPool();
        return *pool;
    }

    // A block of at least `minBytes`, 64-byte aligned. Its contents are whatever the previous user left. Throws
    // std::bad_alloc when the OS refuses the memory.
    Buffer acquire(size_t minBytes) {
        const size_t want = std::max(minBytes, kMinClassBytes);
        if (want > kMaxClassBytes) {
            const sys::VmRegion r = sys::tryVmMap(want);
            if (!r.base)
                throw std::bad_alloc();
            hugeInUse_.fetch_add(1, std::memory_order_relaxed);
            hugeMapped_.fetch_add(r.bytes, std::memory_order_relaxed);
            hugeCalls_.fetch_add(1, std::memory_order_relaxed);
            return Buffer(this, static_cast<uint8_t*>(r.base), r.bytes);
        }
        const mem::SizeClassPool::Block b = pool_.acquire(want);
        return Buffer(this, static_cast<uint8_t*>(b.data), b.capacity);
    }

    // Returns a block taken from acquire(). Prefer letting the Buffer do it.
    void release(uint8_t* data, size_t capacity) noexcept {
        if (capacity > kMaxClassBytes) {
            hugeInUse_.fetch_sub(1, std::memory_order_relaxed);
            hugeMapped_.fetch_sub(capacity, std::memory_order_relaxed);
            hugeUnmaps_.fetch_add(1, std::memory_order_relaxed);
            sys::vmUnmap(data, capacity);
            return;
        }
        pool_.release({data, capacity});
    }

    // Unmaps every idle large block. Small-class slabs stay mapped.
    void trim() noexcept { pool_.releaseIdleLarge(); }

    Stats stats() const {
        const mem::Stats s = pool_.stats();
        Stats out;
        out.mappedBytes = static_cast<size_t>(s.mappedBytes) + hugeMapped_.load(std::memory_order_relaxed);
        out.retainedBytes = static_cast<size_t>(pool_.idleLargeBytes());
        out.blocksInUse = static_cast<size_t>(s.blocksInUse) + hugeInUse_.load(std::memory_order_relaxed);
        out.mapCalls = s.mapCalls + hugeCalls_.load(std::memory_order_relaxed);
        out.unmapCalls = s.unmapCalls + hugeUnmaps_.load(std::memory_order_relaxed);
        return out;
    }

    // The capacity acquire(bytes) hands out.
    static size_t classBytes(size_t bytes) {
        const size_t want = std::max(bytes, kMinClassBytes);
        if (want > kMaxClassBytes)
            return (want + 4095) / 4096 * 4096;
        size_t c = kMinClassBytes;
        while (c < want)
            c <<= 1;
        return c;
    }

private:
    static mem::PoolOptions convert(const Options& o) {
        mem::PoolOptions p;
        p.classes = mem::ClassTable::pow2(kMinClassBytes, kMaxClassBytes);
        p.smallLimit = kMaxSmallBytes;
        p.slabBytes = std::max(o.slabBytes, kMaxSmallBytes);
        p.retainLargeBytes = o.maxRetainedLargeBytes;
        p.maxIdlePerLargeClass = o.maxRetainedPerLargeClass;
        p.releaseIdleAfter = o.releasePagesAfter;
        p.hooks.abortOnMisuse = false;
        return p;
    }
    mem::SizeClassPool pool_;
    std::atomic<size_t> hugeInUse_{0}, hugeMapped_{0};
    std::atomic<uint64_t> hugeCalls_{0}, hugeUnmaps_{0};
};

inline void Buffer::reset() noexcept {
    if (data_)
        pool_->release(data_, capacity_);
    pool_ = nullptr;
    data_ = nullptr;
    capacity_ = 0;
}

} // namespace bufs
