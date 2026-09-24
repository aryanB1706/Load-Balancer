#pragma once

// Task 4: Listener is a thin manager over N Workers (SO_REUSEPORT).
// Design choice: SO_REUSEPORT (one listen socket per worker) instead of a
// single acceptor + eventfd/queue, because:
//   1. Zero cross-thread fd handoff — no queue mutex, no wakeup latency.
//   2. Kernel accept distribution is fair and removes the acceptor bottleneck.
//   3. Each worker is fully self-contained (listen+epoll+conns); the hot path
//      (read/write/epoll) is 100% lock-free. The shared BackendPool picker
//      (Task 5) only uses lock-free atomics.
// Task 5: Listener owns the BackendPool shared by all workers.
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "Backend.h"
#include "HealthChecker.h"
#include "RateLimiter.h"
#include "Worker.h"

namespace lb {

class Listener {
public:
    Listener(int listen_port, std::shared_ptr<BackendPool> pool, int workers,
             HealthCheckParams hc_params, RateLimitParams rl_params);
    ~Listener() = default;

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Blocking: spawn workers, wait for requestStop() (SIGINT/SIGTERM),
    // then stop accepting, drain, join. Returns when all workers exited.
    void run();
    void requestStop() noexcept { stop_.store(true); }

private:
    int listen_port_;
    std::shared_ptr<BackendPool> pool_;
    int nworkers_;
    HealthCheckParams hc_params_;
    RateLimitParams rl_params_;
    std::atomic<bool> stop_{false};
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::thread> threads_;
};

}  // namespace lb
