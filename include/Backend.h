#pragma once

// Task 5: BackendPool with lock-free round-robin + health skipping.
// Each worker thread calls pick() concurrently; the only shared mutable
// state is an atomic counter and per-backend atomic<bool> healthy flags,
// so the hot path stays lock-free. HealthChecker (Task 6) will flip the
// healthy flags; pick() skips unhealthy entries.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Config.h"

namespace lb {

struct Backend {
    std::string host;
    int port = 0;
    std::atomic<bool> healthy{true};

    Backend(std::string h, int p) : host(std::move(h)), port(p) {}

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    std::string addr() const { return host + ":" + std::to_string(port); }
};

class BackendPool {
public:
    explicit BackendPool(const std::vector<BackendEndpoint>& endpoints);
    ~BackendPool() = default;

    BackendPool(const BackendPool&) = delete;
    BackendPool& operator=(const BackendPool&) = delete;

    bool empty() const noexcept { return backends_.empty(); }
    size_t size() const noexcept { return backends_.size(); }

    // Lock-free round-robin. Returns nullptr when empty or all unhealthy.
    const Backend* pick() noexcept;

    // Health control (used by HealthChecker later; also handy in tests).
    void setHealthy(size_t idx, bool h) noexcept;
    bool isHealthy(size_t idx) const noexcept;
    const Backend* at(size_t idx) const noexcept;

    std::string describe() const;

private:
    // Heap-allocated entries: addresses stay stable for raw-pointer picks.
    std::vector<std::unique_ptr<Backend>> backends_;
    std::atomic<uint64_t> next_{0};
};

}  // namespace lb
