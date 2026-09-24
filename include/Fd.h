#pragma once

// RAII wrapper for a POSIX file descriptor. Task 1: owns an fd, closes on
// destruction, move-only. No raw fd leaks.
#include <cerrno>
#include <unistd.h>

#include <utility>

namespace lb {

class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    int release() noexcept {
        int f = fd_;
        fd_ = -1;
        return f;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            // Retry on EINTR; ignore other errors in dtor path.
            while (::close(fd_) < 0) {
                if (errno != EINTR) break;
            }
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // namespace lb
