#include "Worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "Logger.h"
#include "SocketUtil.h"

namespace lb {
namespace {

constexpr int kMaxEvents = 128;
// Bounded drain after stop: existing conns get this long to finish, then
// force-closed so join() can't hang on idle keep-alives.
constexpr auto kDrainGrace = std::chrono::seconds(5);

void epollDel(int epfd, int fd) {
    ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}

bool epollAdd(int epfd, int fd, uint32_t mask) {
    struct epoll_event ev {};
    ev.events = mask;
    ev.data.fd = fd;
    while (::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

bool epollMod(int epfd, int fd, uint32_t mask) {
    struct epoll_event ev {};
    ev.events = mask;
    ev.data.fd = fd;
    while (::epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

int acceptOne(int listen_fd) {
    for (;;) {
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
        int c = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c >= 0) return c;
        if (errno == ENOSYS || errno == EINVAL) {
#endif
            int c2 = ::accept(listen_fd, nullptr, nullptr);
            if (c2 < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (!setNonBlocking(c2)) {
                while (::close(c2) < 0 && errno == EINTR) {
                }
                return -1;
            }
            return c2;
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
        }
        if (errno == EINTR) continue;
        return -1;
#endif
    }
}

void setReuse(int fd) {
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
}

// Client IP for the rate limiter. Numeric string; "?" on failure (fail-open).
std::string peerIp(int fd) {
    struct sockaddr_storage ss {};
    socklen_t len = sizeof(ss);
    if (::getpeername(fd, reinterpret_cast<struct sockaddr*>(&ss), &len) != 0) return "?";
    char out[INET6_ADDRSTRLEN] = {};
    if (ss.ss_family == AF_INET) {
        auto* a = reinterpret_cast<struct sockaddr_in*>(&ss);
        if (::inet_ntop(AF_INET, &a->sin_addr, out, sizeof(out)) == nullptr) return "?";
    } else if (ss.ss_family == AF_INET6) {
        auto* a = reinterpret_cast<struct sockaddr_in6*>(&ss);
        if (::inet_ntop(AF_INET6, &a->sin6_addr, out, sizeof(out)) == nullptr) return "?";
    } else {
        return "?";
    }
    return out;
}

void closeFd(int fd) {
    while (::close(fd) < 0 && errno == EINTR) {
    }
}

}  // namespace

Worker::Worker(int id, int listen_port, std::shared_ptr<BackendPool> pool,
               std::shared_ptr<RateLimiter> limiter)
    : id_(id), listen_port_(listen_port), pool_(std::move(pool)), limiter_(std::move(limiter)) {}

bool Worker::setupListenSocket(std::string& err) {
    // Each worker binds the SAME port with SO_REUSEPORT; the kernel
    // distributes incoming connections across workers. Fall back to IPv4
    // if dual-stack IPv6 is unavailable.
    Fd s;
    bool v6 = false;

    Fd s6(::socket(AF_INET6, SOCK_STREAM, 0));
    if (s6) {
        int off = 0;
        ::setsockopt(s6.get(), IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        setReuse(s6.get());
        struct sockaddr_in6 a6 {};
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons(static_cast<uint16_t>(listen_port_));
        if (::bind(s6.get(), reinterpret_cast<sockaddr*>(&a6), sizeof(a6)) == 0) {
            s = std::move(s6);
            v6 = true;
        }
    }
    if (!s) {
        Fd s4(::socket(AF_INET, SOCK_STREAM, 0));
        if (!s4) {
            err = "socket() failed";
            return false;
        }
        setReuse(s4.get());
        struct sockaddr_in a4 {};
        a4.sin_family = AF_INET;
        a4.sin_addr.s_addr = htonl(INADDR_ANY);
        a4.sin_port = htons(static_cast<uint16_t>(listen_port_));
        if (::bind(s4.get(), reinterpret_cast<sockaddr*>(&a4), sizeof(a4)) != 0) {
            err = "worker " + std::to_string(id_) + " bind(:" +
                  std::to_string(listen_port_) + ") failed: " + ::strerror(errno);
            return false;
        }
        s = std::move(s4);
    }

    if (!setNonBlocking(s.get())) {
        err = "failed to set listen socket non-blocking";
        return false;
    }
    while (::listen(s.get(), SOMAXCONN) < 0) {
        if (errno == EINTR) continue;
        err = "listen() failed: ";
        err += ::strerror(errno);
        return false;
    }
    listen_fd_ = std::move(s);
    LOG_INFO("worker " + std::to_string(id_) + " listening on :" +
             std::to_string(listen_port_) + (v6 ? " (dual-stack v6, reuseport)" : " (ipv4, reuseport)"));
    return true;
}

bool Worker::setupEpoll(std::string& err) {
    Fd e;
    while (true) {
        int f = ::epoll_create1(EPOLL_CLOEXEC);
        if (f >= 0) {
            e.reset(f);
            break;
        }
        if (errno == EINTR) continue;
        err = "epoll_create1 failed";
        return false;
    }
    if (!epollAdd(e.get(), listen_fd_.get(),
                  static_cast<uint32_t>(EPOLLIN | EPOLLERR))) {
        err = "epoll_ctl ADD listen failed";
        return false;
    }
    epoll_fd_ = std::move(e);
    return true;
}

int Worker::createAndConnectBackend(const Backend& be, bool& connecting,
                                    std::string& err) {
    connecting = false;
    err.clear();

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string port = std::to_string(be.port);
    int rc = ::getaddrinfo(be.host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        err = "getaddrinfo(" + be.host + ":" + port + "): " + ::gai_strerror(rc);
        return -1;
    }

    int out = -1;
    std::string lastErr;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        int s = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (s < 0) {
            if (errno == EINVAL) s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s < 0) {
                lastErr = ::strerror(errno);
                continue;
            }
        }
        if (!setNonBlocking(s)) {
            lastErr = "setNonBlocking failed";
            while (::close(s) < 0 && errno == EINTR) {
            }
            continue;
        }
        int flag = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        int r = ::connect(s, ai->ai_addr, ai->ai_addrlen);
        if (r == 0) {
            connecting = false;
            out = s;
            break;
        }
        if (errno == EINPROGRESS || errno == EINTR || errno == EALREADY) {
            connecting = true;
            out = s;
            break;
        }
        if (errno == EISCONN) {
            connecting = false;
            out = s;
            break;
        }
        lastErr = ::strerror(errno);
        while (::close(s) < 0 && errno == EINTR) {
        }
    }
    ::freeaddrinfo(res);
    if (out < 0) err = lastErr.empty() ? "connect failed" : lastErr;
    return out;
}

void Worker::updateInterest(const std::shared_ptr<Connection>& c) {
    epollMod(epoll_fd_.get(), c->clientFd(), c->clientInterest());
    epollMod(epoll_fd_.get(), c->backendFd(), c->backendInterest());
}

void Worker::closeConnection(const std::shared_ptr<Connection>& c) {
    int cf = c->clientFd();
    int bf = c->backendFd();
    epollDel(epoll_fd_.get(), cf);
    epollDel(epoll_fd_.get(), bf);
    fd_map_.erase(cf);
    fd_map_.erase(bf);
}

void Worker::handleAccept() {
    for (;;) {
        int c = acceptOne(listen_fd_.get());
        if (c < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
                LOG_WARN("worker " + std::to_string(id_) + ": accept fd exhaustion");
                break;
            }
            if (errno == ECONNABORTED || errno == EINTR) continue;
            if (stop_.load()) break;  // listen closed during shutdown
            LOG_WARN("worker " + std::to_string(id_) +
                     std::string(" accept failed: ") + ::strerror(errno));
            break;
        }

        int flag = 1;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        // Task 7: per-IP token bucket. Reject before touching any backend.
        if (limiter_ && limiter_->enabled()) {
            std::string ip = peerIp(c);
            if (ip != "?" && !limiter_->allow(ip)) {
                rejected_.fetch_add(1);
                LOG_INFO("worker " + std::to_string(id_) + ": rate limited " + ip +
                         ", closing");
                closeFd(c);
                continue;
            }
        }

        // Task 5: lock-free round-robin pick; skip unhealthy backends.
        const Backend* be = pool_ ? pool_->pick() : nullptr;
        if (be == nullptr) {
            LOG_WARN("worker " + std::to_string(id_) +
                     ": all backends down, closing client");
            while (::close(c) < 0 && errno == EINTR) {
            }
            continue;
        }

        bool connecting = false;
        std::string berr;
        int b = createAndConnectBackend(*be, connecting, berr);
        if (b < 0) {
            LOG_WARN("worker " + std::to_string(id_) + " backend " + be->addr() +
                     " connect failed: " + berr);
            while (::close(c) < 0 && errno == EINTR) {
            }
            continue;
        }
        LOG_INFO("worker " + std::to_string(id_) + ": client fd=" +
                 std::to_string(c) + " -> backend " + be->addr());

        auto conn = std::make_shared<Connection>(c, b, connecting);
        if (!epollAdd(epoll_fd_.get(), c, conn->clientInterest()) ||
            !epollAdd(epoll_fd_.get(), b, conn->backendInterest())) {
            LOG_WARN("worker " + std::to_string(id_) + " epoll add failed, dropping");
            epollDel(epoll_fd_.get(), c);
            epollDel(epoll_fd_.get(), b);
            continue;
        }
        fd_map_[c] = conn;
        fd_map_[b] = conn;
        accepted_.fetch_add(1);
    }
}

void Worker::handleEvent(int fd, uint32_t events) {
    if (fd == listen_fd_.get()) {
        if (!stop_.load()) handleAccept();
        return;
    }
    auto it = fd_map_.find(fd);
    if (it == fd_map_.end()) return;
    std::shared_ptr<Connection> conn = it->second;

    bool wantClose = false;
    if (fd == conn->clientFd()) {
        wantClose = conn->onClientEvent(events);
    } else if (fd == conn->backendFd()) {
        wantClose = conn->onBackendEvent(events);
    } else {
        return;
    }

    if (wantClose || conn->closed()) {
        closeConnection(conn);
    } else {
        updateInterest(conn);
    }
}

void Worker::loop() {
    struct epoll_event evs[kMaxEvents];
    bool draining = false;
    auto stopAt = std::chrono::steady_clock::now();

    while (true) {
        if (stop_.load() && !draining) {
            // Phase 1: stop accepting immediately.
            draining = true;
            stopAt = std::chrono::steady_clock::now();
            epollDel(epoll_fd_.get(), listen_fd_.get());
            listen_fd_.reset();  // close listen socket; kernel sends new SYNs elsewhere
            LOG_INFO("worker " + std::to_string(id_) + " stopped accepting, draining " +
                     std::to_string(fd_map_.size() / 2) + " conns");
        }
        if (draining) {
            if (fd_map_.empty()) break;
            if (std::chrono::steady_clock::now() - stopAt > kDrainGrace) {
                LOG_WARN("worker " + std::to_string(id_) + " drain timeout, force-closing " +
                         std::to_string(fd_map_.size() / 2) + " conns");
                break;  // fd_map_ clear below closes all Fds
            }
        }
        int n = ::epoll_wait(epoll_fd_.get(), evs, kMaxEvents, 200 /*ms*/);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("worker " + std::to_string(id_) +
                      std::string(" epoll_wait failed: ") + ::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            handleEvent(evs[i].data.fd, evs[i].events);
        }
        if (!draining && stop_.load()) continue;  // re-evaluate drain entry promptly
        if (!draining && fd_map_.empty() && stop_.load()) break;
    }
    fd_map_.clear();  // closes all remaining client+backend Fds
}

void Worker::run() {
    std::string err;
    if (!setupListenSocket(err)) {
        LOG_ERROR(err);
        return;
    }
    if (!setupEpoll(err)) {
        LOG_ERROR("worker " + std::to_string(id_) + ": " + err);
        return;
    }
    loop();
    LOG_INFO("worker " + std::to_string(id_) + " exited, accepted=" +
             std::to_string(accepted_.load()) + " rejected=" +
             std::to_string(rejected_.load()));
}

}  // namespace lb
