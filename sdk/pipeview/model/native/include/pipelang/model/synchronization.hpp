// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "collection.hpp"
#include "task.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace pipelang::model {

class Lock {
    struct Cell {
        std::recursive_timed_mutex mutex;
    };

public:
    class Guard {
    public:
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&) noexcept = default;
        Guard& operator=(Guard&&) = delete;
        ~Guard() = default;

    private:
        friend class Lock;
        explicit Guard(std::shared_ptr<Cell> cell)
            : cell_(std::move(cell)), lock_(cell_->mutex, std::adopt_lock) {
        }

        // Keep the cell alive until after unique_lock releases the mutex.
        std::shared_ptr<Cell> cell_;
        std::unique_lock<std::recursive_timed_mutex> lock_;
    };

    Lock() : cell_(std::make_shared<Cell>()) {
    }

    static Lock Create() {
        return {};
    }

    Guard acquire(EvaluationBudget& budget, CancellationToken cancellation = {}) const {
        requireCell();
        for (;;) {
            budget.step();
            if (!budget.inCleanup() && cancellation.is_cancelled())
                throw OperationCancelled("lock acquisition cancelled");
            if (cell_->mutex.try_lock_for(std::chrono::milliseconds(1)))
                return Guard(cell_);
        }
    }

    void requireCell() const {
        if (!cell_)
            throw ExecutionFailure("lock handle has no cell");
    }

private:
    std::shared_ptr<Cell> cell_;
};

class SemaphoreSlim {
    struct Cell {
        Cell(std::int64_t initial, std::int64_t limit) : count(initial), maximum(limit) {
        }

        std::mutex mutex;
        std::condition_variable changed;
        std::int64_t count;
        std::int64_t maximum;
    };
    using Clock = std::chrono::steady_clock;

public:
    SemaphoreSlim() : SemaphoreSlim(0, maximumCount) {
    }

    static constexpr std::int64_t maximumCount = 4096;
    static constexpr std::int64_t maximumTimeoutMs = 60000;

    static SemaphoreSlim Create(std::int64_t initial, std::int64_t maximum = maximumCount) {
        return SemaphoreSlim(initial, maximum);
    }

    std::int64_t currentCount() const {
        auto& cell = checkedCell();
        std::lock_guard lock(cell.mutex);
        return cell.count;
    }

    std::int64_t release(std::int64_t count = 1) const {
        auto& cell = checkedCell();
        std::int64_t previous;
        {
            std::lock_guard lock(cell.mutex);
            if (count <= 0 || count > cell.maximum - cell.count)
                throw ExecutionFailure("semaphore release exceeds admitted count");
            previous = cell.count;
            cell.count += count;
        }
        cell.changed.notify_all();
        return previous;
    }

    bool wait(EvaluationBudget& budget, std::int64_t timeoutMs = -1,
              CancellationToken cancellation = {}) const {
        validateTimeout(timeoutMs);
        auto& cell = checkedCell();
        const auto deadline = timeoutMs < 0 ? Clock::time_point::max()
            : Clock::now() + std::chrono::milliseconds(timeoutMs);
        bool attempted = false;
        for (;;) {
            budget.step();
            if (!budget.inCleanup() && cancellation.is_cancelled())
                throw OperationCancelled("semaphore wait cancelled");
            std::unique_lock lock(cell.mutex);
            if ((attempted || timeoutMs > 0) && timeoutMs >= 0 && Clock::now() >= deadline)
                return false;
            if (cell.count != 0) {
                --cell.count;
                return true;
            }
            attempted = true;
            if (timeoutMs == 0 || Clock::now() >= deadline)
                return false;
            const auto tick = Clock::now() + std::chrono::milliseconds(1);
            cell.changed.wait_until(lock, std::min(tick, deadline));
        }
    }

    TaskScope::PollingAwaitable waitAsync(TaskScope& scope, std::int64_t timeoutMs = -1,
                                         CancellationToken cancellation = {}) const {
        validateTimeout(timeoutMs);
        requireCell();
        const auto deadline = timeoutMs < 0 ? Clock::time_point::max()
            : Clock::now() + std::chrono::milliseconds(timeoutMs);
        // The owner polls this short predicate; neither its host worker nor a
        // new thread is occupied while the semaphore has no permits.
        return scope.pollUntil(
            [cell = cell_, deadline, timeoutMs, attempted = false]() mutable -> std::optional<bool> {
                std::lock_guard lock(cell->mutex);
                if ((attempted || timeoutMs > 0) && timeoutMs >= 0 && Clock::now() >= deadline)
                    return false;
                if (cell->count != 0) {
                    --cell->count;
                    return true;
                }
                attempted = true;
                if (timeoutMs == 0 || Clock::now() >= deadline)
                    return false;
                return std::nullopt;
            }, std::move(cancellation));
    }

    void requireCell() const {
        (void)checkedCell();
    }

private:
    SemaphoreSlim(std::int64_t initial, std::int64_t maximum) {
        if (maximum < 1 || maximum > maximumCount || initial < 0 || initial > maximum)
            throw ExecutionFailure("semaphore count outside admitted range");
        cell_ = std::make_shared<Cell>(initial, maximum);
    }

    static void validateTimeout(std::int64_t timeoutMs) {
        if (timeoutMs < -1 || timeoutMs > maximumTimeoutMs)
            throw ExecutionFailure("semaphore timeout must be -1 or between 0 and 60000 milliseconds");
    }

    Cell& checkedCell() const {
        if (!cell_)
            throw ExecutionFailure("semaphore handle has no cell");
        return *cell_;
    }

    std::shared_ptr<Cell> cell_;
};

inline void validate(const Lock& value) {
    value.requireCell();
}

inline void validate(const SemaphoreSlim& value) {
    value.requireCell();
}

}
