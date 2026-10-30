// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Building a message front to back. The root goes first, then every child
// after its parent, so each reference is a forward offset written once.
//
// A builder writes either into pooled memory, which grows by moving to a
// larger block (one copy, then the block size is remembered per root type),
// or into memory the caller owns (a shared-memory ring slot, a stack array),
// which never moves and throws CapacityError when full.
//
// Builder handles hold offsets, not pointers, so they stay valid when a pooled
// buffer grows. Raw pointers from data() or ListBuilder::data() are valid only
// until the next allocation in the same message.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "bufs/format.hpp"
#include "bufs/pool.hpp"

namespace bufs {

// Passed to a generated initX(count, bufs::uninitialized): the vector's
// elements are left as they were in the pooled block, so the caller must write
// every one of them. Skips a memset the caller would overwrite anyway.
struct Uninitialized {};
constexpr Uninitialized uninitialized{};

// Upper bounds on the bytes an object adds to a message, alignment padding
// included, for sizing a block so a builder never has to grow.
constexpr size_t vectorBytes(size_t count, size_t elementBytes, size_t elementAlign) {
    return align4(4 + count * elementBytes) + (elementAlign >= 8 ? 4 : 0);
}
constexpr size_t stringBytes(size_t length) { return vectorBytes(length + 1, 1, 1); }
constexpr size_t tableBytes(uint16_t dataBytes, uint16_t refCount) {
    return align4(dataBytes) + 4 * size_t{refCount} + 4;
}

// A finished message that owns its pooled block.
class Message {
public:
    Message() = default;
    Message(Buffer buffer, size_t size) : buffer_(std::move(buffer)), size_(size) {}

    const uint8_t* data() const { return buffer_.data(); }
    uint8_t* data() { return buffer_.data(); }
    size_t size() const { return size_; }
    uint32_t typeId() const { return size_ >= kHeaderBytes ? messageType(buffer_.data()) : 0; }
    explicit operator bool() const { return size_ != 0; }
    void reset() {
        buffer_.reset();
        size_ = 0;
    }

private:
    Buffer buffer_;
    size_t size_ = 0;
};

namespace detail {

// Per root type: the largest message built so far, so the next builder of
// that type starts with a block it will not outgrow.
template <class T>
std::atomic<uint32_t>& sizeHint() {
    static std::atomic<uint32_t> hint{0};
    return hint;
}

} // namespace detail

class MessageBuilder {
public:
    // Builds into blocks from `pool`; `initialBytes` is a first guess, which the
    // root type's remembered size overrides when larger.
    explicit MessageBuilder(BufferPool& pool = BufferPool::global(), size_t initialBytes = 0)
        : pool_(&pool), initialBytes_(initialBytes) {}

    // Builds into `memory` (8-byte aligned, `capacity` bytes), which is never
    // grown or freed. Throws CapacityError when the message does not fit.
    MessageBuilder(void* memory, size_t capacity)
        : data_(static_cast<uint8_t*>(memory)), capacity_(std::min(capacity, kMaxMessageBytes)) {}

    MessageBuilder(const MessageBuilder&) = delete;
    MessageBuilder& operator=(const MessageBuilder&) = delete;

    // Starts a message whose root is the table `T`. Must be the first call.
    template <class T>
    typename T::Builder initRoot() {
        begin(T::typeId, &detail::sizeHint<T>());
        const uint32_t root = allocateTable(T::kDataBytes, T::kRefCount);
        return typename T::Builder(*this, root);
    }

    // Starts a message whose root is the struct `S`, all zero, and returns it.
    // The reference stays valid: nothing else is ever added to such a message.
    template <class S>
    S& initRootStruct() {
        begin(StructInfo<S>::typeId, &detail::sizeHint<S>());
        const uint32_t root = allocate(sizeof(S), 8, 0);
        return *reinterpret_cast<S*>(data_ + root);
    }

