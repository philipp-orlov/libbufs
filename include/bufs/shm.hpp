// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Shared memory as the transport. Linux only (futex).
//
// Memory  an anonymous shared-memory file (memfd), sealed so it can never be
//         resized, mapped here; its descriptor goes to the other process over
//         a local socket (bufs/local.hpp). Nothing appears in /dev/shm, and
//         the kernel frees it when the last process closes it.
// Region  a block with a small header: anonymous (Memory) or a named POSIX
//         shared-memory object, which the creator unlinks when done.
// Ring    a single-producer, single-consumer queue of variable-size messages
//         inside any shared memory. A message is always contiguous, so a
//         MessageBuilder can build straight into reserve()'s slot and the
//         consumer can read it in place: no copy on either side. Waiting
//         uses a futex in the shared memory, so it works across processes.
// Channel a Region holding two Rings, one each way, for a request/reply
//         pair of processes.
//
// Prefer anonymous memory: a peer cannot name another process's memory, and
// the seals mean a peer cannot shrink the file under a mapping (reading past
// the end of a shrunk file kills the reader with SIGBUS).
//
// The peer is another process. Ring positions and record headers from the
// shared memory are validated before use, and a corrupt ring throws
// CorruptRing. Message bytes can still change while they are read, so a
// consumer that does not trust its producer should copy a message out
// (it is small) and verify the copy, not the original.

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "bufs/fd.hpp"
#include "bufs/format.hpp"

namespace bufs::shm {

class CorruptRing : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free,
              "shared-memory rings need lock-free atomics");

inline void futexWait(std::atomic<uint32_t>* word, uint32_t expected, std::chrono::nanoseconds timeout) {
    timespec ts{};
    timespec* pts = nullptr;
    if (timeout.count() >= 0) {
        ts.tv_sec = static_cast<time_t>(timeout.count() / 1000000000);
        ts.tv_nsec = static_cast<long>(timeout.count() % 1000000000);
        pts = &ts;
    }
    // Not FUTEX_PRIVATE: the word lives in memory other processes map.
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAIT, expected, pts, nullptr, 0);
}

inline void futexWake(std::atomic<uint32_t>* word) {
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Polls `ready` for up to `spin` before the caller sleeps on a futex. A futex
// round trip between processes costs tens of microseconds; a peer that answers
// within the spin is seen in about one.
template <class Ready>
bool spinFor(std::chrono::nanoseconds spin, Ready ready) {
    if (spin.count() <= 0)
        return false;
    const auto until = std::chrono::steady_clock::now() + spin;
    for (int i = 0;; ++i) {
        if (ready())
            return true;
        cpuRelax();
        if ((i & 63) == 63 && std::chrono::steady_clock::now() >= until)
            return false;
    }
}

} // namespace detail

// ------------------------------------------------------------------ Memory --

// `bytes` of anonymous shared memory, zeroed, mapped read-write. Created here
// (create) or received from another process (adopt). The descriptor stays
// open, so it can be passed on with bufs::local::Socket.
class Memory {
public:
    // The seals every Memory carries; adopt() requires them.
    static constexpr int kSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;

