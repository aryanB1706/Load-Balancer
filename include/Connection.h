#pragma once

// Bidirectional client<->backend relay over two non-blocking fds.
// Level-triggered epoll. The owner (Listener) drives:
//   onClientEvent / onBackendEvent -> true means "tear me down".
//   clientInterest / backendInterest -> current EPOLL mask to arm.
//
// Buffering (Task 3): two fixed 64KB buffers (c2b, b2c). Partial writes /
// EAGAIN keep leftovers and arm EPOLLOUT. Backpressure: when a buffer is
// full we drop EPOLLIN on the source fd until it drains, then resume.
//
// Half-close forwarding: when one side hits EOF (recv==0, e.g. `nc -N`
// shutdowns WR after sending), we flush queued bytes and propagate
// shutdown(WR) to the other side instead of discarding data. The pair
// is torn down once both sides hit EOF and buffers drain, or on error.
#include <cstdint>
#include <vector>

#include "Fd.h"

namespace lb {

class Connection {
public:
    // Takes ownership of both fds.
    Connection(int client_fd, int backend_fd, bool backend_connecting);
    ~Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    int clientFd() const { return client_.get(); }
    int backendFd() const { return backend_.get(); }
    bool closed() const { return closed_; }

    bool onClientEvent(uint32_t events);
    bool onBackendEvent(uint32_t events);

    uint32_t clientInterest() const;
    uint32_t backendInterest() const;

private:
    static constexpr size_t kMaxBuf = 64u * 1024u;  // fixed cap per dir (Task 3)

    bool finishBackendConnect();
    bool doReadClient();   // sets client_eof_ on recv==0
    bool doFlushB2C();     // backend->client pending bytes -> client
    bool doReadBackend();  // sets backend_eof_ on recv==0
    bool doFlushC2B();     // client->backend pending bytes -> backend
    void propagateShutdowns();
    bool shouldClose() const;
    static void shutWr(int fd) noexcept;

    Fd client_;
    Fd backend_;
    bool backend_connecting_ = false;
    bool closed_ = false;

    bool client_eof_ = false;
    bool backend_eof_ = false;
    bool client_shut_wr_ = false;
    bool backend_shut_wr_ = false;

    std::vector<char> c2b_;  // bytes read from client, awaiting write to backend
    size_t c2b_off_ = 0;
    std::vector<char> b2c_;  // bytes read from backend, awaiting write to client
    size_t b2c_off_ = 0;
};

}  // namespace lb