    // Seals the header and returns the bytes, still owned by the builder.
    std::pair<const uint8_t*, size_t> finish() {
        const uint32_t end = allocate(0, 8, 0);
        store<uint32_t>(data_, end);
        // Remembered only up to the small-block limit: one outlier must not
        // make every later message of its type start with a huge block.
        const uint32_t remembered = static_cast<uint32_t>(std::min<size_t>(end, BufferPool::kMaxSmallBytes));
        // A maximum: a smaller message finishing later on another thread must not overwrite a larger one's.
        if (hint_) {
            uint32_t seen = hint_->load(std::memory_order_relaxed);
            while (seen < remembered && !hint_->compare_exchange_weak(seen, remembered, std::memory_order_relaxed)) {
            }
        }
        return {data_, end};
    }

    // Seals a pooled message and hands its block over. The builder is empty
    // afterwards and can start another message.
    Message release() {
        if (!pool_)
            throw std::logic_error("bufs: release() needs a pooled builder");
        const size_t size = finish().second;
        data_ = nullptr;
        capacity_ = 0;
        used_ = 0;
        return Message(std::move(buffer_), size);
    }

    // Drops what was built, keeping the block for the next message.
    void clear() { used_ = 0; }

    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return used_; }

    // ------------------------------------------------ used by generated code

    // `bytes` at an offset that is `phase` mod `align` (align 4 or 8, phase 0
    // or 4), zeroed with any padding before it unless `zero` is false (the
    // padding is always zeroed: pooled memory must not leak into a message).
    uint32_t allocate(size_t bytes, size_t align, size_t phase, bool zero = true) {
        size_t at = used_;
        while (at % align != phase)
            at += 4;
        const size_t end = align4(at + bytes);
        if (end > capacity_)
            grow(end);
        if (zero) {
            std::memset(data_ + used_, 0, end - used_);
        } else {
            std::memset(data_ + used_, 0, at - used_);
            std::memset(data_ + at + bytes, 0, end - at - bytes);
        }
        used_ = end;
        return static_cast<uint32_t>(at);
    }

    uint32_t allocateTable(uint16_t dataBytes, uint16_t refCount) {
        const uint32_t at = allocate(align4(dataBytes) + 4 * size_t{refCount}, 8, 0);
        store<uint16_t>(data_ + at, dataBytes);
        store<uint16_t>(data_ + at + 2, refCount);
        return at;
    }

    // A vector of `count` elements of `elementBytes` each; 8-byte elements are
    // 8-aligned. Zeroed unless `zero` is false.
    uint32_t allocateVector(size_t count, size_t elementBytes, size_t elementAlign, bool zero = true) {
        const size_t bytes = 4 + count * elementBytes;
        if (count > kMaxMessageBytes / std::max<size_t>(elementBytes, 1) || bytes > kMaxMessageBytes)
            throw CapacityError("bufs: vector too large");
        const uint32_t at = elementAlign == 8 ? allocate(bytes, 8, 4, zero) : allocate(bytes, 4, 0, zero);
        store<uint32_t>(data_ + at, static_cast<uint32_t>(count));
        return at;
    }

    uint32_t createString(std::string_view text) {
        const uint32_t at = allocateVector(text.size() + 1, 1, 1, false);
        std::memcpy(data_ + at + 4, text.data(), text.size());
        data_[at + 4 + text.size()] = 0;
        store<uint32_t>(data_ + at, static_cast<uint32_t>(text.size()));
        return at;
    }

    // Points the reference slot at `slot` to the object at `target` (later in
    // the message).
    void setRef(uint32_t slot, uint32_t target) { store<uint32_t>(data_ + slot, target - slot); }

    // The slot of reference `index` in the table at `table`.
    static uint32_t refSlot(uint32_t table, uint16_t dataBytes, uint32_t index) {
        return table + static_cast<uint32_t>(align4(dataBytes)) + 4 * index;
    }

private:
    void begin(uint32_t typeId, std::atomic<uint32_t>* hint) {
        used_ = 0;
        hint_ = hint;
        if (pool_) {
            const size_t want = std::max<size_t>({initialBytes_, hint->load(std::memory_order_relaxed), 64});
            if (capacity_ < want) {
                buffer_ = pool_->acquire(want);
                data_ = buffer_.data();
                capacity_ = std::min(buffer_.capacity(), kMaxMessageBytes);
            }
        } else if (capacity_ < kHeaderBytes) {
            throw CapacityError("bufs: buffer smaller than a message header");
        }
        allocate(kHeaderBytes, 8, 0);
        store<uint32_t>(data_ + 4, typeId);
    }

