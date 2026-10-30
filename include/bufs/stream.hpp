// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Messages laid end to end. Every message starts with its own u32 size, a
// multiple of 8, so the next one starts 8-aligned right after it and no extra
// framing is needed: a server can append batches to one buffer or one socket.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "bufs/format.hpp"
#include "bufs/verifier.hpp"

namespace bufs {

// Walks the messages of a stream. Checks only that each size is sane, not the
// messages themselves; see verifyStream() for that.
class MessageStream {
public:
    MessageStream(const void* data, size_t size) : data_(static_cast<const uint8_t*>(data)), size_(size) {}

    // The next message, or false at the end of the stream. Throws
    // std::runtime_error ("bufs: bad message size at offset N") when the size
    // is below the header, not a multiple of 8, or runs past the end.
    bool next(const uint8_t*& message, size_t& messageSize) {
        if (offset_ >= size_)
            return false;
        const size_t left = size_ - offset_;
        const uint32_t declared = left >= 4 ? load<uint32_t>(data_ + offset_) : 0;
        if (left < kHeaderBytes || declared < kHeaderBytes || declared % 8 != 0 || declared > left)
            throw std::runtime_error("bufs: bad message size at offset " + std::to_string(offset_));
        message = data_ + offset_;
        messageSize = declared;
        offset_ += declared;
        return true;
    }

    // Where the next message starts.
    size_t offset() const { return offset_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t offset_ = 0;
};

// verify<T>() on every message of the stream. False on the first message that
// fails, is not of type T, or has a bad size.
template <class T>
bool verifyStream(const void* data, size_t size, const VerifyOptions& options = {}) {
    try {
        MessageStream stream(data, size);
        const uint8_t* message = nullptr;
        size_t messageSize = 0;
        while (stream.next(message, messageSize))
            if (!verify<T>(message, messageSize, options))
                return false;
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

} // namespace bufs
