// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Heap accounting: malloc and friends are interposed (they forward to glibc's
// __libc_* entry points) and operator new is replaced, so every heap
// allocation in the process -- libstdc++, FlatBuffers' default allocator,
// std::string -- is counted while heap::counting is on. libbufs' pool maps
// its memory with mmap, which this does not see; the benchmark reports the
// pool's own mmap count next to it.

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <new>

#include "bench_common.hpp"

namespace bd::heap {
std::atomic<bool> counting{false};
std::atomic<uint64_t> allocations{0};
std::atomic<uint64_t> bytes{0};
std::atomic<uint64_t> newCalls{0};
} // namespace bd::heap

namespace {

inline void count(size_t size) {
    if (bd::heap::counting.load(std::memory_order_relaxed)) {
        bd::heap::allocations.fetch_add(1, std::memory_order_relaxed);
        bd::heap::bytes.fetch_add(size, std::memory_order_relaxed);
    }
}

} // namespace

#if defined(__GLIBC__)
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* __libc_memalign(size_t, size_t);
void __libc_free(void*);

void* malloc(size_t size) {
    count(size);
    return __libc_malloc(size);
}
void* calloc(size_t n, size_t size) {
    count(n * size);
    return __libc_calloc(n, size);
}
void* realloc(void* p, size_t size) {
    count(size);
    return __libc_realloc(p, size);
}
void* memalign(size_t alignment, size_t size) {
    count(size);
    return __libc_memalign(alignment, size);
}
void* aligned_alloc(size_t alignment, size_t size) {
    count(size);
    return __libc_memalign(alignment, size);
}
int posix_memalign(void** out, size_t alignment, size_t size) {
    count(size);
    void* p = __libc_memalign(alignment, size);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}
void free(void* p) { __libc_free(p); }
}
#endif

void* operator new(std::size_t size) {
    if (bd::heap::counting.load(std::memory_order_relaxed))
        bd::heap::newCalls.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return operator new(size, std::nothrow); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
