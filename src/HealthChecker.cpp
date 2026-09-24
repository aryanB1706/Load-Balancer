#include "HealthChecker.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "Logger.h"
#include "SocketUtil.h"

namespace lb {

HealthChecker::HealthChecker(std::shared_ptr<BackendPool> pool, HealthCheckParams params)
    : pool_(std::move(pool)), params_(params) {}

void HealthChecker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;  // already running
    stop_.store(false);
    fails_.assign(pool_ ? pool_->size() : 0, 0);
    oks_.assign(pool_ ? pool_->size() : 0, 0);
    thread_ = std::thread([this] { threadMain(); });
}

void HealthChecker::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

bool HealthChecker::sleepSlice(int total_ms) {
    // Sleep in small slices so stop() wakes promptly (<=100ms).
    int waited = 0;
    while (waited < total_ms) {
        if (stop_.load()) return false;
        int step = total_ms - waited > 100 ? 100 : total_ms - waited;
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        waited += step;
    }
    return !stop_.load();
}

bool HealthChecker::probe(const Backend& be) {
    // Non-blocking TCP connect with timeout. Resolves via getaddrinfo so
    // hostnames work; tries each addr until one connects or all fail.
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string port = std::to_string(be.port);
    if (::getaddrinfo(be.host.c_str(), port.c_str(), &hints, &res) != 0) return false;

    bool ok = false;
    for (struct addrinfo* ai = res; ai != nullptr && !ok; ai = ai->ai_next) {
        int s = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (s < 0) {
            if (errno == EINVAL) s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s < 0) continue;
        }
        if (!setNonBlocking(s)) {
            while (::close(s) < 0 && errno == EINTR) {
            }
            continue;
        }

        int r = ::connect(s, ai->ai_addr, ai->ai_addrlen);
        if (r == 0) {
            ok = true;  // instant connect (loopback)
        } else if (errno == EINPROGRESS || errno == EINTR || errno == EALREADY) {
            struct pollfd pfd {};
            pfd.fd = s;
            pfd.events = POLLOUT;
            int ms_left = params_.timeout_ms;
            auto t0 = std::chrono::steady_clock::now();
            for (;;) {
                int pr = ::poll(&pfd, 1, ms_left);
                if (pr > 0) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                        ok = true;
                    break;
                }
                if (pr == 0) break;  // timeout -> fail
                if (errno == EINTR) {
                    auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count();
                    ms_left = params_.timeout_ms - static_cast<int>(spent);
                    if (ms_left <= 0) break;
                    continue;
                }
                break;  // poll error -> fail
            }
        } else if (errno == EISCONN) {
            ok = true;
        }
        // else: immediate refusal (ECONNREFUSED etc.) -> try next addr
        while (::close(s) < 0 && errno == EINTR) {
        }
    }
    ::freeaddrinfo(res);
    return ok;
}

void HealthChecker::threadMain() {
    if (!pool_ || pool_->empty()) {
        running_.store(false);
        return;
    }
    LOG_INFO("healthchecker: probing " + pool_->describe() + " every " +
             std::to_string(params_.interval_sec) + "s (fail_after=" +
             std::to_string(params_.fail_after) + " healthy_after=" +
             std::to_string(params_.healthy_after) + ")");
    while (!stop_.load()) {
        size_t n = pool_->size();
        if (fails_.size() != n) {
            fails_.assign(n, 0);
            oks_.assign(n, 0);
        }
        for (size_t i = 0; i < n; ++i) {
            if (stop_.load()) break;
            const Backend* b = pool_->at(i);
            if (b == nullptr) continue;
            bool up = probe(*b);
            if (up) {
                fails_[i] = 0;
                if (oks_[i] < params_.healthy_after) ++oks_[i];
                if (!pool_->isHealthy(i) && oks_[i] >= params_.healthy_after) {
                    pool_->setHealthy(i, true);
                    LOG_WARN("healthchecker: backend " + b->addr() + " DOWN -> UP (" +
                             std::to_string(oks_[i]) + " successes)");
                }
            } else {
                oks_[i] = 0;
                if (fails_[i] < params_.fail_after) ++fails_[i];
                if (pool_->isHealthy(i) && fails_[i] >= params_.fail_after) {
                    pool_->setHealthy(i, false);
                    LOG_WARN("healthchecker: backend " + b->addr() + " UP -> DOWN (" +
                             std::to_string(fails_[i]) + " failures)");
                }
            }
        }
        if (!sleepSlice(params_.interval_sec * 1000)) break;
    }
    LOG_INFO("healthchecker stopped");
}

}  // namespace lb
