// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The shared-memory transport against sockets: a client process and a server
// process exchange the benchmark's query and report, built with libbufs
// straight into the send buffer (a ring slot, or a socket's staging buffer)
// and read in place on the other side.
//
//   bufs_bench_shm [--quick] [--iterations N] [--only pingpong,stream]
//
// Ping-pong: the client sends a Query, the server verifies and reads it and
// answers with a Report, which the client verifies and reads; round-trip
// times per message. Stream: the server sends reports one way as fast as the
// client takes them.

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "bench_bufs.hpp"
#include "bufs/shm.hpp"

namespace {

using namespace std::chrono_literals;

constexpr uint64_t kStop = ~uint64_t{0};

enum class Wait { Futex, Spin, SpinThenFutex };

const char* waitName(Wait w) {
    switch (w) {
    case Wait::Futex: return "futex";
    case Wait::Spin: return "spin";
    case Wait::SpinThenFutex: return "spin 20us+futex";
    }
    return "";
}

inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

// One end of a connection: build into beginSend()'s memory, publish with
// endSend(); receive() blocks for the next message, valid until release().
class Transport {
public:
    virtual ~Transport() = default;
    virtual uint8_t* beginSend(size_t maxBytes) = 0;
    virtual void endSend(size_t bytes) = 0;
    virtual void receive(const uint8_t*& data, size_t& size) = 0;
    virtual void release() = 0;
};

class ShmTransport : public Transport {
public:
    ShmTransport(bufs::shm::Channel channel, Wait wait) : channel_(std::move(channel)), wait_(wait) {}

    uint8_t* beginSend(size_t maxBytes) override { return channel_.send().reserve(maxBytes, -1ns); }
    void endSend(size_t bytes) override { channel_.send().commit(bytes); }

    void receive(const uint8_t*& data, size_t& size) override {
        bufs::shm::Ring& ring = channel_.receive();
        for (;;) {
            if (ring.front(data, size))
                return;
            switch (wait_) {
            case Wait::Futex: ring.wait(-1ns); break;
            case Wait::Spin:
                while (ring.empty())
                    cpuRelax();
                break;
            case Wait::SpinThenFutex: ring.wait(-1ns, 20us); break;
            }
        }
    }

    void release() override { channel_.receive().pop(); }

private:
    bufs::shm::Channel channel_;
    Wait wait_;
};

class SocketTransport : public Transport {
public:
    explicit SocketTransport(int fd) : fd_(fd) {}
    ~SocketTransport() override { ::close(fd_); }

    uint8_t* beginSend(size_t maxBytes) override { return reserve(send_, maxBytes); }
    void endSend(size_t bytes) override { writeAll(send_.get(), bytes); }

    void receive(const uint8_t*& data, size_t& size) override {
        uint8_t* header = reserve(receive_, bufs::kHeaderBytes);
        readAll(header, bufs::kHeaderBytes);
        const size_t total = bufs::messageSize(header);
        if (total < bufs::kHeaderBytes)
            throw std::runtime_error("bad frame");
        uint8_t* buffer = reserve(receive_, total, bufs::kHeaderBytes);
        readAll(buffer + bufs::kHeaderBytes, total - bufs::kHeaderBytes);
        data = buffer;
        size = total;
    }

    void release() override {}

private:
    struct Free {
        void operator()(uint8_t* p) const { std::free(p); }
    };
    struct Buffer {
        std::unique_ptr<uint8_t, Free> memory;
        size_t capacity = 0;
        uint8_t* get() const { return memory.get(); }
    };

    // At least `bytes`, 64-aligned, keeping the first `keep` bytes.
    static uint8_t* reserve(Buffer& b, size_t bytes, size_t keep = 0) {
        if (b.capacity < bytes) {
            const size_t capacity = std::max<size_t>(bytes, 4096) * 2 / 64 * 64;
            std::unique_ptr<uint8_t, Free> bigger(static_cast<uint8_t*>(std::aligned_alloc(64, capacity)));
            if (keep)
                std::memcpy(bigger.get(), b.get(), keep);
            b.memory = std::move(bigger);
            b.capacity = capacity;
        }
        return b.get();
    }

    void writeAll(const uint8_t* p, size_t n) {
        while (n) {
            const ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error(std::string("send: ") + std::strerror(errno));
            }
            p += w;
            n -= static_cast<size_t>(w);
        }
    }

    void readAll(uint8_t* p, size_t n) {
        while (n) {
            const ssize_t r = ::recv(fd_, p, n, 0);
            if (r <= 0) {
                if (r < 0 && errno == EINTR)
                    continue;
                throw std::runtime_error("peer closed");
            }
            p += r;
            n -= static_cast<size_t>(r);
        }
    }

    int fd_;
    Buffer send_, receive_;
};

// ---------------------------------------------------------- the messages --

struct Messages {
    bd::QueryData query = bd::makeQuery();
    bd::ReportData report;
    size_t reportBytes = 0; // exact size of a built report
    size_t queryBytes = 0;

