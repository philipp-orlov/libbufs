// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Local (Unix domain) sockets for the control side of the shared-memory
// transport: one SOCK_SEQPACKET connection per peer, carrying whole messages
// (record boundaries kept, in order) and, with them, file descriptors such as
// a bufs::shm::Memory or an anonymous Channel. Linux only.
//
// The usual pattern: the client creates a Channel, connects, and sends its
// first message with the channel's descriptor; afterwards each side builds
// messages in the channel and sends kDoorbell, one byte, or sends a message
// inline when it does not fit the channel. The socket is what tells each side
// that the other has gone. A zero-byte message cannot serve as the doorbell:
// on a SOCK_SEQPACKET socket it reads exactly like the peer closing.

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "bufs/fd.hpp"

namespace bufs::local {

// The most descriptors one message carries.
constexpr size_t kMaxFds = 4;

// A one-byte message: "a message waits in the channel". A libbufs message is
// at least 8 bytes, so the two never mix.
constexpr uint8_t kDoorbell = 0xDB;

// Descriptors received with a message, owned until moved out.
struct ReceivedFds {
    Fd fds[kMaxFds];
    size_t count = 0;

    Fd take(size_t index) { return index < count ? std::move(fds[index]) : Fd(); }
};

class Socket {
public:
    Socket() = default;
    explicit Socket(Fd fd) : fd_(std::move(fd)) {}

    // Connects to the socket at `path`.
    static Socket connect(const std::string& path) {
        Fd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        if (!fd)
            throw std::system_error(errno, std::generic_category(), "socket");
        const sockaddr_un address = addressOf(path);
        if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
            throw std::system_error(errno, std::generic_category(), "connect " + path);
        return Socket(std::move(fd));
    }

    // Listens at `path` with file mode `mode`. A socket file left behind by a
    // process that died is replaced; one another process still listens on is
    // an error.
    static Socket listen(const std::string& path, mode_t mode = 0666, int backlog = 64) {
        const sockaddr_un address = addressOf(path);
        struct stat st {};
        if (::lstat(path.c_str(), &st) == 0) {
            if (!S_ISSOCK(st.st_mode))
                throw std::runtime_error("bufs: " + path + " exists and is not a socket");
            Fd probe(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
            if (probe && ::connect(probe.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0)
                throw std::runtime_error("bufs: another process is listening on " + path);
            ::unlink(path.c_str());
        }
        Fd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (!fd)
            throw std::system_error(errno, std::generic_category(), "socket");
        if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
            throw std::system_error(errno, std::generic_category(), "bind " + path);
        if (::chmod(path.c_str(), mode) != 0 || ::listen(fd.get(), backlog) != 0) {
            const int error = errno;
            ::unlink(path.c_str());
            throw std::system_error(error, std::generic_category(), "listen " + path);
        }
        return Socket(std::move(fd));
    }

    // A new connection, non-blocking, or an empty Socket when none waits.
    Socket accept() const {
        Fd fd(::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
        if (!fd && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED && errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "accept");
        return Socket(std::move(fd));
    }

    // Sends one message with up to kMaxFds descriptors (borrowed: the caller
    // keeps them); `size` is at least 1. Returns false
    // when the socket is non-blocking and its buffer is full; throws when the
    // peer has gone or the message is larger than the socket takes.
    bool send(const void* data, size_t size, const int* fds = nullptr, size_t fdCount = 0) const {
        if (fdCount > kMaxFds)
            throw std::invalid_argument("bufs: too many descriptors");
        if (size == 0)
            throw std::invalid_argument("bufs: an empty message reads as the connection closing");
        iovec iov{const_cast<void*>(data), size};
        msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFds)];
        if (fdCount) {
            std::memset(control, 0, sizeof control);
            message.msg_control = control;
            message.msg_controllen = CMSG_SPACE(sizeof(int) * fdCount);
            cmsghdr* header = CMSG_FIRSTHDR(&message);
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = CMSG_LEN(sizeof(int) * fdCount);
            std::memcpy(CMSG_DATA(header), fds, sizeof(int) * fdCount);
        }
        for (;;) {
            if (::sendmsg(fd_.get(), &message, MSG_NOSIGNAL) >= 0)
                return true;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;
            throw std::system_error(errno, std::generic_category(), "sendmsg");
        }
    }

    bool sendDoorbell() const { return send(&kDoorbell, 1); }

    // Receives one message into `buffer`. Returns its size, -1 when the
    // socket is non-blocking and nothing waits, or 0 when the peer has gone.
    // A message
    // larger than `capacity`, or with more than kMaxFds descriptors, throws
    // std::length_error (its descriptors are closed).
    long receive(void* buffer, size_t capacity, ReceivedFds& fds) const {
        fds = ReceivedFds();
        iovec iov{buffer, capacity};
        msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFds)];
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        ssize_t got;
        do
            got = ::recvmsg(fd_.get(), &message, MSG_CMSG_CLOEXEC);
        while (got < 0 && errno == EINTR);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            throw std::system_error(errno, std::generic_category(), "recvmsg");
        }
        for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
                continue;
            const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) {
                int fd;
                std::memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof fd);
                if (fds.count < kMaxFds)
                    fds.fds[fds.count++].reset(fd);
                else
                    ::close(fd);
            }
        }
        if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
            fds = ReceivedFds();
            throw std::length_error("bufs: a message larger than the receive buffer");
        }
        return got;
    }

    // Grows the kernel's send buffer, which bounds the largest message, to at
    // least `bytes` where the system allows (net.core.wmem_max).
    void setSendBuffer(int bytes) const { ::setsockopt(fd_.get(), SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes); }

    void setNonBlocking(bool on) const {
        const int flags = ::fcntl(fd_.get(), F_GETFL);
        ::fcntl(fd_.get(), F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
    }

    // The peer's process, user and group ids.
    ucred peer() const {
        ucred credentials{};
        socklen_t length = sizeof credentials;
        ::getsockopt(fd_.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &length);
        return credentials;
    }

    // Ends the connection both ways; the peer's receive() then fails.
    void shutdown() const { ::shutdown(fd_.get(), SHUT_RDWR); }

    // Discards every message waiting, closing any descriptors they carry.
    // Closing a socket with unread messages makes the peer's next receive
    // fail with ECONNRESET before it reads what was sent to it last (a
    // closing error, say); after shutdown() and drain(), the peer reads
    // everything and then the end of the connection.
    void drain() const {
        for (;;) {
            alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFds)];
            char byte;
            iovec iov{&byte, 1};
            msghdr message{};
            message.msg_iov = &iov;
            message.msg_iovlen = 1;
            message.msg_control = control;
            message.msg_controllen = sizeof control;
            if (::recvmsg(fd_.get(), &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC) <= 0)
                return;
            for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
                if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
                    continue;
                const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < count; ++i) {
                    int fd;
                    std::memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof fd);
                    ::close(fd);
                }
            }
        }
    }

    int fd() const { return fd_.get(); }
    explicit operator bool() const { return static_cast<bool>(fd_); }

private:
    static sockaddr_un addressOf(const std::string& path) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.empty() || path.size() >= sizeof address.sun_path)
            throw std::invalid_argument("bufs: a socket path must be 1 to " +
                                        std::to_string(sizeof address.sun_path - 1) + " bytes");
        std::memcpy(address.sun_path, path.data(), path.size());
        return address;
    }

    Fd fd_;
};

} // namespace bufs::local
