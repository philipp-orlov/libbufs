// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Zero-copy views over a message. Nothing here copies or allocates: a view is
// a pointer into the message and stays valid as long as the message bytes do.
// Read only messages that verify() accepted, or that this process built.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "bufs/format.hpp"

namespace bufs {

// A vector of scalars, enums or structs: contiguous elements, read in place.
template <class T>
class Vector {
public:
    Vector() = default;
    explicit Vector(const uint8_t* object) : object_(object) {}

    uint32_t size() const { return object_ ? load<uint32_t>(object_) : 0; }
    bool empty() const { return size() == 0; }
    const T* data() const { return object_ ? reinterpret_cast<const T*>(object_ + 4) : nullptr; }
    const T* begin() const { return data(); }
    const T* end() const { return data() + size(); }
    const T& operator[](size_t i) const { return data()[i]; }
    explicit operator bool() const { return object_ != nullptr; }

private:
    const uint8_t* object_ = nullptr;
};

// A vector of bools, one byte each.
class BoolVector {
public:
    BoolVector() = default;
    explicit BoolVector(const uint8_t* object) : object_(object) {}

    uint32_t size() const { return object_ ? load<uint32_t>(object_) : 0; }
    bool empty() const { return size() == 0; }
    bool operator[](size_t i) const { return object_[4 + i] != 0; }
    explicit operator bool() const { return object_ != nullptr; }

private:
    const uint8_t* object_ = nullptr;
};

namespace detail {

// Shared by the vectors of references: element i is a u32 forward offset.
inline const uint8_t* element(const uint8_t* object, size_t i) {
    const uint8_t* slot = object + 4 + 4 * i;
    const uint32_t offset = load<uint32_t>(slot);
    return offset ? slot + offset : nullptr;
}

template <class Owner, class Value>
class RefIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Value;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = Value;

    RefIterator(const Owner* owner, size_t index) : owner_(owner), index_(index) {}
    Value operator*() const { return (*owner_)[index_]; }
    RefIterator& operator++() {
        ++index_;
        return *this;
    }
    RefIterator operator++(int) {
        RefIterator old = *this;
        ++index_;
        return old;
    }
    bool operator==(const RefIterator& other) const { return index_ == other.index_; }
    bool operator!=(const RefIterator& other) const { return index_ != other.index_; }

private:
    const Owner* owner_;
    size_t index_;
};

} // namespace detail

// A vector of strings.
class StringVector {
public:
    using iterator = detail::RefIterator<StringVector, std::string_view>;

    StringVector() = default;
    explicit StringVector(const uint8_t* object) : object_(object) {}

    uint32_t size() const { return object_ ? load<uint32_t>(object_) : 0; }
    bool empty() const { return size() == 0; }
    std::string_view operator[](size_t i) const { return detail::readString(detail::element(object_, i)); }
    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, size()); }
    explicit operator bool() const { return object_ != nullptr; }

private:
    const uint8_t* object_ = nullptr;
};

// A vector of tables; `Reader` is the table's generated Reader.
template <class Reader>
class TableVector {
public:
    using iterator = detail::RefIterator<TableVector, Reader>;

    TableVector() = default;
    explicit TableVector(const uint8_t* object) : object_(object) {}

    uint32_t size() const { return object_ ? load<uint32_t>(object_) : 0; }
    bool empty() const { return size() == 0; }
    Reader operator[](size_t i) const { return Reader(detail::element(object_, i)); }
    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, size()); }
    explicit operator bool() const { return object_ != nullptr; }

private:
    const uint8_t* object_ = nullptr;
};

// The root table of a message whose root type is the table `T`. Does not
// check anything: verify() first unless this process built the message.
template <class T>
typename T::Reader root(const void* message) {
    return typename T::Reader(static_cast<const uint8_t*>(message) + kHeaderBytes);
}

// The root of a message whose root type is the struct `S`.
template <class S>
const S& rootStruct(const void* message) {
    return *reinterpret_cast<const S*>(static_cast<const uint8_t*>(message) + kHeaderBytes);
}

} // namespace bufs
