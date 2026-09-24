#include "Config.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace lb {
namespace {

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

BackendEndpoint parseEndpoint(const std::string& s) {
    // Expect host:port. Host may be an IP or name; port must be 1..65535.
    auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size())
        throw std::runtime_error("bad backend endpoint '" + s + "', want host:port");
    BackendEndpoint ep;
    ep.host = s.substr(0, colon);
    std::string portStr = s.substr(colon + 1);
    try {
        size_t idx = 0;
        int p = std::stoi(portStr, &idx);
        if (idx != portStr.size() || p <= 0 || p > 65535)
            throw std::runtime_error("bad port");
        ep.port = p;
    } catch (const std::exception&) {
        throw std::runtime_error("bad backend endpoint '" + s + "', want host:port");
    }
    return ep;
}

std::vector<BackendEndpoint> parseBackendList(const std::string& s) {
    std::vector<BackendEndpoint> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) out.push_back(parseEndpoint(tok));
    }
    if (out.empty()) throw std::runtime_error("empty backend list");
    return out;
}

int parseInt(const std::string& s, const char* what) {
    try {
        size_t idx = 0;
        int v = std::stoi(trim(s), &idx);
        if (idx != trim(s).size()) throw std::runtime_error("x");
        return v;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("bad value for ") + what + ": '" + s + "'");
    }
}

double parseDouble(const std::string& s, const char* what) {
    try {
        size_t idx = 0;
        double v = std::stod(trim(s), &idx);
        if (idx != trim(s).size()) throw std::runtime_error("x");
        return v;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("bad value for ") + what + ": '" + s + "'");
    }
}

}  // namespace

void Config::printUsage(const char* prog) {
    // Keep on stdout so --help is script-friendly.
    std::string msg =
        std::string("Usage: ") + prog + " [options]\n"
        + "  --listen PORT            listen port (1-65535) [default 8080]\n"
        + "  --backends a:b[,c:d...]  backend list, e.g. 127.0.0.1:9001,127.0.0.1:9002\n"
        + "  --workers N              worker threads (1-64) [default 4]\n"
        + "  --health-interval SEC    probe period (1-3600) [default 2]\n"
        + "  --health-timeout MS      connect timeout (50-60000) [default 1000]\n"
        + "  --fail-after K           DOWN after K failures (1-100) [default 2]\n"
        + "  --healthy-after M        UP after M successes (1-100) [default 2]\n"
        + "  --rate RPS               tokens/sec per IP, 0 = unlimited [default 0]\n"
        + "  --burst N                bucket capacity per IP [default 100]\n"
        + "  --config PATH            key=value file (listen_port, workers, backends, ...)\n"
        + "  --help                   this message\n";
    // Use write() to be EINTR-safe-ish; iostream is fine for a stub main.
    fwrite(msg.c_str(), 1, msg.size(), stdout);
}

Config Config::loadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open config file: " + path);
    Config c;
    bool hasListen = false, hasWorkers = false, hasBackends = false;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error(path + ":" + std::to_string(lineno) + ": want key=value");
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key == "listen_port" || key == "listen") {
            c.listen_port = parseInt(val, "listen_port");
            hasListen = true;
        } else if (key == "workers") {
            c.workers = parseInt(val, "workers");
            hasWorkers = true;
        } else if (key == "backends" || key == "backend") {
            c.backends = parseBackendList(val);
            hasBackends = true;
        } else if (key == "health_interval" || key == "health_interval_sec") {
            c.health_interval_sec = parseInt(val, "health_interval");
        } else if (key == "health_timeout" || key == "health_timeout_ms") {
            c.health_timeout_ms = parseInt(val, "health_timeout_ms");
        } else if (key == "fail_after" || key == "unhealthy_after") {
            c.fail_after = parseInt(val, "fail_after");
        } else if (key == "healthy_after" || key == "success_after") {
            c.healthy_after = parseInt(val, "healthy_after");
        } else if (key == "rate" || key == "rate_per_sec") {
            c.rate_per_sec = parseDouble(val, "rate");
        } else if (key == "burst" || key == "burst_capacity") {
            c.burst = parseDouble(val, "burst");
        } else {
            throw std::runtime_error(path + ":" + std::to_string(lineno) +
                                     ": unknown key '" + key + "'");
        }
    }
    (void)hasListen;
    (void)hasWorkers;
    (void)hasBackends;
    return c;
}