    explicit Messages(bd::ReportData r) : report(std::move(r)) {
        bufs::MessageBuilder mb;
        bufsbench::build(mb, reportInput());
        reportBytes = mb.finish().second;
        bufsbench::build(mb, queryInput());
        queryBytes = mb.finish().second;
    }
    bd::Input queryInput() const {
        bd::Input in;
        in.type = bd::Type::Query;
        in.query = &query;
        return in;
    }
    bd::Input reportInput() const {
        bd::Input in;
        in.type = bd::Type::Report;
        in.report = &report;
        return in;
    }
};

uint64_t sink;

void sendQuery(Transport& t, Messages& m, uint64_t id) {
    uint8_t* slot = t.beginSend(m.queryBytes);
    bufs::MessageBuilder mb(slot, m.queryBytes);
    m.query.queryId = id;
    bufsbench::build(mb, m.queryInput());
    t.endSend(mb.finish().second);
}

void sendReport(Transport& t, Messages& m, uint64_t id) {
    uint8_t* slot = t.beginSend(m.reportBytes);
    bufs::MessageBuilder mb(slot, m.reportBytes);
    m.report.queryId = id;
    bufsbench::build(mb, m.reportInput());
    t.endSend(mb.finish().second);
}

// Receives one message of `type`, verifies and reads it; returns its queryId.
uint64_t receive(Transport& t, bd::Type type) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    t.receive(data, size);
    if (!bufsbench::verify(type, data, size))
        throw std::runtime_error("message does not verify");
    sink += bufsbench::read(type, data);
    const uint64_t id = type == bd::Type::Query ? bufs::root<bench::Query>(data).queryId()
                                                : bufs::root<bench::Report>(data).queryId();
    t.release();
    return id;
}

// ------------------------------------------------------------ processes --

struct Endpoints {
    std::function<std::unique_ptr<Transport>()> client; // in the parent, after fork
    std::function<std::unique_ptr<Transport>()> server; // in the child
    std::function<void()> cleanup;                       // in the parent, after the child exits
};

int g_serial = 0;

Endpoints shmEndpoints(Wait wait) {
    const std::string name = "/bufs-bench-" + std::to_string(::getpid()) + "-" + std::to_string(g_serial++);
    auto channel = std::make_shared<bufs::shm::Channel>(bufs::shm::Channel::create(name, size_t{1} << 20));
    Endpoints e;
    e.client = [channel, wait] { return std::make_unique<ShmTransport>(std::move(*channel), wait); };
    e.server = [name, wait] { return std::make_unique<ShmTransport>(bufs::shm::Channel::open(name), wait); };
    return e;
}

Endpoints unixEndpoints() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        throw std::runtime_error("socketpair");
    Endpoints e;
    e.client = [fds] {
        ::close(fds[1]);
        return std::make_unique<SocketTransport>(fds[0]);
    };
    e.server = [fds] {
        ::close(fds[0]);
        return std::make_unique<SocketTransport>(fds[1]);
    };
    return e;
}

Endpoints tcpEndpoints() {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof address;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        throw std::runtime_error("tcp listen");
    auto noDelay = [](int fd) {
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    };
    Endpoints e;
    e.client = [listener, noDelay] {
        const int fd = ::accept(listener, nullptr, nullptr);
        ::close(listener);
        noDelay(fd);
        return std::make_unique<SocketTransport>(fd);
    };
    e.server = [listener, address, noDelay] {
        ::close(listener);
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
            ::_exit(3);
        noDelay(fd);
        return std::make_unique<SocketTransport>(fd);
    };
    return e;
}

std::vector<int> g_cpus;

// Forks the server: `serve` runs in the child on the second allowed CPU,
// `drive` in the parent on the first.
void run(Endpoints e, const std::function<void(Transport&)>& serve, const std::function<void(Transport&)>& drive) {
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (g_cpus.size() > 1)
            bd::pinTo(g_cpus[1]);
        int status = 0;
        try {
            std::unique_ptr<Transport> t = e.server();
            serve(*t);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "server: %s\n", error.what());
            status = 1;
        }
        // Skips the parent's objects copied into this process (a Channel
        // copy would unlink the parent's region).
        ::_exit(status);
    }
    {
        std::unique_ptr<Transport> t = e.client();
        drive(*t);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        std::printf("  (server failed, status %d)\n", status);
}

struct Latency {
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0, mean = 0, perSecond = 0;
};

Latency pingPong(Endpoints e, Messages& m, uint64_t iterations) {
    Latency result;
    run(
        std::move(e),
        [&](Transport& t) {
            for (;;) {
                const uint64_t id = receive(t, bd::Type::Query);
                if (id == kStop)
                    return;
                sendReport(t, m, id);
            }
        },
        [&](Transport& t) {
            const uint64_t warm = std::min<uint64_t>(iterations / 10, 10000);
            std::vector<double> rtt;
            rtt.reserve(iterations);
            double start = 0;
            for (uint64_t i = 0; i < warm + iterations; ++i) {
                if (i == warm)
                    start = bd::now();
                const auto t0 = std::chrono::steady_clock::now();
                sendQuery(t, m, i);
                const uint64_t id = receive(t, bd::Type::Report);
                const auto t1 = std::chrono::steady_clock::now();
                if (id != i)
                    throw std::runtime_error("report out of order");
                if (i >= warm)
                    rtt.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            }
            const double seconds = bd::now() - start;
            sendQuery(t, m, kStop);
            std::sort(rtt.begin(), rtt.end());
            auto at = [&](double q) { return rtt[std::min(rtt.size() - 1, static_cast<size_t>(q * rtt.size()))]; };
            double sum = 0;
            for (double x : rtt)
                sum += x;
            result = Latency{at(0.5), at(0.9), at(0.99), at(0.999), rtt.back(), sum / rtt.size(),
                             iterations / seconds};
        });
    return result;
}

