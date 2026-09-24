#include "RateLimiter.h"

#include <functional>

namespace lb {

RateLimiter::RateLimiter(RateLimitParams params) : params_(params) {}

void RateLimiter::start() {
    if (!enabled()) return;
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stop_.store(false);
    thread_ = std::thread([this] { threadMain(); });
}

void RateLimiter::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

RateLimiter::Shard& RateLimiter::shardFor(const std::string& ip) {
    size_t h = std::hash<std::string>{}(ip);
    return shards_[h % kShards];
}

bool RateLimiter::allow(const std::string& ip) {
    if (!enabled()) return true;
    auto now = std::chrono::steady_clock::now();
    Shard& s = shardFor(ip);
    std::lock_guard<std::mutex> lk(s.m);
    auto it = s.map.find(ip);
    if (it == s.map.end()) {
        // New IP starts with a full bucket, spends one token immediately.
        s.map.emplace(ip, Bucket{params_.burst - 1.0, now});
        return true;
    }
    Bucket& b = it->second;
    double elapsed =
        std::chrono::duration_cast<std::chrono::duration<double>>(now - b.last).count();
    if (elapsed > 0) {
        b.tokens += elapsed * params_.rate_per_sec;
        if (b.tokens > params_.burst) b.tokens = params_.burst;
        b.last = now;
    }
    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    return false;
}

size_t RateLimiter::size() const {
    size_t n = 0;
    for (auto& s : shards_) {
        std::lock_guard<std::mutex> lk(s.m);
        n += s.map.size();
    }
    return n;
}

void RateLimiter::cleanupOnce() {
    auto now = std::chrono::steady_clock::now();
    auto idle = std::chrono::seconds(params_.idle_expire_sec);
    for (auto& s : shards_) {
        std::lock_guard<std::mutex> lk(s.m);
        for (auto it = s.map.begin(); it != s.map.end();) {
            if (now - it->second.last > idle) {
                it = s.map.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void RateLimiter::threadMain() {
    while (!stop_.load()) {
        int waited = 0;
        int total_ms = params_.cleanup_sec * 1000;
        while (waited < total_ms && !stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waited += 100;
        }
        if (stop_.load()) break;
        cleanupOnce();
    }
}

}  // namespace lb
