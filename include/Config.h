#pragma once

#include <string>
#include <vector>

namespace lb {

struct BackendEndpoint {
    std::string host;
    int port = 0;
};

struct Config {
    int listen_port = 8080;
    std::vector<BackendEndpoint> backends;
    int workers = 4;

    // Health checking (Task 6). Probe every interval, non-blocking connect
    // with timeout; mark DOWN after fail_after consecutive failures, UP
    // again after healthy_after consecutive successes (anti-flap).
    int health_interval_sec = 2;
    int health_timeout_ms = 1000;
    int fail_after = 2;
    int healthy_after = 2;

    // Rate limiting (Task 7): per-client-IP token bucket. rate_per_sec <= 0
    // disables limiting. burst is the bucket capacity (max short burst).
    double rate_per_sec = 0;
    double burst = 100;

    // Parse command-line args. Throws std::runtime_error on error.
    // Supported flags:
    //   --listen PORT
    //   --backends host:port[,host:port...]
    //   --workers N
    //   --health-interval SEC   (1..3600, default 2)
    //   --health-timeout MS     (50..60000, default 1000)
    //   --fail-after K          (1..100, default 2)
    //   --healthy-after M       (1..100, default 2)
    //   --rate RPS              tokens/sec per IP, 0 = unlimited [default 0]
    //   --burst N               bucket capacity per IP [default 100]
    //   --config PATH   (small key=value file, CLI flags override it)
    //   --help
    static Config parseArgs(int argc, char* argv[]);

    // Load a small key=value config file. Throws on error.
    // Keys: listen_port, workers, backends, health_interval(_sec),
    // health_timeout(_ms), fail_after, healthy_after, rate(_per_sec), burst
    // Example:
    //   listen_port=8080
    //   workers=4
    //   backends=127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003
    //   health_interval=2
    //   health_timeout_ms=1000
    //   fail_after=2
    //   healthy_after=2
    //   rate=0
    //   burst=100
    // Lines starting with '#' are comments. Whitespace is trimmed.
    static Config loadFile(const std::string& path);

    static void printUsage(const char* prog);

    std::string toString() const;
};

}  // namespace lb
