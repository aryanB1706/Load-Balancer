#include "Backend.h"

#include <sstream>

using namespace std;

namespace lb {

BackendPool::BackendPool(const vector<BackendEndpoint>& endpoints) {
    backends_.reserve(endpoints.size());
    for (const auto& ep : endpoints) {
        backends_.push_back(make_unique<Backend>(ep.host, ep.port));
    }
}

const Backend* BackendPool::pick() noexcept {
    size_t n = backends_.size();
    if (n == 0) return nullptr;
    // Count healthy first so degraded sets stay even: with one of three
    // down, survivors split 50/50 instead of 33/67 (scan-forward would
    // double up the successor of the dead slot).
    size_t h = 0;
    for (auto& b : backends_) {
        if (b->healthy.load(memory_order_relaxed)) ++h;
    }
    if (h == 0) return nullptr;  // all unhealthy
    uint64_t k = next_.fetch_add(1, memory_order_relaxed) % h;
    for (auto& b : backends_) {
        if (!b->healthy.load(memory_order_relaxed)) continue;
        if (k == 0) return b.get();
        --k;
    }
    // Healthy set flipped mid-pick; fall back to first healthy (re-scanned).
    for (auto& b : backends_) {
        if (b->healthy.load(memory_order_relaxed)) return b.get();
    }
    return nullptr;
}

void BackendPool::setHealthy(size_t idx, bool h) noexcept {
    if (idx < backends_.size()) backends_[idx]->healthy.store(h, memory_order_relaxed);
}

bool BackendPool::isHealthy(size_t idx) const noexcept {
    if (idx >= backends_.size()) return false;
    return backends_[idx]->healthy.load(memory_order_relaxed);
}

const Backend* BackendPool::at(size_t idx) const noexcept {
    if (idx >= backends_.size()) return nullptr;
    return backends_[idx].get();
}

string BackendPool::describe() const {
    ostringstream os;
    os << "[";
    for (size_t i = 0; i < backends_.size(); ++i) {
        if (i) os << ",";
        os << backends_[i]->addr();
    }
    os << "]";
    return os.str();
}

}  // namespace lb
