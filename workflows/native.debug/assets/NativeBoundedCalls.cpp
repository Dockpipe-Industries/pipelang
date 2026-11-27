// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_bounded;
extern "C" const native::HostedModel proof_plain_bounded;

template<class Value>
Value take(model::Result<Value> result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return std::move(result).value();
}

void require(bool condition, const char* diagnostic)
{
    if (!condition)
        throw std::runtime_error(diagnostic);
}

class StepHost final : public native::HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "BoundedCalls.StepHost"; }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Response = model::Result<native::HostedServiceValue>;
        if (signature == "Step(CancellationToken)" && arguments.size() == 1) {
            ++calls;
            if (std::get<model::CancellationToken>(arguments.front()).is_cancelled())
                return Response::Cancelled("cancelled by fixture");
            return Response::Success(std::int64_t{1});
        }
        if (signature == "Reenter()" && arguments.empty() && reentry)
            return reentry();
        return Response::Failure("unexpected bounded fixture host call");
    }

    std::atomic<int> calls{0};
    std::function<model::Result<native::HostedServiceValue>()> reentry;
};

void depthFailure(const native::HostedValueResult& result)
{
    if (!result.is_failure() || result.error() != "source call depth limit exceeded")
        throw std::runtime_error("source depth limit changed: " + result.error());
}

native::HostedValue depths(std::int64_t value, std::size_t count = 2)
{
    return native::HostedValue::fromList("List<int>", std::vector<native::HostedValue>(count, native::HostedValue::fromInteger(value)));
}

void finish(model::TaskScope& tasks, const model::Task<native::HostedValueResult>& task)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        tasks.poll();
        require(std::chrono::steady_clock::now() < deadline, "bounded task did not settle");
        std::this_thread::yield();
    }
}

