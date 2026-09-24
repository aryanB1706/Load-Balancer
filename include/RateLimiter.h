#pragma once

// Per-client-IP token-bucket rate limiter (Task 7).
// 16 shards, each with its own mutex + hash map, so different IPs (and
// usually even the same burst from one IP lands on one shard) contend
// minimally. Token refill is lazy (computed on each allow() from the
// monotonic clock). A background thread periodically drops entries idle
// longer than idle_expire_sec so the map can't grow forever.
// rate_per_sec <= 0 means disabled: allow() always returns true.
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace lb {

struct RateLimitParams {
    double rate_per_sec = 0;  // tokens added per second; 0 = unlimited
    double burst = 100;       // bucket capacity (also starting tokens)
    int idle_expire_sec = 60;
    int cleanup_sec = 10;
};

class RateLimiter {
public:
    explicit RateLimiter(RateLimitParams params);
    ~RateLimiter() { stop(); }

    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    bool enabled() const noexcept { return params_.rate_per_sec > 0; }

    void start();  // no-op when disabled
    void stop();   // idempotent, joins the cleanup thread

    // True = connection may proceed (one token consumed). False = reject.
    bool allow(const std::string& ip);

    // Total tracked IPs (locks each shard briefly; for tests/logging).
    size_t size() const;

private:
    struct Bucket {
        double tokens = 0;
        std::chrono::steady_clock::time_point last{};
    };
    struct Shard {
        mutable std::mutex m;
        std::unordered_map<std::string, Bucket> map;
    };
    static constexpr size_t kShards = 16;

    Shard& shardFor(const std::string& ip);
    void threadMain();
    void cleanupOnce();

    RateLimitParams params_;
    std::array<Shard, kShards> shards_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace lb
