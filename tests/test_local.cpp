// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Anonymous shared memory and local sockets: sealed memfds that cannot be
// resized, adoption that refuses anything else, a socket file left by a dead
// process, and a channel handed to another process over a socket and then
// driven by doorbells, with a reply too large for the channel sent inline.

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bufs/local.hpp"
#include "bufs/shm.hpp"
#include "ring.bufs.hpp"

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using test::ring::Packet;
using test::ring::Stop;
using namespace std::chrono_literals;

std::string socketPath() { return "/tmp/bufs-test-" + std::to_string(::getpid()) + ".sock"; }

template <class F>
bool throws(F f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void sealedMemory() {
    bufs::shm::Memory memory = bufs::shm::Memory::create("bufs-test", 8192);
    CHECK(memory.size() == 8192 && memory.data()[100] == 0);
    memory.data()[100] = 7;
    CHECK(::ftruncate(memory.fd(), 4096) != 0); // sealed: cannot shrink
    CHECK(::ftruncate(memory.fd(), 16384) != 0); // nor grow

    bufs::shm::Memory same = bufs::shm::Memory::adopt(bufs::Fd(::dup(memory.fd())));
    CHECK(same.size() == 8192 && same.data()[100] == 7);
    CHECK(throws([&] { bufs::shm::Memory::adopt(bufs::Fd(::dup(memory.fd())), 4096); }));

    // A plain memfd without seals is refused: it could shrink under a mapping.
    bufs::Fd unsealed(::memfd_create("unsealed", MFD_CLOEXEC));
    CHECK(::ftruncate(unsealed.get(), 4096) == 0);
    CHECK(throws([&] { bufs::shm::Memory::adopt(std::move(unsealed)); }));

    bufs::shm::Region region = bufs::shm::Region::createAnonymous("bufs-region", 1024);
    CHECK(region.name().empty() && region.fd() >= 0 && region.size() == 1024);
    region.data()[0] = 3;
    bufs::shm::Region adopted = bufs::shm::Region::adopt(bufs::Fd(::dup(region.fd())));
    CHECK(adopted.data()[0] == 3);
    // Sealed memory that is not a region (no header) is refused as one.
    CHECK(throws([&] { bufs::shm::Region::adopt(bufs::Fd(::dup(memory.fd()))); }));
}

void socketFile() {
    const std::string path = socketPath();
    {
        bufs::local::Socket server = bufs::local::Socket::listen(path);
        CHECK(throws([&] { bufs::local::Socket::listen(path); })); // still listening
    }
    // The listener is gone but its file is left, as after a crash: replaced.
    bufs::local::Socket again = bufs::local::Socket::listen(path);
    CHECK(static_cast<bool>(again));
    ::unlink(path.c_str());
}

// Child: connects, sends its channel inline with the descriptor, then
// sends 1000 packets through the channel with a doorbell each, then a Stop.
// Reads one reply per packet, ring or inline, and checks it.
int client(const std::string& path) {
    try {
        bufs::local::Socket socket = bufs::local::Socket::connect(path);
        bufs::shm::Channel channel = bufs::shm::Channel::createAnonymous("bufs-test-channel", 64 << 10);
        {
            bufs::MessageBuilder mb;
            mb.initRoot<Stop>().setCount(0); // "hello": a message with the channel attached
            const auto [data, size] = mb.finish();
            const int fd = channel.fd();
            socket.send(data, size, &fd, 1);
        }
        alignas(8) static uint8_t inbox[1 << 20];
        for (uint64_t seq = 0; seq < 1000; ++seq) {
            uint8_t* slot = channel.send().reserve(256, 1s);
            if (!slot)
                return 2;
            bufs::MessageBuilder mb(slot, 256);
            mb.initRoot<Packet>().setSeq(seq);
            channel.send().commit(mb.finish().second);
            socket.sendDoorbell();

            bufs::local::ReceivedFds fds;
            const long got = socket.receive(inbox, sizeof inbox, fds);
            const uint8_t* data = inbox;
            size_t size = static_cast<size_t>(got);
            const bool ring = got == 1;
            if (ring && !channel.receive().front(data, size))
                return 3;
            if (!bufs::verify<Packet>(data, size) || bufs::root<Packet>(data).seq() != seq)
                return 4;
            if ((seq % 100 == 0) != !ring || (seq % 100 == 0 && bufs::root<Packet>(data).payload().size() != 100000))
                return 5;
            if (ring)
                channel.receive().pop();
        }
        bufs::MessageBuilder mb;
        mb.initRoot<Stop>().setCount(1000);
        const auto [data, size] = mb.finish();
        socket.send(data, size);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "client: %s\n", e.what());
        return 1;
    }
}

void channelOverSocket() {
    const std::string path = socketPath();
    bufs::local::Socket listener = bufs::local::Socket::listen(path);
    const pid_t child = ::fork();
    if (child == 0)
        ::_exit(client(path));

    bufs::local::Socket peer;
    for (int i = 0; i < 1000 && !peer; ++i) {
        peer = listener.accept();
        if (!peer)
            ::usleep(1000);
    }
    CHECK(static_cast<bool>(peer));
    peer.setNonBlocking(false);
    peer.setSendBuffer(1 << 20);
    CHECK(peer.peer().pid == child);

    alignas(8) static uint8_t inbox[4096];
    bufs::local::ReceivedFds fds;
    long got = peer.receive(inbox, sizeof inbox, fds);
    CHECK(got > 1 && fds.count == 1 && bufs::typeOf(inbox, static_cast<size_t>(got)) == Stop::typeId);
    bufs::shm::Channel channel = bufs::shm::Channel::adopt(fds.take(0));

    std::vector<uint8_t> big(100000, 9);
    uint64_t served = 0;
    bool stopped = false;
    while (!stopped) {
        got = peer.receive(inbox, sizeof inbox, fds);
        if (got <= 0)
            break;
        if (got != 1) {
            stopped = bufs::typeOf(inbox, static_cast<size_t>(got)) == Stop::typeId &&
                      bufs::root<Stop>(inbox).count() == 1000;
            continue;
        }
        const uint8_t* data = nullptr;
        size_t size = 0;
        CHECK(channel.receive().front(data, size));
        const uint64_t seq = bufs::verify<Packet>(data, size) ? bufs::root<Packet>(data).seq() : ~0ull;
        channel.receive().pop();
        if (seq % 100 == 0) {
            // Larger than half the ring: inline.
            bufs::MessageBuilder mb;
            Packet::Builder p = mb.initRoot<Packet>();
            p.setSeq(seq);
            p.setPayload(big.data(), big.size());
            const auto [bytes, length] = mb.finish();
            CHECK(peer.send(bytes, length));
        } else {
            uint8_t* slot = channel.send().reserve(256);
            bufs::MessageBuilder mb(slot, 256);
            mb.initRoot<Packet>().setSeq(seq);
            channel.send().commit(mb.finish().second);
            CHECK(peer.sendDoorbell());
        }
        ++served;
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(stopped && served == 1000);
    // The client has exited: its end of the connection reads as closed.
    CHECK(peer.receive(inbox, sizeof inbox, fds) == 0);
    std::printf("  1000 requests over a channel passed on a local socket, 10 inline replies\n");
    ::unlink(path.c_str());
}

} // namespace

int main() {
    sealedMemory();
    socketFile();
    channelOverSocket();
    if (failures == 0)
        std::puts("libbufs local: sealed memory, socket file and channel over a local socket passed");
    return failures == 0 ? 0 : 1;
}
