#pragma once

// Background health checker (Task 6). A single thread probes every backend
// every interval_sec with a non-blocking TCP connect + timeout, and flips
// the pool's atomic healthy flags with hysteresis: DOWN after `fail_after`
// consecutive failures, back UP after `healthy_after` consecutive successes.
// Only transitions are logged. stop() is prompt and joinable; the hot path
// (workers) never blocks on this thread.
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "Backend.h"

namespace lb {

struct HealthCheckParams {
    int interval_sec = 2;
    int timeout_ms = 1000;
    int fail_after = 2;     // K consecutive failures -> DOWN
    int healthy_after = 2;  // M consecutive successes -> UP
};

class HealthChecker {
public:
    HealthChecker(std::shared_ptr<BackendPool> pool, HealthCheckParams params);
    ~HealthChecker() { stop(); }

    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    void start();
    void stop();  // idempotent, joins the thread

private:
    void threadMain();
    bool probe(const Backend& be);  // true = TCP connect succeeded
    bool sleepSlice(int total_ms);  // false when stop requested

    std::shared_ptr<BackendPool> pool_;
    HealthCheckParams params_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::vector<int> fails_;
    std::vector<int> oks_;
};

}  // namespace lb
