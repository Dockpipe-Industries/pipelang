// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "collection.hpp"
#include "task.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

namespace pipelang::model {

struct ParallelOptions {
    std::size_t maximumDegreeOfParallelism = 4;
    static constexpr std::size_t maximumDegree = 32;
};

namespace detail {

class ParallelJoin {
public:
    explicit ParallelJoin(std::vector<std::thread>& workers) : workers_(workers) {
    }

    ParallelJoin(const ParallelJoin&) = delete;

    ParallelJoin& operator=(const ParallelJoin&) = delete;

    ~ParallelJoin() {
        join();
    }

    void join() {
        for (auto& worker : workers_) {
            if (worker.joinable())
                worker.join();
        }
    }

private:
    std::vector<std::thread>& workers_;
};

}

// The compiler admits only isolated callbacks: owned input and immutable
// captures plus explicit Atomic/concurrent collection handles. Ordinary model/host access, awaits and
// nested parallel calls are refused before emission. Shared effects persist
// even when the surrounding ordinary-state transaction fails or is cancelled.
template<class Sequence, class Function>
void parallelForEach(const Sequence& source, EvaluationBudget& budget,
                     CancellationToken cancellation, ParallelOptions options,
                     Function function) {
    if (options.maximumDegreeOfParallelism == 0 ||
        options.maximumDegreeOfParallelism > ParallelOptions::maximumDegree)
        throw ExecutionFailure("parallel degree must be between 1 and 32");
    validate(source);
    // List exposes a const vector reference; other sequence owners may produce
    // their ordered vector by value. Auto owns exactly one stable snapshot in
    // either case, without manufacturing an intermediate List or retaining a
    // borrowed iterator into mutable source storage.
    const auto snapshot = source.values();
    const auto count = snapshot.size();
    budget.prepareParallel();
    if (cancellation.is_cancelled())
        throw OperationCancelled("parallel operation cancelled");
    if (count == 0)
        return;

    const auto degree = std::min(count, options.maximumDegreeOfParallelism);
    std::vector<std::exception_ptr> failures(count);
    std::vector<std::exception_ptr> workerFailures(degree);
    std::vector<std::thread> workers;
    workers.reserve(degree);
    std::atomic<std::size_t> nextIndex{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> cancelled{false};
    // This guard must be destroyed before every object captured by reference,
    // including the scheduling flags, during exceptional submission unwinding.
    detail::ParallelJoin joining(workers);

    try {
        for (std::size_t workerIndex = 0; workerIndex < degree; ++workerIndex) {
            // Each worker owns its callback copy. Copy/construction failures on
            // the owner still pass through the same join boundary.
            workers.emplace_back([&, workerIndex, callback = function]() mutable {
                try {
                    auto childBudget = budget.fork();
                    childBudget.setCancellationCheck([cancellation] {
                        return cancellation.is_cancelled();
                    });
                    while (!stopping.load()) {
                        if (cancellation.is_cancelled()) {
                            cancelled.store(true);
                            return;
                        }
                        const auto index = nextIndex.fetch_add(1);
                        if (index >= count)
                            return;
                        try {
                            callback(snapshot[index], childBudget);
                        } catch (const OperationCancelled&) {
                            cancelled.store(true);
                            return;
                        } catch (...) {
                            // Do not stop after an item fault: lower-index work
                            // must finish so exception choice is scheduling-free.
                            failures[index] = std::current_exception();
                        }
                    }
                } catch (const OperationCancelled&) {
                    cancelled.store(true);
                } catch (...) {
                    workerFailures[workerIndex] = std::current_exception();
                }
            });
        }

    } catch (...) {
        stopping.store(true);
        throw;
    }

    joining.join();
    for (const auto& failure : failures) {
        if (failure)
            std::rethrow_exception(failure);
    }

    for (const auto& failure : workerFailures) {
        if (failure)
            std::rethrow_exception(failure);
    }

    if (cancelled.load())
        throw OperationCancelled("parallel operation cancelled");
}

}
