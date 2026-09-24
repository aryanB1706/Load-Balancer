#pragma once

// Small socket helpers shared by Listener / Connection.
#include <fcntl.h>

namespace lb {

// Set O_NONBLOCK on fd. Returns true on success. Never throws.
inline bool setNonBlocking(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (flags & O_NONBLOCK) return true;
    // Retry on EINTR.
    while (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (errno != EINTR) return false;
    }
    return true;
}

}  // namespace lb