Config Config::parseArgs(int argc, char* argv[]) {
    Config c;
    bool haveBackends = false;
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto needVal = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + flag);
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (a == "--listen") {
            c.listen_port = parseInt(needVal("--listen"), "listen");
        } else if (a == "--backends") {
            c.backends = parseBackendList(needVal("--backends"));
            haveBackends = true;
        } else if (a == "--workers") {
            c.workers = parseInt(needVal("--workers"), "workers");
        } else if (a == "--health-interval") {
            c.health_interval_sec = parseInt(needVal("--health-interval"), "health-interval");
        } else if (a == "--health-timeout") {
            c.health_timeout_ms = parseInt(needVal("--health-timeout"), "health-timeout");
        } else if (a == "--fail-after") {
            c.fail_after = parseInt(needVal("--fail-after"), "fail-after");
        } else if (a == "--healthy-after") {
            c.healthy_after = parseInt(needVal("--healthy-after"), "healthy-after");
        } else if (a == "--rate") {
            c.rate_per_sec = parseDouble(needVal("--rate"), "rate");
        } else if (a == "--burst") {
            c.burst = parseDouble(needVal("--burst"), "burst");
        } else if (a == "--config") {
            configPath = needVal("--config");
        } else {
            throw std::runtime_error("unknown arg '" + a + "' (try --help)");
        }
    }

    if (!configPath.empty()) {
        Config file = loadFile(configPath);
        // CLI flags override file values: re-scan is simplest and robust.
        // Re-derive: start from file, then apply CLI overrides.
        Config merged = file;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--listen") {
                merged.listen_port = parseInt(argv[++i], "listen");
            } else if (a == "--backends") {
                merged.backends = parseBackendList(argv[++i]);
            } else if (a == "--workers") {
                merged.workers = parseInt(argv[++i], "workers");
            } else if (a == "--health-interval") {
                merged.health_interval_sec = parseInt(argv[++i], "health-interval");
            } else if (a == "--health-timeout") {
                merged.health_timeout_ms = parseInt(argv[++i], "health-timeout");
            } else if (a == "--fail-after") {
                merged.fail_after = parseInt(argv[++i], "fail-after");
            } else if (a == "--healthy-after") {
                merged.healthy_after = parseInt(argv[++i], "healthy-after");
            } else if (a == "--rate") {
                merged.rate_per_sec = parseDouble(argv[++i], "rate");
            } else if (a == "--burst") {
                merged.burst = parseDouble(argv[++i], "burst");
            } else if (a == "--config") {
                ++i;  // skip path
            }
        }
        c = merged;
        haveBackends = !c.backends.empty();
        // If user gave neither file backends nor CLI backends, fall through to error below.
        if (!haveBackends && !file.backends.empty()) haveBackends = true;
    }

    if (c.listen_port <= 0 || c.listen_port > 65535)
        throw std::runtime_error("listen port must be 1..65535");
    if (c.workers <= 0 || c.workers > 64)
        throw std::runtime_error("workers must be 1..64");
    if (c.health_interval_sec <= 0 || c.health_interval_sec > 3600)
        throw std::runtime_error("health-interval must be 1..3600 seconds");
    if (c.health_timeout_ms < 50 || c.health_timeout_ms > 60000)
        throw std::runtime_error("health-timeout must be 50..60000 ms");
    if (c.fail_after <= 0 || c.fail_after > 100)
        throw std::runtime_error("fail-after must be 1..100");
    if (c.healthy_after <= 0 || c.healthy_after > 100)
        throw std::runtime_error("healthy-after must be 1..100");
    if (!(c.rate_per_sec >= 0) || c.rate_per_sec > 1000000)
        throw std::runtime_error("rate must be 0..1000000");
    if (!(c.burst >= 1) || c.burst > 1000000)
        throw std::runtime_error("burst must be 1..1000000");
    if (!haveBackends || c.backends.empty())
        throw std::runtime_error("no backends: pass --backends host:port,... or --config file");

    return c;
}

std::string Config::toString() const {
    std::ostringstream os;
    os << "listen_port=" << listen_port << " workers=" << workers << " backends=[";
    for (size_t i = 0; i < backends.size(); ++i) {
        if (i) os << ",";
        os << backends[i].host << ":" << backends[i].port;
    }
    os << "] health_interval=" << health_interval_sec << "s health_timeout=" << health_timeout_ms
       << "ms fail_after=" << fail_after << " healthy_after=" << healthy_after
       << " rate=" << rate_per_sec << "/s burst=" << burst;
    return os.str();
}

}  // namespace lb
