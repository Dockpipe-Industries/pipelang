// SPDX-License-Identifier: Apache-2.0
#include "Operation.hpp"
#include <thread>

namespace pipelang::sqlite {
Operation::Operation(ConnectionState& state, pipelang::model::CancellationToken cancellation)
    : state_(state), cancellation_(std::move(cancellation)), started_(std::chrono::steady_clock::now()),
      deadline_(started_ + std::chrono::milliseconds(state.options.queryTimeoutMilliseconds)) {
    if (state_.owner != std::this_thread::get_id())
        throw Failure("sqlite.thread: connection belongs to another thread");
    if (!state_.database)
        throw Failure("sqlite.closed: connection is closed");
    if (state_.active)
        throw Failure("sqlite.busy: connection operation is already active");
    Check();
    state_.active = true;
    sqlite3_progress_handler(state_.database, 1000, Progress, this);
    sqlite3_busy_handler(state_.database, Busy, this);
}
Operation::~Operation() {
    sqlite3_progress_handler(state_.database, 0, nullptr, nullptr);
    sqlite3_busy_handler(state_.database, nullptr, nullptr);
    state_.active = false;
}
void Operation::Check() const {
    if (cancellation_.is_cancelled())
        throw pipelang::model::OperationCancelled("sqlite.cancelled: operation cancelled");
    if (std::chrono::steady_clock::now() >= deadline_)
        throw Failure("sqlite.timeout: operation deadline exceeded");
    if (steps_ >= state_.options.maximumVmSteps)
        throw Failure("sqlite.budget: virtual machine work budget exceeded");
}
void Operation::CheckCode(int code) const {
    // A completed SQLite statement is authoritative, including committed writes.
    if (code == SQLITE_DONE)
        return;
    Check();
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE)
        return;
    const int primary = code & 0xff;
    std::string category = "sqlite.error";
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED)
        category = "sqlite.busy";
    else if (primary == SQLITE_READONLY)
        category = "sqlite.read_only";
    else if (primary == SQLITE_FULL || primary == SQLITE_IOERR || primary == SQLITE_CANTOPEN)
        category = "sqlite.storage";
    else if (primary == SQLITE_TOOBIG || primary == SQLITE_NOMEM)
        category = "sqlite.resource_limit";
    throw Failure(category + ": " + sqlite3_errstr(code) + " (" + std::to_string(code) + ")");
}
int Operation::Progress(void* context) noexcept {
    auto& operation = *static_cast<Operation*>(context);
    operation.steps_ += 1000;
    try {
        operation.Check();
        return 0;
    } catch (...) {
        return 1;
    }
}
int Operation::Busy(void* context, int) noexcept {
    auto& operation = *static_cast<Operation*>(context);
    try {
        operation.Check();
        if (std::chrono::steady_clock::now() - operation.started_ >=
            std::chrono::milliseconds(operation.state_.options.busyTimeoutMilliseconds))
            return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 1;
    } catch (...) {
        return 0;
    }
}
}
