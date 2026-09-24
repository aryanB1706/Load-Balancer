#pragma once

// Task 4: one worker = one thread + one epoll instance + own listen socket
// (SO_REUSEPORT) + own connection set. No shared mutable state in the hot
// path: each Connection is touched by exactly one worker thread.
// Task 5: backend choice comes from the shared lock-free BackendPool.
// Task 7: accept-path rate check via the shared sharded RateLimiter.
#include <atomic>
#include <memory>
#include <unordered_map>

#include "Backend.h"
#include "Connection.h"
#include "Fd.h"
#include "RateLimiter.h"

namespace lb {

class Worker {
public:
    Worker(int id, int listen_port, std::shared_ptr<BackendPool> pool,
           std::shared_ptr<RateLimiter> limiter);
    ~Worker() = default;

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Blocking: setup listen+epoll, serve until requestStop(), then stop
    // accepting, drain existing connections (bounded), and return.
    void run();
    void requestStop() noexcept { stop_.store(true); }
    bool stopped() const noexcept { return stop_.load(); }

    int id() const noexcept { return id_; }
    int accepted() const noexcept { return accepted_.load(); }
    int rejected() const noexcept { return rejected_.load(); }

private:
    bool setupListenSocket(std::string& err);
    bool setupEpoll(std::string& err);
    void loop();
    void handleAccept();
    void handleEvent(int fd, uint32_t events);
    void updateInterest(const std::shared_ptr<Connection>& c);
    void closeConnection(const std::shared_ptr<Connection>& c);

    static int createAndConnectBackend(const Backend& be, bool& connecting,
                                       std::string& err);

    int id_;
    int listen_port_;
    std::shared_ptr<BackendPool> pool_;
    std::shared_ptr<RateLimiter> limiter_;
    Fd listen_fd_;
    Fd epoll_fd_;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepted_{0};
    std::atomic<int> rejected_{0};

    // Both client fd and backend fd point at the same Connection.
    // Only this worker's thread touches this map (no locks).
    std::unordered_map<int, std::shared_ptr<Connection>> fd_map_;
};

}  // namespace lb