struct Throughput {
    double perSecond = 0, megabytesPerSecond = 0;
};

Throughput stream(Endpoints e, Messages& m, uint64_t count) {
    Throughput result;
    run(
        std::move(e),
        [&](Transport& t) {
            for (uint64_t i = 0; i < count; ++i)
                sendReport(t, m, i);
        },
        [&](Transport& t) {
            const double start = bd::now();
            for (uint64_t i = 0; i < count; ++i)
                if (receive(t, bd::Type::Report) != i)
                    throw std::runtime_error("stream out of order");
            const double seconds = bd::now() - start;
            result = Throughput{count / seconds, count * static_cast<double>(m.reportBytes) / seconds / 1e6};
        });
    return result;
}

} // namespace

int main(int argc, char** argv) {
    bool quick = false;
    uint64_t iterations = 200000;
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quick") {
            quick = true;
            iterations = 20000;
        } else if (arg == "--iterations" && i + 1 < argc) {
            iterations = std::stoull(argv[++i]);
        } else if (arg == "--only" && i + 1 < argc) {
            only.push_back(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: bufs_bench_shm [--quick] [--iterations N] [--only pingpong|stream]\n");
            return 2;
        }
    }
    auto wants = [&](const char* s) {
        return only.empty() || std::any_of(only.begin(), only.end(), [&](const std::string& o) {
                   return o.find(s) != std::string::npos;
               });
    };

    g_cpus = bd::allowedCpus();
    if (!g_cpus.empty())
        bd::pinTo(g_cpus.front());
    std::printf("bufs_bench_shm: client on CPU %d, server on CPU %d%s; %llu iterations%s\n",
                g_cpus.empty() ? -1 : g_cpus[0], g_cpus.size() > 1 ? g_cpus[1] : (g_cpus.empty() ? -1 : g_cpus[0]),
                g_cpus.size() > 1 ? "" : " (ONE CPU: both processes share it)",
                static_cast<unsigned long long>(iterations), quick ? " (quick)" : "");

    struct ReportCase {
        const char* name;
        Messages messages;
    };
    std::vector<ReportCase> reports;
    reports.push_back({"report 1 item", Messages(bd::makeReport(1, 0))});
    reports.push_back({"report 4 + 8K blob", Messages(bd::makeReport(4, 8192))});

    struct TransportCase {
        std::string name;
        std::function<Endpoints()> make;
    };
    const std::vector<TransportCase> transports = {
        {std::string("shm ") + waitName(Wait::Futex), [] { return shmEndpoints(Wait::Futex); }},
        {std::string("shm ") + waitName(Wait::SpinThenFutex), [] { return shmEndpoints(Wait::SpinThenFutex); }},
        {std::string("shm ") + waitName(Wait::Spin), [] { return shmEndpoints(Wait::Spin); }},
        {"unix socket", unixEndpoints},
        {"tcp loopback", tcpEndpoints},
    };

    if (wants("pingpong")) {
        std::printf("\nPing-pong round trip (query -> report, both verified and read), ns\n");
        std::printf("%-18s %-20s %8s %9s %8s %8s %8s %8s %9s %10s\n", "message", "transport", "bytes", "mean", "p50",
                    "p90", "p99", "p99.9", "max", "rt/s");
        for (ReportCase& r : reports) {
            for (const TransportCase& t : transports) {
                const Latency l = pingPong(t.make(), r.messages, iterations);
                std::printf("%-18s %-20s %8zu %9.0f %8.0f %8.0f %8.0f %8.0f %9.0f %10.0f\n", r.name, t.name.c_str(),
                            r.messages.reportBytes, l.mean, l.p50, l.p90, l.p99, l.p999, l.max, l.perSecond);
            }
        }
    }

    if (wants("stream")) {
        std::printf("\nOne-way stream of reports (server builds, client verifies and reads)\n");
        std::printf("%-18s %-20s %8s %10s %12s %10s\n", "message", "transport", "bytes", "messages", "msg/s", "MB/s");
        for (ReportCase& r : reports) {
            const uint64_t count = std::max<uint64_t>(1000, iterations * (r.messages.reportBytes > 8192 ? 2 : 10));
            for (const TransportCase& t : transports) {
                const Throughput x = stream(t.make(), r.messages, count);
                std::printf("%-18s %-20s %8zu %10llu %12.0f %10.0f\n", r.name, t.name.c_str(), r.messages.reportBytes,
                            static_cast<unsigned long long>(count), x.perSecond, x.megabytesPerSecond);
            }
        }
    }
    (void)sink;
    return 0;
}
