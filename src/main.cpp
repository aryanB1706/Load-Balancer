#include <atomic>
#include <csignal>
#include <iostream>

#include "Backend.h"
#include "Config.h"
#include "Listener.h"
#include "Logger.h"

using namespace std;

namespace {
atomic<lb::Listener*> g_listener{nullptr};

void handleSignal(int) {
    lb::Listener* l = g_listener.load(memory_order_relaxed);
    if (l) l->requestStop();
}
}  // namespace

int main(int argc, char* argv[]) {
    // Never die on EPIPE from a racing client/backend close; we use
    // MSG_NOSIGNAL + EPIPE handling instead.
    ::signal(SIGPIPE, SIG_IGN);

    try {
        lb::Config cfg = lb::Config::parseArgs(argc, argv);
        if (cfg.backends.empty()) {
            LOG_ERROR("no backends configured");
            return 1;
        }
        auto pool = make_shared<lb::BackendPool>(cfg.backends);
        lb::HealthCheckParams hc;
        hc.interval_sec = cfg.health_interval_sec;
        hc.timeout_ms = cfg.health_timeout_ms;
        hc.fail_after = cfg.fail_after;
        hc.healthy_after = cfg.healthy_after;
        lb::RateLimitParams rl;
        rl.rate_per_sec = cfg.rate_per_sec;
        rl.burst = cfg.burst;
        LOG_INFO("tcp_load_balancer starting: listen=" + std::to_string(cfg.listen_port) +
                 " backends=" + pool->describe() + " workers=" + std::to_string(cfg.workers));

        lb::Listener listener(cfg.listen_port, pool, cfg.workers, hc, rl);
        g_listener.store(&listener, memory_order_relaxed);
        ::signal(SIGINT, handleSignal);
        ::signal(SIGTERM, handleSignal);

        listener.run();
        g_listener.store(nullptr, memory_order_relaxed);
        return 0;
    } catch (const exception& e) {
        LOG_ERROR(string("fatal: ") + e.what());
        lb::Config::printUsage(argv[0]);
        return 1;
    }
}
