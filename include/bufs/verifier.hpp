// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Checks a message from an untrusted peer before any accessor reads it. After
// verify<T>() returns true, every accessor of T stays inside the message, every
// string ends in NUL, and every element is aligned for its type. Readers then
// need no further checks.
//
// References only point forward, so there are no cycles; a DAG that shares
// objects could still make a small message expensive to walk, which
// VerifyOptions::maxObjects bounds.

#include <cstddef>
#include <cstdint>

#include "bufs/format.hpp"

namespace bufs {

struct VerifyOptions {
    uint32_t maxDepth = 64;
    uint32_t maxObjects = 1u << 20;
};

class Verifier {
public:
    Verifier(const uint8_t* message, size_t size, VerifyOptions options = {})
        : base_(message), size_(size), options_(options) {}

    // The header: size and alignment, and that the root type is `typeId`.
    bool header(uint32_t typeId) const {
        if (!base_ || size_ < kHeaderBytes || (reinterpret_cast<uintptr_t>(base_) & 7) != 0)
            return false;
        const uint32_t declared = messageSize(base_);
        return declared >= kHeaderBytes && declared <= size_ && declared % 8 == 0 && messageType(base_) == typeId;
    }

    // Shrinks the checked range to the size the header declares.
    void useDeclaredSize() { size_ = messageSize(base_); }

    // A table at `table`: in bounds, 8-aligned, header consistent.
    bool table(const uint8_t* table, uint32_t depth) {
        if (depth > options_.maxDepth || ++objects_ > options_.maxObjects)
            return false;
        const size_t at = offsetOf(table);
        if (at % 8 != 0 || at + kTableHeaderBytes > size_)
            return false;
        const uint16_t data = detail::dataBytes(table);
        const uint16_t refs = detail::refCount(table);
        return data >= kTableHeaderBytes && at + align4(data) + 4 * size_t{refs} <= size_;
    }

    // Reference `index` of a verified table: null, or a forward, 4-aligned
    // offset to somewhere inside the message. `target` gets the object.
    bool ref(const uint8_t* table, uint32_t index, const uint8_t*& target) const {
        target = nullptr;
        if (index >= detail::refCount(table))
            return true;
        return slot(table + align4(detail::dataBytes(table)) + 4 * size_t{index}, target);
    }

    // A vector of `count` elements of `elementBytes`, element-aligned.
    bool vector(const uint8_t* object, size_t elementBytes, size_t elementAlign, uint32_t& count) {
        if (++objects_ > options_.maxObjects)
            return false;
        const size_t at = offsetOf(object);
        if (at % 4 != 0 || at + 4 > size_)
            return false;
        count = load<uint32_t>(object);
        const size_t bytes = size_t{count} * elementBytes;
        if (bytes / (elementBytes ? elementBytes : 1) != count || bytes > size_ - at - 4)
            return false;
        return elementAlign < 8 || (at + 4) % 8 == 0;
    }

    bool string(const uint8_t* object) {
        uint32_t length = 0;
        return vector(object, 1, 1, length) && size_t{length} < size_ - offsetOf(object) - 4 &&
               object[4 + length] == 0;
    }

    // A vector of strings: the vector, then every string in it.
    bool stringVector(const uint8_t* object) {
        uint32_t count = 0;
        if (!vector(object, 4, 4, count) || !charge(count))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* element = nullptr;
            if (!slot(object + 4 + 4 * size_t{i}, element) || (element && !string(element)))
                return false;
        }
        return true;
    }

    // A vector of tables `T`: the vector, then every table in it.
    template <class T>
    bool tableVector(const uint8_t* object, uint32_t depth) {
        uint32_t count = 0;
        if (!vector(object, 4, 4, count) || !charge(count))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* element = nullptr;
            if (!slot(object + 4 + 4 * size_t{i}, element) || (element && !T::verifyTable(*this, element, depth)))
                return false;
        }
        return true;
    }

    const uint8_t* base() const { return base_; }

private:
    size_t offsetOf(const uint8_t* p) const { return static_cast<size_t>(p - base_); }

    // Every element of a vector of references costs one object, null or not:
    // otherwise one vector of null slots shared by many tables would be walked
    // once per table at no cost.
    bool charge(uint32_t elements) {
        objects_ += elements;
        return objects_ <= options_.maxObjects;
    }

    bool slot(const uint8_t* slotAddress, const uint8_t*& target) const {
        target = nullptr;
        const uint32_t offset = load<uint32_t>(slotAddress);
        if (offset == 0)
            return true;
        const size_t at = offsetOf(slotAddress) + offset;
        if (offset % 4 != 0 || at < offset || at + 4 > size_)
            return false;
        target = base_ + at;
        return true;
    }

    const uint8_t* base_;
    size_t size_;
    VerifyOptions options_;
    uint64_t objects_ = 0;
};

// True when `message` is a well-formed message whose root is the table `T`.
template <class T>
bool verify(const void* message, size_t size, VerifyOptions options = {}) {
    Verifier v(static_cast<const uint8_t*>(message), size, options);
    if (!v.header(T::typeId))
        return false;
    v.useDeclaredSize();
    return T::verifyTable(v, static_cast<const uint8_t*>(message) + kHeaderBytes, 0);
}

// True when `message` is a message whose root is the struct `S`.
template <class S>
bool verifyStruct(const void* message, size_t size) {
    Verifier v(static_cast<const uint8_t*>(message), size);
    return v.header(StructInfo<S>::typeId) && messageSize(message) >= kHeaderBytes + sizeof(S);
}

} // namespace bufs
