// SPDX-License-Identifier: Apache-2.0
#include "CooperativeLock.hpp"
#include <sys/file.h>
#include <cerrno>
#include <chrono>
#include <thread>

namespace pipelang::workspace {
CooperativeLock::CooperativeLock(int descriptor, std::timed_mutex& mutex, const Cancellation& cancelled)
    : descriptor_(descriptor), local_(mutex, std::defer_lock) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!local_.try_lock_for(std::chrono::milliseconds(10))) {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw WorkspaceError("busy");
    }
    for (;;) {
        checkCancellation(cancelled);
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) == 0)
            break;
        if (errno != EWOULDBLOCK && errno != EINTR)
            throw WorkspaceError("lock_failed");
        if (std::chrono::steady_clock::now() >= deadline)
            throw WorkspaceError("busy");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
CooperativeLock::~CooperativeLock() {
    ::flock(descriptor_, LOCK_UN);
}
}