    Memory() = default;
    ~Memory() { unmap(); }
    Memory(Memory&& other) noexcept
        : fd_(std::move(other.fd_)), data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}
    Memory& operator=(Memory&& other) noexcept {
        if (this != &other) {
            unmap();
            fd_ = std::move(other.fd_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    // `name` only labels the file in /proc/<pid>/fd and maps.
    static Memory create(const char* name, size_t bytes) {
        if (bytes == 0)
            throw std::invalid_argument("bufs: empty shared memory");
        Fd fd(::memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (!fd)
            throw std::system_error(errno, std::generic_category(), "memfd_create");
        if (::ftruncate(fd.get(), static_cast<off_t>(bytes)) != 0)
            throw std::system_error(errno, std::generic_category(), "ftruncate");
        if (::fcntl(fd.get(), F_ADD_SEALS, kSeals) != 0)
            throw std::system_error(errno, std::generic_category(), "F_ADD_SEALS");
        return map(std::move(fd), bytes);
    }

    // Maps memory another process created with create() (or sealed the same
    // way). Refuses anything else: an unsealed file could be shrunk under the
    // mapping. `maxBytes` bounds what a peer can make this process map.
    static Memory adopt(Fd fd, size_t maxBytes = size_t{1} << 32) {
        if (!fd)
            throw std::invalid_argument("bufs: no descriptor");
        const int seals = ::fcntl(fd.get(), F_GET_SEALS);
        if (seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW))
            throw std::invalid_argument("bufs: shared memory is not sealed against resizing");
        struct stat st {};
        if (::fstat(fd.get(), &st) != 0 || st.st_size <= 0)
            throw std::invalid_argument("bufs: shared memory has no size");
        const size_t bytes = static_cast<size_t>(st.st_size);
        if (bytes > maxBytes)
            throw std::invalid_argument("bufs: shared memory is larger than allowed");
        return map(std::move(fd), bytes);
    }

    uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    int fd() const { return fd_.get(); }
    explicit operator bool() const { return data_ != nullptr; }

private:
    static Memory map(Fd fd, size_t bytes) {
        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
        if (p == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "mmap");
        Memory memory;
        memory.fd_ = std::move(fd);
        memory.data_ = static_cast<uint8_t*>(p);
        memory.size_ = bytes;
        return memory;
    }

    void unmap() noexcept {
        if (data_)
            ::munmap(data_, size_);
        data_ = nullptr;
        size_ = 0;
        fd_.reset();
    }

    Fd fd_;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

// ------------------------------------------------------------------ Region --

class Region {
public:
    static constexpr uint64_t kMagic = 0x0131485353465542ull; // "BUFSSH1\1"
    static constexpr size_t kHeaderBytes = 64;

    Region() = default;
    ~Region() { close(); }
    Region(Region&& other) noexcept { *this = std::move(other); }
    Region& operator=(Region&& other) noexcept {
        if (this != &other) {
            close();
            name_ = std::move(other.name_);
            memory_ = std::move(other.memory_);
            base_ = std::exchange(other.base_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
            owner_ = std::exchange(other.owner_, false);
        }
        return *this;
    }
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    // Creates `name` ("/something") with `bytes` usable bytes after the
    // header. Fails if it exists. The new region is zeroed.
    static Region create(const std::string& name, size_t bytes) {
        const size_t total = kHeaderBytes + bytes;
        const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "shm_open " + name);
        if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
            const int error = errno;
            ::close(fd);
            ::shm_unlink(name.c_str());
            throw std::system_error(error, std::generic_category(), "ftruncate " + name);
        }
        void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        const int error = errno;
        ::close(fd);
        if (p == MAP_FAILED) {
            ::shm_unlink(name.c_str());
            throw std::system_error(error, std::generic_category(), "mmap " + name);
        }
        Region region;
        region.name_ = name;
        region.base_ = static_cast<uint8_t*>(p);
        region.bytes_ = total;
        region.owner_ = true;
        store<uint64_t>(region.base_ + 8, total);
        std::atomic_thread_fence(std::memory_order_release);
        store<uint64_t>(region.base_, kMagic);
        return region;
    }

    // An anonymous region (Memory) with `bytes` usable bytes after the header.
    static Region createAnonymous(const char* name, size_t bytes) {
        Region region;
        region.memory_ = Memory::create(name, kHeaderBytes + bytes);
        region.base_ = region.memory_.data();
        region.bytes_ = region.memory_.size();
        store<uint64_t>(region.base_ + 8, region.bytes_);
        std::atomic_thread_fence(std::memory_order_release);
        store<uint64_t>(region.base_, kMagic);
        return region;
    }

    // Maps an anonymous region another process created and sent; checks the
    // seals (Memory::adopt) and the header.
    static Region adopt(Fd fd, size_t maxBytes = size_t{1} << 32) {
        Region region;
        region.memory_ = Memory::adopt(std::move(fd), maxBytes);
        region.base_ = region.memory_.data();
        region.bytes_ = region.memory_.size();
        if (region.bytes_ < kHeaderBytes || load<uint64_t>(region.base_) != kMagic ||
            load<uint64_t>(region.base_ + 8) != region.bytes_)
            throw std::runtime_error("bufs: not a libbufs region");
        return region;
    }

    // Maps an existing region; checks its header against its file size.
    static Region open(const std::string& name) {
        const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "shm_open " + name);
        struct stat st {};
        if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < kHeaderBytes) {
            ::close(fd);
            throw std::runtime_error("bufs: " + name + " is not a libbufs region");
        }
        const size_t total = static_cast<size_t>(st.st_size);
        void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        const int error = errno;
        ::close(fd);
        if (p == MAP_FAILED)
            throw std::system_error(error, std::generic_category(), "mmap " + name);
        Region region;
        region.name_ = name;
        region.base_ = static_cast<uint8_t*>(p);
        region.bytes_ = total;
        if (load<uint64_t>(region.base_) != kMagic || load<uint64_t>(region.base_ + 8) != total)
            throw std::runtime_error("bufs: " + name + " is not a libbufs region");
        return region;
    }

    // The usable bytes, after the header; 64-byte aligned.
    uint8_t* data() const { return base_ ? base_ + kHeaderBytes : nullptr; }
    size_t size() const { return bytes_ ? bytes_ - kHeaderBytes : 0; }
    // Empty for an anonymous region.
    const std::string& name() const { return name_; }
    bool owner() const { return owner_; }
    // An anonymous region's descriptor, to send to the other process; -1 for
    // a named one.
    int fd() const { return memory_.fd(); }
    explicit operator bool() const { return base_ != nullptr; }

    void close() noexcept {
        if (memory_)
            memory_ = Memory();
        else if (base_)
            ::munmap(base_, bytes_);
        if (owner_)
            ::shm_unlink(name_.c_str());
        base_ = nullptr;
        bytes_ = 0;
        owner_ = false;
    }

private:
    std::string name_;
    Memory memory_; // anonymous regions only
    uint8_t* base_ = nullptr;
    size_t bytes_ = 0;
    bool owner_ = false;
};