int main()
{
    try {
        const auto host = std::make_shared<StepHost>();
        const std::array<native::HostedConstructorArgument, 1> inputs{host};
        const auto app = take(native::HostedInstance::bind(proof_bounded, inputs));
        model::TaskScope tasks;
        const auto start = [&](std::string_view name, std::span<const native::HostedCallArgument> arguments = {}) {
            return app.startMethod(tasks, name, arguments);
        };
        const auto call = [&](std::string_view name, std::span<const native::HostedCallArgument> arguments = {}) {
            const auto task = start(name, arguments);
            finish(tasks, task);
            return task.result();
        };
        const auto one = [&](std::string_view name, native::HostedCallArgument argument) {
            return call(name, std::array<native::HostedCallArgument, 1>{std::move(argument)});
        };
        const auto number = [&](std::string_view name) { return take(call(name)).integer(); };
        const auto snapshot = [&] { return take(app.snapshot()).field("marker").integer(); };

        require(number("NestedRunner") == 7 && number("Visit") == 30, "source receiver dispatch changed");
        require(take(one("Direct", std::int64_t{30})).integer() == 1, "depth 32 was refused");
        const auto before = snapshot();
        depthFailure(one("Direct", std::int64_t{31}));
        require(snapshot() == before, "failed recursive source committed receiver state");
        require(take(one("Siblings", std::int64_t{30})).integer() == 2, "sibling calls accumulated ancestry");
        require(take(one("Pure", std::int64_t{30})).integer() == 1, "pure source depth 32 was refused");
        depthFailure(one("Pure", std::int64_t{31}));
        require(take(one("Mutual", std::int64_t{4})).integer() == 1, "mutual recursion failed");
        depthFailure(one("Mutual", std::int64_t{20}));
        require(take(one("Generic", std::int64_t{30})).integer() == 17, "generic recursion failed");
        depthFailure(one("Generic", std::int64_t{31}));
        require(take(one("Virtual", std::int64_t{4})).integer() == 2, "virtual base dispatch failed");
        depthFailure(one("Virtual", std::int64_t{40}));
        const auto work = call("Work");
        require(work.is_failure() && work.error() == "source evaluation budget exceeded", "source work was unbounded");
        depthFailure(one("CleanupDepth", native::HostedValue::fromBoolean(false)));
        require(number("Cleaned") == 31, "cleanup did not unwind active frames");
        depthFailure(one("CleanupDepth", native::HostedValue::fromBoolean(true)));
        require(number("Cleaned") == 61, "cleanup helper received extra ancestry");
        const auto cleanup = call("CleanupWork");
        require(cleanup.is_failure() && cleanup.error() == "primary fault" && number("Cleaned") > 1,
                "cleanup exhaustion displaced the primary fault");
        require(one("Construct", std::int64_t{30}).is_success() && number("ConstructedCount") == 31,
                "constructor ancestry changed");
        depthFailure(one("Construct", std::int64_t{31}));
        require(number("ConstructedCount") == 31 && one("Construct", std::int64_t{1}).is_success(),
                "constructor depth failure leaked state");
        require(take(one("ParallelRoot", depths(30))).integer() == 2, "root worker lost ancestry");
        depthFailure(one("ParallelRoot", depths(31)));
        require(take(one("ParallelObject", depths(29))).integer() == 2, "object worker lost ancestry");
        depthFailure(one("ParallelObject", depths(30)));
        const auto parallelWork = one("ParallelRoot", depths(1, 4096));
        require(parallelWork.is_failure() && parallelWork.error() == "source evaluation budget exceeded",
                "parallel siblings got independent work allowances");

        for (const bool suspend : {false, true}) {
            std::array<native::HostedCallArgument, 3> arguments{
                std::int64_t{30}, native::HostedValue::fromBoolean(suspend), model::CancellationToken{}};
            require(take(call("LocalAsync", arguments)).integer() == 5, "local async source depth 32 failed");
            require(take(call("ObjectAsync", arguments)).integer() == 3, "interface async source depth 32 failed");
            arguments[0] = std::int64_t{31};
            depthFailure(call("LocalAsync", arguments));
            const auto callsBefore = host->calls.load();
            depthFailure(call("ObjectAsync", arguments));
            require(host->calls.load() - callsBefore == (suspend ? 31 : 0), "rejected child ran host work");
        }

        std::array<native::HostedCallArgument, 2> siblings{std::int64_t{20}, model::CancellationToken{}};
        const auto first = start("AsyncSiblings", siblings);
        const auto second = start("AsyncSiblings", siblings);
        finish(tasks, first);
        finish(tasks, second);
        require(take(first.result()).integer() == 6 && take(second.result()).integer() == 6,
                "interleaved task ancestry changed");
        siblings[0] = std::int64_t{5};
        require(take(call("VirtualAsync", siblings)).integer() == 3, "async base dispatch changed");
        siblings[0] = std::int64_t{30};
        depthFailure(call("VirtualAsync", siblings));
        const auto asyncWork = call("AsyncWork");
        require(asyncWork.is_failure() && asyncWork.error() == "source evaluation budget exceeded",
                "immediate async child reset its parent's budget");

        const std::array<native::HostedCallArgument, 3> waiting{
            std::int64_t{20}, native::HostedValue::fromBoolean(true), model::CancellationToken{}};
        auto cancelled = start("ObjectAsync", waiting);
        cancelled.cancel();
        finish(tasks, cancelled);
        require(cancelled.result().is_cancelled() && tasks.pending() == 0, "async cancellation retained a task");

        host->reentry = [&] { return model::Result<native::HostedServiceValue>::Success(number("Visit")); };
        const auto reentered = call("HostReentry");
        require(reentered.is_failure() && reentered.error().find("reentry") != std::string::npos,
                "synchronous callback reentered the receiver");

        const auto plain = take(native::HostedInstance::bind(proof_plain_bounded, inputs));
        bool wrongThread = false;
        bool groupRejected = false;
        std::thread foreign([&] {
            model::TaskScope other;
            const std::array<native::HostedArgument, 1> arguments{std::int64_t{0}};
            const auto result = plain.start(other, arguments).result();
            wrongThread = result.is_failure() && result.error() == "model accessed from a different owner thread";
            const auto grouped = plain.tryStartMethod(other, other.createGroup(), "Visit", {});
            groupRejected = grouped.is_failure() && grouped.error() == "model accessed from a different owner thread";
        });
        foreign.join();
        require(wrongThread, "plain hosted model admitted a different owner thread");
        require(groupRejected, "bound method admission did not enforce its owner thread");

        std::cout << "Original bounded source call depth, budgets, dispatch, suspension and ownership passed.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
