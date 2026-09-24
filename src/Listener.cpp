#include "Listener.h"

#include <chrono>
#include <thread>

#include "Logger.h"

namespace lb {

Listener::Listener(int listen_port, std::shared_ptr<BackendPool> pool, int workers,
                   HealthCheckParams hc_params, RateLimitParams rl_params)
    : listen_port_(listen_port),
      pool_(std::move(pool)),
      nworkers_(workers > 0 ? workers : 1),
      hc_params_(hc_params),
      rl_params_(rl_params) {}

void Listener::run() {
    if (!pool_ || pool_->empty()) {
        LOG_ERROR("no backends configured");
        return;
    }
    // Health checker runs alongside workers and flips pool flags only.
    HealthChecker checker(pool_, hc_params_);
    checker.start();

    // Rate limiter (shared, sharded). Disabled when rate <= 0.
    auto limiter = std::make_shared<RateLimiter>(rl_params_);
    limiter->start();
    if (limiter->enabled()) {
        LOG_INFO("ratelimit: " + std::to_string(rl_params_.rate_per_sec) + "/s per IP, burst " +
                 std::to_string(rl_params_.burst));
    }

    workers_.reserve(static_cast<size_t>(nworkers_));
    threads_.reserve(static_cast<size_t>(nworkers_));
    for (int i = 0; i < nworkers_; ++i) {
        workers_.push_back(std::make_unique<Worker>(i, listen_port_, pool_, limiter));
    }
    for (int i = 0; i < nworkers_; ++i) {
        threads_.emplace_back([this, i] { workers_[static_cast<size_t>(i)]->run(); });
    }
    LOG_INFO("proxying -> " + pool_->describe() + " with " + std::to_string(nworkers_) +
             " workers (SO_REUSEPORT, round-robin)");

    // Wait for SIGINT/SIGTERM.
    while (!stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Graceful shutdown: stop health checks + limiter cleanup first (freeze
    // routing/limit view), then stop accepting, drain, join.
    LOG_INFO("shutting down: stop accepting, draining...");
    checker.stop();
    limiter->stop();
    for (auto& w : workers_) w->requestStop();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    int total = 0, rejected = 0;
    for (auto& w : workers_) {
        total += w->accepted();
        rejected += w->rejected();
    }
    threads_.clear();
    workers_.clear();
    LOG_INFO("listener stopped, total accepted=" + std::to_string(total) + " rejected=" +
             std::to_string(rejected));
}

}  // namespace lb
