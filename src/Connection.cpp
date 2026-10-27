#include "Connection.h"

#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>

#include "Logger.h"

using namespace std;

namespace lb {
namespace {

constexpr size_t kReadChunk = 32 * 1024;

size_t pending(const vector<char>& v, size_t off) {
    return v.size() > off ? v.size() - off : 0;
}

void compact(vector<char>& v, size_t& off) {
    if (off == 0) return;
    if (off >= v.size()) {
        v.clear();
        off = 0;
        return;
    }
    v.erase(v.begin(), v.begin() + static_cast<ptrdiff_t>(off));
    off = 0;
}

}  // namespace

Connection::Connection(int client_fd, int backend_fd, bool backend_connecting)
    : client_(client_fd), backend_(backend_fd), backend_connecting_(backend_connecting) {}

uint32_t Connection::clientInterest() const {
    // Backpressure: no EPOLLIN while client->backend buffer is full.
    // EPOLLOUT only when there is leftover backend->client data.
    // ERR/HUP/RDHUP stay armed so closes are still observed while paused.
    uint32_t m = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (pending(c2b_, c2b_off_) < kMaxBuf && !client_eof_) m |= EPOLLIN;
    if (pending(b2c_, b2c_off_) > 0) m |= static_cast<uint32_t>(EPOLLOUT);
    return m;
}

uint32_t Connection::backendInterest() const {
    uint32_t m = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (pending(b2c_, b2c_off_) < kMaxBuf && !backend_eof_) m |= EPOLLIN;
    if (backend_connecting_ || pending(c2b_, c2b_off_) > 0)
        m |= static_cast<uint32_t>(EPOLLOUT);
    return m;
}

void Connection::shutWr(int fd) noexcept {
    if (fd < 0) return;
    while (::shutdown(fd, SHUT_WR) < 0) {
        if (errno == EINTR) continue;
        break;  // ENOTCONN / already closed etc. — best effort only
    }
}

bool Connection::finishBackendConnect() {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(backend_.get(), SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        err = errno;  // getsockopt itself failed
    }
    if (err != 0) {
        LOG_WARN("backend connect failed: errno=" + std::to_string(err));
        closed_ = true;
        return false;
    }
    backend_connecting_ = false;
    // Client may have already hit EOF while we were connecting; the pending
    // c2b flush + shutdown propagation happens in the event handlers.
    return true;
}

bool Connection::doReadClient() {
    if (client_eof_) return true;
    // Backpressure: buffer full -> leave data in kernel, stop reading until
    // the flush path drains below cap. Listener has EPOLLIN disarmed.
    if (pending(c2b_, c2b_off_) >= kMaxBuf) return true;
    char buf[kReadChunk];
    for (;;) {
        size_t avail = kMaxBuf - pending(c2b_, c2b_off_);
        if (avail == 0) break;  // full -> wait for drain
        size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
        ssize_t n = ::recv(client_.get(), buf, want, 0);
        if (n > 0) {
            c2b_.insert(c2b_.end(), buf, buf + n);
            continue;  // level-triggered: keep draining until EAGAIN/full
        }
        if (n == 0) {
            client_eof_ = true;  // orderly shutdown; flush + propagate later
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        closed_ = true;
        return false;
    }
    return true;
}

bool Connection::doFlushB2C() {
    while (pending(b2c_, b2c_off_) > 0) {
        ssize_t n = ::send(client_.get(), b2c_.data() + b2c_off_,
                           pending(b2c_, b2c_off_), MSG_NOSIGNAL);
        if (n > 0) {
            b2c_off_ += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        // EPIPE / ECONNRESET / other -> close both.
        closed_ = true;
        return false;
    }
    compact(b2c_, b2c_off_);
    return true;
}

bool Connection::doReadBackend() {
    if (backend_eof_) return true;
    if (pending(b2c_, b2c_off_) >= kMaxBuf) return true;
    char buf[kReadChunk];
    for (;;) {
        size_t avail = kMaxBuf - pending(b2c_, b2c_off_);
        if (avail == 0) break;
        size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
        ssize_t n = ::recv(backend_.get(), buf, want, 0);
        if (n > 0) {
            b2c_.insert(b2c_.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            backend_eof_ = true;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        closed_ = true;
        return false;
    }
    return true;
}

bool Connection::doFlushC2B() {
    while (pending(c2b_, c2b_off_) > 0) {
        ssize_t n = ::send(backend_.get(), c2b_.data() + c2b_off_,
                           pending(c2b_, c2b_off_), MSG_NOSIGNAL);
        if (n > 0) {
            c2b_off_ += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        closed_ = true;
        return false;
    }
    compact(c2b_, c2b_off_);
    return true;
}

void Connection::propagateShutdowns() {
    // Client done sending and everything forwarded -> backend sees EOF.
    if (client_eof_ && !backend_connecting_ && pending(c2b_, c2b_off_) == 0 &&
        !backend_shut_wr_) {
        shutWr(backend_.get());
        backend_shut_wr_ = true;
    }
    // Backend done sending and everything flushed -> client sees EOF.
    if (backend_eof_ && pending(b2c_, b2c_off_) == 0 && !client_shut_wr_) {
        shutWr(client_.get());
        client_shut_wr_ = true;
    }
}

bool Connection::shouldClose() const {
    if (closed_) return true;
    // Both directions finished and nothing left to forward -> full close.
    if (client_eof_ && backend_eof_ && pending(c2b_, c2b_off_) == 0 &&
        pending(b2c_, b2c_off_) == 0)
        return true;
    return false;
}

bool Connection::onClientEvent(uint32_t events) {
    if (closed_) return true;
    // Always try normal I/O first (drains data even when ERR/HUP/RDHUP set).
    if (events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        if (!doReadClient()) return true;
        if (!backend_connecting_) {
            if (!doFlushC2B()) return true;
        }
    }
    if (events & EPOLLOUT) {
        if (!doFlushB2C()) return true;
    }
    propagateShutdowns();
    return shouldClose();
}

bool Connection::onBackendEvent(uint32_t events) {
    if (closed_) return true;
    if (events & (EPOLLERR | EPOLLHUP)) {
        // For a connecting socket, EPOLLERR often just means "connect
        // finished — check SO_ERROR". Must not close blindly.
        if (backend_connecting_) {
            if (!finishBackendConnect()) return true;
            if (!doFlushC2B()) return true;
            propagateShutdowns();
            return shouldClose();
        }
        // Established connection error: drain to confirm, then decide.
    }
    if (backend_connecting_ && (events & EPOLLOUT)) {
        if (!finishBackendConnect()) return true;
        if (!doFlushC2B()) return true;
        propagateShutdowns();
        // Fall through: a freshly connected backend may already be readable.
    }
    if (!backend_connecting_ && (events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP))) {
        if (!doReadBackend()) return true;
        if (!doFlushB2C()) return true;
    }
    if (!backend_connecting_ && (events & EPOLLOUT)) {
        if (!doFlushC2B()) return true;
    }
    propagateShutdowns();
    return shouldClose();
}

}  // namespace lb
