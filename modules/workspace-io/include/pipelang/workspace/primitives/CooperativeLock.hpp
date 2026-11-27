// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WorkspaceError.hpp"
#include <mutex>

namespace pipelang::workspace {
class CooperativeLock final {
    int descriptor_;
    std::unique_lock<std::timed_mutex> local_;
public:
    CooperativeLock(int descriptor, std::timed_mutex& mutex, const Cancellation& cancelled);
    ~CooperativeLock();
};
}
