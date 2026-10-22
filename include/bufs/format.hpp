// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The wire format. Little-endian, 8-byte aligned messages:
//
//   message  = header root-object other-objects...
//   header   = u32 size (bytes, the whole message, a multiple of 8)
//              u32 typeId (FNV-1a of the root type's qualified name)
//   table    = u16 dataBytes  (header + data section, >= 4)
//              u16 refCount
//              data section   (fields at fixed offsets, value XOR default)
//              pad to 4
//              refCount x u32 (forward offset from the slot, 0 = null)
//   vector   = u32 count, elements (scalars, structs or u32 refs)
//   string   = u32 length, bytes, NUL
//
// Tables start 8-aligned; vectors whose elements need 8-byte alignment start
// at 4 mod 8 so their elements do. Every reference points forward, so a
// message is a DAG built front to back. See docs/design.md.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "libbufs supports little-endian targets only"
#endif

namespace bufs {

constexpr uint32_t kHeaderBytes = 8;
constexpr uint32_t kTableHeaderBytes = 4;
constexpr size_t kMaxMessageBytes = size_t{0xFFFFFFF8u};

// Thrown by a builder that has run out of room: a fixed buffer is full, or a
// message would pass kMaxMessageBytes.
class CapacityError : public std::length_error {
public:
    using std::length_error::length_error;
};

// FNV-1a, 32 bits: how a type's qualified name ("ns.Type") becomes its typeId.
constexpr uint32_t fnv1a(std::string_view text) {
    uint32_t hash = 2166136261u;
    for (char c : text) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

constexpr size_t align4(size_t value) { return (value + 3) & ~size_t{3}; }
constexpr size_t align8(size_t value) { return (value + 7) & ~size_t{7}; }

template <class T>
inline T load(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

template <class T>
inline void store(uint8_t* p, T value) {
    std::memcpy(p, &value, sizeof value);
}

namespace detail {

template <size_t N>
struct UintOf;
template <>
struct UintOf<1> {
    using type = uint8_t;
};
template <>
struct UintOf<2> {
    using type = uint16_t;
};
template <>
struct UintOf<4> {
    using type = uint32_t;
};
template <>
struct UintOf<8> {
    using type = uint64_t;
};

template <class T>
using Bits = typename UintOf<sizeof(T)>::type;

template <class T>
inline Bits<T> toBits(T value) {
    Bits<T> bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

template <class T>
inline T fromBits(Bits<T> bits) {
    T value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

inline uint16_t dataBytes(const uint8_t* table) { return load<uint16_t>(table); }
inline uint16_t refCount(const uint8_t* table) { return load<uint16_t>(table + 2); }

// A scalar field at `offset` from the table start; `def` when the table's
// data section ends before it (written by an older schema).
template <class T>
inline T readField(const uint8_t* table, uint32_t offset, T def) {
    if (!table || offset + sizeof(T) > dataBytes(table))
        return def;
    return fromBits<T>(static_cast<Bits<T>>(load<Bits<T>>(table + offset) ^ toBits(def)));
}

inline bool readBit(const uint8_t* table, uint32_t bit, bool def) {
    if (!table || (bit >> 3) >= dataBytes(table))
        return def;
    return (((table[bit >> 3] >> (bit & 7)) & 1) != 0) != def;
}

template <class T>
inline void writeField(uint8_t* table, uint32_t offset, T value, T def) {
    store<Bits<T>>(table + offset, static_cast<Bits<T>>(toBits(value) ^ toBits(def)));
}

inline void writeBit(uint8_t* table, uint32_t bit, bool value, bool def) {
    const uint8_t mask = static_cast<uint8_t>(1u << (bit & 7));
    if (value != def)
        table[bit >> 3] |= mask;
    else
        table[bit >> 3] &= static_cast<uint8_t>(~mask);
}

template <class S>
inline const S& zeroStruct() {
    static const S zero{};
    return zero;
}

// An inline struct field; the zero struct when the data section ends first.
template <class S>
inline const S& readStruct(const uint8_t* table, uint32_t offset) {
    if (!table || offset + sizeof(S) > dataBytes(table))
        return zeroStruct<S>();
    return *reinterpret_cast<const S*>(table + offset);
}

// The object reference slot `index` points at, or nullptr.
inline const uint8_t* readRef(const uint8_t* table, uint32_t index) {
    if (!table || index >= refCount(table))
        return nullptr;
    const uint8_t* slot = table + align4(dataBytes(table)) + 4 * size_t{index};
    const uint32_t offset = load<uint32_t>(slot);
    return offset ? slot + offset : nullptr;
}

inline std::string_view readString(const uint8_t* object) {
    if (!object)
        return {};
    return std::string_view(reinterpret_cast<const char*>(object + 4), load<uint32_t>(object));
}

} // namespace detail

// The header's fields, without verifying anything else.
inline uint32_t messageSize(const void* data) { return load<uint32_t>(static_cast<const uint8_t*>(data)); }
inline uint32_t messageType(const void* data) { return load<uint32_t>(static_cast<const uint8_t*>(data) + 4); }

// The type id of a message of at least kHeaderBytes, or 0 when it is shorter.
// The way to dispatch on what a received message is.
inline uint32_t typeOf(const void* data, size_t size) {
    return size >= kHeaderBytes ? messageType(data) : 0;
}

// Type information for generated structs (tables carry theirs as members).
template <class S>
struct StructInfo;

} // namespace bufs