// -------------------------------------------------------------------- Ring --

class Ring {
public:
    static constexpr uint64_t kMagic = 0x01474e4953465542ull; // "BUFSING\1"
    static constexpr uint32_t kMessage = 1;
    static constexpr uint32_t kPadding = 2;

    // Bytes a ring with `capacity` bytes of messages takes in shared memory.
    static constexpr size_t bytesFor(size_t capacity) { return sizeof(Control) + capacity; }

    Ring() = default;

    // Lays out a new, empty ring in `memory` (64-byte aligned, bytesFor()
    // bytes). `capacity` is a power of two, at least 256.
    static Ring initialize(void* memory, size_t capacity) {
        if (capacity < 256 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("bufs: ring capacity must be a power of two >= 256");
        Ring ring;
        ring.control_ = new (memory) Control();
        ring.control_->capacity = capacity;
        ring.data_ = static_cast<uint8_t*>(memory) + sizeof(Control);
        ring.capacity_ = capacity;
        std::atomic_thread_fence(std::memory_order_release);
        ring.control_->magic = kMagic;
        return ring;
    }

    // Uses a ring another process laid out in `memory` (at most `available`
    // bytes). Checks the header; a ring that does not fit throws CorruptRing.
    static Ring attach(void* memory, size_t available) {
        if (available < sizeof(Control))
            throw CorruptRing("bufs: no room for a ring");
        Ring ring;
        ring.control_ = static_cast<Control*>(memory);
        const uint64_t capacity = ring.control_->capacity;
        if (ring.control_->magic != kMagic || capacity < 256 || (capacity & (capacity - 1)) != 0 ||
            capacity > available - sizeof(Control))
            throw CorruptRing("bufs: not a ring");
        ring.data_ = static_cast<uint8_t*>(memory) + sizeof(Control);
        ring.capacity_ = capacity;
        return ring;
    }

    size_t capacity() const { return capacity_; }
    // The largest message reserve() accepts: half the ring, less its header.
    size_t maxMessageBytes() const { return capacity_ / 2 - 8; }
    explicit operator bool() const { return control_ != nullptr; }

    // ---------------------------------------------------------- producer --

    // A contiguous, 8-aligned slot of at least `bytes`, or nullptr when the
    // consumer has not freed enough yet. Throws CapacityError when `bytes`
    // can never fit. Publish it with commit(), or drop it with cancel().
    uint8_t* reserve(size_t bytes) {
        if (bytes > maxMessageBytes())
            throw CapacityError("bufs: message larger than half the ring");
        const uint64_t pos = control_->writePos.load(std::memory_order_relaxed);
        const uint64_t read = control_->readPos.load(std::memory_order_acquire);
        // Each position's own alignment, not just their difference: a peer
        // that corrupts both by the same misalignment would otherwise pass
        // the `used` check below and index this ring out of bounds.
        if (pos % 8 != 0 || read % 8 != 0)
            throw CorruptRing("bufs: ring position is not aligned");
        const uint64_t index = pos & (capacity_ - 1);
        const uint64_t need = 8 + align8(bytes);
        const uint64_t tail = capacity_ - index;
        const uint64_t skip = need > tail ? tail : 0;
        const uint64_t used = pos - read;
        if (used > capacity_ || capacity_ - used < skip + need)
            return nullptr;
        if (skip)
            writeHeader(index, static_cast<uint32_t>(tail - 8), kPadding);
        reservedPos_ = pos + skip;
        reservedBytes_ = bytes;
        return data_ + (reservedPos_ & (capacity_ - 1)) + 8;
    }

    // reserve(), waiting up to `timeout` for room (forever when negative):
    // polling for up to `spin` first, then sleeping on the futex.
    uint8_t* reserve(size_t bytes, std::chrono::nanoseconds timeout,
                     std::chrono::nanoseconds spin = std::chrono::nanoseconds(0)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        uint8_t* spun = nullptr;
        if (detail::spinFor(spin, [&] { return (spun = reserve(bytes)) != nullptr; }))
            return spun;
        for (;;) {
            control_->producerWaiting.store(1, std::memory_order_seq_cst);
            const uint32_t seq = control_->readSeq.load(std::memory_order_seq_cst);
            if (uint8_t* slot = reserve(bytes)) {
                control_->producerWaiting.store(0, std::memory_order_relaxed);
                return slot;
            }
            const auto left = deadline - std::chrono::steady_clock::now();
            if (timeout.count() >= 0 && left.count() <= 0) {
                control_->producerWaiting.store(0, std::memory_order_relaxed);
                return nullptr;
            }
            detail::futexWait(&control_->readSeq, seq,
                              timeout.count() >= 0 ? std::chrono::nanoseconds(left) : std::chrono::nanoseconds(-1));
        }
    }

    // Publishes the reserved slot holding `bytes` (at most what was reserved).
    void commit(size_t bytes) {
        if (bytes > reservedBytes_)
            throw std::logic_error("bufs: commit() larger than reserve()");
        writeHeader(reservedPos_ & (capacity_ - 1), static_cast<uint32_t>(bytes), kMessage);
        control_->writePos.store(reservedPos_ + 8 + align8(bytes), std::memory_order_release);
        control_->writeSeq.fetch_add(1, std::memory_order_seq_cst);
        if (control_->consumerWaiting.load(std::memory_order_seq_cst))
            detail::futexWake(&control_->writeSeq);
        reservedBytes_ = 0;
    }

    void cancel() { reservedBytes_ = 0; }

    // Copies `bytes` in as one message; false when there is no room now.
    bool push(const void* bytes, size_t size) {
        uint8_t* slot = reserve(size);
        if (!slot)
            return false;
        std::memcpy(slot, bytes, size);
        commit(size);
        return true;
    }

    // ---------------------------------------------------------- consumer --

    // The oldest message, read in place, or false when the ring is empty.
    // It stays valid until pop().
    bool front(const uint8_t*& data, size_t& size) {
        uint64_t pos = control_->readPos.load(std::memory_order_relaxed);
        for (;;) {
            const uint64_t end = control_->writePos.load(std::memory_order_acquire);
            if (end == pos)
                return false;
            // Each position's own alignment, not just their difference: a
            // peer that corrupts both by the same misalignment would
            // otherwise pass this check and index this ring out of bounds.
            if (pos % 8 != 0 || end % 8 != 0 || end - pos > capacity_ || (end - pos) % 8 != 0)
                throw CorruptRing("bufs: ring positions are inconsistent");
            const uint64_t index = pos & (capacity_ - 1);
            const uint32_t length = load<uint32_t>(data_ + index);
            const uint32_t kind = load<uint32_t>(data_ + index + 4);
            const uint64_t tail = capacity_ - index;
            if (kind == kPadding) {
                if (length != tail - 8 || tail > end - pos)
                    throw CorruptRing("bufs: bad padding record");
                pos += tail;
                control_->readPos.store(pos, std::memory_order_release);
                continue;
            }
            if (kind != kMessage || length > tail - 8 || 8 + align8(length) > end - pos)
                throw CorruptRing("bufs: bad message record");
            data = data_ + index + 8;
            size = length;
            frontBytes_ = 8 + align8(length);
            return true;
        }
    }

    // Frees the message front() returned.
    void pop() {
        if (!frontBytes_)
            throw std::logic_error("bufs: pop() without front()");
        const uint64_t pos = control_->readPos.load(std::memory_order_relaxed) + frontBytes_;
        frontBytes_ = 0;
        control_->readPos.store(pos, std::memory_order_release);
        control_->readSeq.fetch_add(1, std::memory_order_seq_cst);
        if (control_->producerWaiting.load(std::memory_order_seq_cst))
            detail::futexWake(&control_->readSeq);
    }

    bool empty() const {
        return control_->readPos.load(std::memory_order_relaxed) ==
               control_->writePos.load(std::memory_order_acquire);
    }

    // Waits until a message is there, up to `timeout` (forever when negative):
    // polling for up to `spin` first, then sleeping on the futex. True when one
    // is. A spin of tens of microseconds cuts a round trip between processes
    // from about 18 us to about 1.5 us when the peer answers that fast, at the
    // cost of a busy core while it polls.
    bool wait(std::chrono::nanoseconds timeout, std::chrono::nanoseconds spin = std::chrono::nanoseconds(0)) {
        if (detail::spinFor(spin, [&] { return !empty(); }))
            return true;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            control_->consumerWaiting.store(1, std::memory_order_seq_cst);
            const uint32_t seq = control_->writeSeq.load(std::memory_order_seq_cst);
            if (!empty()) {
                control_->consumerWaiting.store(0, std::memory_order_relaxed);
                return true;
            }
            const auto left = deadline - std::chrono::steady_clock::now();
            if (timeout.count() >= 0 && left.count() <= 0) {
                control_->consumerWaiting.store(0, std::memory_order_relaxed);
                return false;
            }
            detail::futexWait(&control_->writeSeq, seq,
                              timeout.count() >= 0 ? std::chrono::nanoseconds(left) : std::chrono::nanoseconds(-1));
        }
    }

private:
    // In the shared memory. The producer's and the consumer's words are on
    // separate cache lines.
    struct Control {
        alignas(64) uint64_t magic = 0;
        uint64_t capacity = 0;
        alignas(64) std::atomic<uint64_t> writePos{0};
        std::atomic<uint32_t> writeSeq{0};
        alignas(64) std::atomic<uint64_t> readPos{0};
        std::atomic<uint32_t> readSeq{0};
        alignas(64) std::atomic<uint32_t> consumerWaiting{0};
        std::atomic<uint32_t> producerWaiting{0};
    };
    static_assert(sizeof(Control) == 256, "ring control block layout");

