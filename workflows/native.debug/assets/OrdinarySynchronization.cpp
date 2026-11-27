// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;
using Value = native::HostedValue;
using Argument = native::HostedCallArgument;

extern "C" const native::HostedModel proof_synchronization;

void require(bool condition, const char* diagnostic)
{
    if (!condition)
        throw std::runtime_error(diagnostic);
}

template<class T>
T take(model::Result<T> outcome)
{
    if (!outcome.is_success())
        throw std::runtime_error(outcome.error());
    return std::move(outcome).value();
}

class Session {
public:
    Session() : instance_(take(native::HostedInstance::bind(proof_synchronization, {}))) {}

    auto start(std::string_view name, std::span<const Argument> arguments = {})
    {
        return instance_.startMethod(tasks_, name, arguments);
    }

    auto run(std::string_view name, std::span<const Argument> arguments = {})
    {
        const auto task = start(name, arguments);
        finish(task);
        return task.result();
    }

    auto withInteger(std::string_view name, std::int64_t value)
    {
        const std::array<Argument, 1> arguments{value};
        return run(name, arguments);
    }

    auto snapshot() const { return take(instance_.snapshot()); }

    template<class Predicate>
    void await(Predicate ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            tasks_.poll();
            if (!ready())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(ready(), "a synchronization event did not arrive");
    }

    template<class Task>
    void finish(const Task& task)
    {
        await([&] { return task.completed(); });
    }

    std::int64_t read(std::string_view name)
    {
        return take(run(name)).integer();
    }

private:
    model::TaskScope tasks_;
    native::HostedInstance instance_;
};

int main()
{
    try {
        Session locks;
        std::vector<Value> values;
        for (std::int64_t index = 0; index < 8; ++index)
            values.push_back(Value::fromInteger(index));
        const std::array<Argument, 1> items{Value::fromList("List<int>", std::move(values))};
        require(locks.read("LockReturn") == 3 && locks.read("LockLoop") == 2, "source lock transfer changed");
        require(locks.run("LockFault").is_failure() && locks.snapshot().field("marker").integer() == 0,
                "lock failure committed an ordinary field");
        require(take(locks.run("ParallelLocked", items)).integer() == 16, "nested worker lock call lost shared state");
        require(locks.run("LockBudget").is_failure() && locks.snapshot().field("marker").integer() == 0,
                "lock budget failure escaped the transaction");
        require(take(locks.run("ParallelLocked", items)).integer() == 16, "lock guard survived a budget failure");
        require(take(locks.run("LocalSemaphore")).boolean() && locks.read("ExplicitLock") == 4 &&
                locks.read("SimpleWait") == 1 && locks.run("InvalidCreate").is_failure(),
                "ordinary lock or semaphore behavior changed");
        require(take(locks.run("AsyncAlias")).boolean() &&
                take(locks.run("SynchronousWorkerWait", items)).integer() == 8,
                "inherited semaphore aliasing or worker acquisition changed");

        Session fifo;
        const std::array<Argument, 2> eleven{std::int64_t{11}, model::CancellationToken{}};
        const std::array<Argument, 2> twentyTwo{std::int64_t{22}, model::CancellationToken{}};
        const auto first = fifo.start("Work", eleven);
        const auto second = fifo.start("Work", twentyTwo);
        require(!first.completed() && !second.completed() && fifo.read("EnteredCount") == 0,
                "a waiter passed a zero-count semaphore");
        require(take(fifo.withInteger("ReleasePermit", 1)).integer() == 0, "permit release previous count changed");
        fifo.await([&] { return fifo.read("EnteredCount") == 1; });
        require(fifo.read("PermitCount") == 0 && fifo.read("CleanedCount") == 0,
                "first waiter skipped its suspended segment");
        require(fifo.run("FinishOne").is_success(), "first finish signal failed");
        fifo.await([&] { return fifo.read("EnteredCount") == 2; });
        require(fifo.read("CleanedCount") == 1 && fifo.run("FinishOne").is_success(), "second finish signal failed");
        fifo.finish(first);
        fifo.finish(second);
        require(take(first.result()).integer() == 11 && take(second.result()).integer() == 22 &&
                fifo.read("CleanedCount") == 2 && fifo.read("PermitCount") == 1 &&
                fifo.snapshot().field("marker").integer() == 2,
                "independent suspended tasks lost results, cleanup, or source writes");

        Session cancelled;
        const std::array<Argument, 1> token{model::CancellationToken{}};
        const auto waiting = cancelled.start("ObserveCancelledWait", token);
        require(!waiting.completed() && cancelled.withInteger("EditMarker", 47).is_success(),
                "expected a pending wait before editing the marker");
        waiting.cancel();
        cancelled.finish(waiting);
        require(waiting.result().is_cancelled() && cancelled.read("CleanupObserved") == 47 &&
                cancelled.snapshot().field("marker").integer() == 47,
                "cancelled wait cleanup did not read the latest committed marker");

        Session invalid;
        const auto fault = invalid.start("AcquiredFault");
        require(!fault.completed() && invalid.withInteger("ReleasePermit", 1).is_success(),
                "the fault fixture did not wait for a permit");
        invalid.finish(fault);
        require(fault.result().is_failure() && fault.result().error() == "acquired source fault" &&
                invalid.read("CleanedCount") == 1 && invalid.read("PermitCount") == 1,
                "fault after acquisition failed to settle its cleanup");
        require(invalid.withInteger("ReleasePermit", 0).is_failure() &&
                invalid.withInteger("ReleasePermit", 4096).is_failure() &&
                invalid.read("PermitCount") == 1, "a rejected release changed stored permits");
        for (std::int64_t timeout : {-2, 60001}) {
            const std::array<Argument, 2> arguments{timeout, model::CancellationToken{}};
            require(invalid.run("Timed", arguments).is_failure(), "invalid semaphore timeout was accepted");
        }

        Session boundary;
        require(boundary.withInteger("ReleasePermit", 1).is_success(), "could not prepare the cancellation boundary");
        model::CancellationSource cancellation;
        cancellation.cancel();
        const std::array<Argument, 2> pollCancelled{std::int64_t{0}, cancellation.token()};
        require(boundary.run("Timed", pollCancelled).is_cancelled() && boundary.read("PermitCount") == 1,
                "a cancelled immediate wait consumed a permit");
        const std::array<Argument, 2> pollReady{std::int64_t{0}, model::CancellationToken{}};
        require(take(boundary.run("Timed", pollReady)).boolean() && boundary.read("PermitCount") == 0,
                "an immediate wait failed to consume the available permit");
        const std::array<Argument, 2> timed{std::int64_t{1}, model::CancellationToken{}};
        require(!take(boundary.run("Timed", timed)).boolean(), "a timed wait consumed a nonexistent permit");

        Session budgets;
        const auto immediate = budgets.run("ImmediateBudget");
        require(immediate.is_failure() && immediate.error().find("budget") != std::string::npos,
                "immediately ready awaits reset the evaluation budget");
        const auto suspended = budgets.start("SuspendedBudget", token);
        if (suspended.completed())
            throw std::runtime_error("the source exhausted its segment budget before waiting: " + suspended.result().error());
        require(budgets.withInteger("ReleasePermit", 1).is_success(), "could not release the budget test wait");
        budgets.finish(suspended);
        require(take(suspended.result()).integer() == 10000 && budgets.read("PermitCount") == 1,
                "a real suspension did not reset the budget or preserve cleanup");
        std::cout << "Original synchronization cases matched the reference values and effects.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