    void grow(size_t needed) {
        if (!pool_ || needed > kMaxMessageBytes)
            throw CapacityError(pool_ ? "bufs: message larger than 4 GiB" : "bufs: message does not fit its buffer");
        Buffer bigger = pool_->acquire(std::max(needed, capacity_ * 2));
        std::memcpy(bigger.data(), data_, used_);
        buffer_ = std::move(bigger);
        data_ = buffer_.data();
        capacity_ = std::min(buffer_.capacity(), kMaxMessageBytes);
    }

    BufferPool* pool_ = nullptr;
    Buffer buffer_;
    uint8_t* data_ = nullptr;
    size_t capacity_ = 0;
    size_t used_ = 0;
    size_t initialBytes_ = 0;
    std::atomic<uint32_t>* hint_ = nullptr;
};

// Fills a vector of scalars, enums or structs in place.
template <class T>
class ListBuilder {
public:
    ListBuilder(MessageBuilder& mb, uint32_t object) : mb_(&mb), object_(object) {}

    uint32_t size() const { return load<uint32_t>(mb_->data() + object_); }
    void set(size_t i, const T& value) { std::memcpy(mb_->data() + object_ + 4 + i * sizeof(T), &value, sizeof(T)); }
    // Valid until the next allocation in the message.
    T* data() { return reinterpret_cast<T*>(mb_->data() + object_ + 4); }

private:
    MessageBuilder* mb_;
    uint32_t object_;
};

template <>
class ListBuilder<bool> {
public:
    ListBuilder(MessageBuilder& mb, uint32_t object) : mb_(&mb), object_(object) {}

    uint32_t size() const { return load<uint32_t>(mb_->data() + object_); }
    void set(size_t i, bool value) { mb_->data()[object_ + 4 + i] = value ? 1 : 0; }

private:
    MessageBuilder* mb_;
    uint32_t object_;
};

class StringListBuilder {
public:
    StringListBuilder(MessageBuilder& mb, uint32_t object) : mb_(&mb), object_(object) {}

    uint32_t size() const { return load<uint32_t>(mb_->data() + object_); }
    void set(size_t i, std::string_view text) {
        const uint32_t target = mb_->createString(text);
        mb_->setRef(object_ + 4 + static_cast<uint32_t>(4 * i), target);
    }

private:
    MessageBuilder* mb_;
    uint32_t object_;
};

// Fills a vector of tables; `Builder` is the table's generated Builder.
template <class Builder>
class TableListBuilder {
public:
    TableListBuilder(MessageBuilder& mb, uint32_t object) : mb_(&mb), object_(object) {}

    uint32_t size() const { return load<uint32_t>(mb_->data() + object_); }
    // Adds element i's table; call each index once.
    Builder init(size_t i) {
        const uint32_t target = mb_->allocateTable(Builder::Table::kDataBytes, Builder::Table::kRefCount);
        mb_->setRef(object_ + 4 + static_cast<uint32_t>(4 * i), target);
        return Builder(*mb_, target);
    }

private:
    MessageBuilder* mb_;
    uint32_t object_;
};

namespace detail {

template <class T>
uint32_t createVector(MessageBuilder& mb, const T* values, size_t count) {
    const uint32_t at = mb.allocateVector(count, sizeof(T), alignof(T) >= 8 ? 8 : 4, false);
    if (count)
        std::memcpy(mb.data() + at + 4, values, count * sizeof(T));
    return at;
}

inline uint32_t createBoolVector(MessageBuilder& mb, const bool* values, size_t count) {
    const uint32_t at = mb.allocateVector(count, 1, 1, false);
    for (size_t i = 0; i < count; ++i)
        mb.data()[at + 4 + i] = values[i] ? 1 : 0;
    return at;
}

} // namespace detail

} // namespace bufs