    void writeHeader(uint64_t index, uint32_t length, uint32_t kind) {
        store<uint32_t>(data_ + index, length);
        store<uint32_t>(data_ + index + 4, kind);
    }

    Control* control_ = nullptr;
    uint8_t* data_ = nullptr;
    uint64_t capacity_ = 0;
    uint64_t reservedPos_ = 0;
    size_t reservedBytes_ = 0;
    uint64_t frontBytes_ = 0;
};

// ----------------------------------------------------------------- Channel --

// Two rings in one region: the creator sends on the first and receives on the
// second, the process that opens it the other way round.
class Channel {
public:
    Channel() = default;

    static Channel create(const std::string& name, size_t capacityEachWay) {
        return layOut(Region::create(name, regionBytes(capacityEachWay)), capacityEachWay);
    }

    static Channel open(const std::string& name) { return attachTo(Region::open(name)); }

    // An anonymous channel; send fd() to the other process, which adopt()s it.
    static Channel createAnonymous(const char* name, size_t capacityEachWay) {
        return layOut(Region::createAnonymous(name, regionBytes(capacityEachWay)), capacityEachWay);
    }

    // The other side of createAnonymous(), from the descriptor it sent.
    static Channel adopt(Fd fd, size_t maxBytes = size_t{1} << 32) {
        return attachTo(Region::adopt(std::move(fd), maxBytes));
    }

    Ring& send() { return send_; }
    Ring& receive() { return receive_; }
    // Empty for an anonymous channel.
    const std::string& name() const { return region_.name(); }
    // An anonymous channel's descriptor; -1 for a named one.
    int fd() const { return region_.fd(); }
    explicit operator bool() const { return static_cast<bool>(region_); }

private:
    static size_t align64(size_t value) { return (value + 63) & ~size_t{63}; }
    static size_t regionBytes(size_t capacityEachWay) { return 64 + 2 * align64(Ring::bytesFor(capacityEachWay)); }

    static Channel layOut(Region region, size_t capacityEachWay) {
        Channel c;
        const size_t ring = align64(Ring::bytesFor(capacityEachWay));
        c.region_ = std::move(region);
        store<uint64_t>(c.region_.data(), ring);
        c.send_ = Ring::initialize(c.region_.data() + 64, capacityEachWay);
        c.receive_ = Ring::initialize(c.region_.data() + 64 + ring, capacityEachWay);
        return c;
    }

    static Channel attachTo(Region region) {
        Channel c;
        c.region_ = std::move(region);
        if (c.region_.size() < 64)
            throw CorruptRing("bufs: not a channel");
        const uint64_t ring = load<uint64_t>(c.region_.data());
        if (ring % 64 != 0 || ring > (c.region_.size() - 64) / 2)
            throw CorruptRing("bufs: not a channel");
        c.receive_ = Ring::attach(c.region_.data() + 64, ring);
        c.send_ = Ring::attach(c.region_.data() + 64 + ring, ring);
        return c;
    }

    Region region_;
    Ring send_;
    Ring receive_;
};

} // namespace bufs::shm
