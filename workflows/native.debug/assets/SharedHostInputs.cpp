// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <pipelang/model/concurrent_collections.hpp>
#include <chrono>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_borrowed;

void require(bool condition, const char* diagnostic)
{
    if (!condition)
        throw std::runtime_error(diagnostic);
}

template<class Value>
Value take(model::Result<Value> result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return std::move(result).value();
}

int main()
{
    try {
        const auto instance = take(native::HostedInstance::bind(proof_borrowed, {}));
        model::TaskScope scope;
        const auto finish = [&](const auto& task) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!task.completed() && std::chrono::steady_clock::now() < deadline) {
                scope.poll();
                if (!task.completed())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(task.completed(), "the native semaphore wait did not settle");
        };
        const auto permit = model::SemaphoreSlim::Create(0, 1);
        const auto other = model::SemaphoreSlim::Create(0, 1);
        const auto firstValue = native::HostedValue::fromSemaphore(permit);
        const auto secondValue = native::HostedValue::fromSemaphore(other);
        const std::array<native::HostedCallArgument, 2> firstArguments{firstValue, model::CancellationToken{}};
        model::CancellationSource cancellation;
        const std::array<native::HostedCallArgument, 2> otherArguments{secondValue, cancellation.token()};
        const auto first = instance.startMethod(scope, "Run", firstArguments);
        const auto second = instance.startMethod(scope, "Run", otherArguments);
        require(!first.completed() && !second.completed(), "a zero-count borrowed semaphore did not suspend");
        require(permit.release() == 0, "the C++ owner did not see the borrowed semaphore's count");
        finish(first);
        require(take(first.result()).integer() == 42 && permit.currentCount() == 0 && !second.completed(),
                "a release completed the wrong task or copied the borrowed semaphore");

        cancellation.cancel();
        finish(second);
        require(second.result().is_cancelled() && other.currentCount() == 0,
                "cancellation changed the borrowed semaphore or lost the cancellation outcome");

        const std::array<native::HostedCallArgument, 1> signal{firstValue};
        const auto signaled = instance.startMethod(scope, "Signal", signal);
        require(signaled.completed() && take(signaled.result()).integer() == 0 && permit.currentCount() == 1,
                "source release did not affect the original C++ owner");
        const auto ready = instance.startMethod(scope, "Run", firstArguments);
        finish(ready);
        require(take(ready.result()).integer() == 42 && permit.currentCount() == 0,
                "source wait did not consume the original C++ permit");

        const std::array<native::HostedCallArgument, 1> invalid{native::HostedValue::fromInteger(1)};
        const auto refused = instance.startMethod(scope, "Signal", invalid);
        require(refused.completed() && refused.result().is_failure() && permit.currentCount() == 0,
                "a malformed argument crossed the shared semaphore boundary");
        const model::ConcurrentQueue<std::int64_t> fifo;
        const auto sharedQueue = native::HostedValue::fromConcurrentQueue(fifo);
        const std::array<native::HostedCallArgument, 1> queueArgument{sharedQueue};

        fifo.enqueue(-8);
        fifo.enqueue(4);
        const auto observed = instance.startMethod(scope, "ReadQueue", queueArgument);
        require(observed.completed() && take(observed.result()).integer() == -14 && fifo.count() == 0,
                "source peek, removal, snapshot or clear changed the C++ queue contract");

        const std::array<native::HostedCallArgument, 2> inputs{
            sharedQueue, native::HostedValue::fromList("List<int>", {
                native::HostedValue::fromInteger(5), native::HostedValue::fromInteger(10)
            })
        };
        const auto filled = instance.startMethod(scope, "FillQueue", inputs);
        finish(filled);
        const auto firstQueued = fifo.tryDequeue();
        const auto secondQueued = fifo.tryDequeue();
        require(take(filled.result()).integer() == 2 &&
                firstQueued.value() + secondQueued.value() == 15 && fifo.count() == 0,
                "parallel worker captures copied the host queue");

        const std::array<native::HostedCallArgument, 1> wrongHandle{firstValue};
        const auto rejected = instance.startMethod(scope, "ReadQueue", wrongHandle);
        require(rejected.completed() && rejected.result().is_failure() && fifo.count() == 0,
                "an unrelated SDK handle could enter the concurrent queue parameter");
        const auto lock = model::Lock::Create();
        const auto counter = model::Atomic<std::int64_t>::Create(3);
        const std::array<native::HostedCallArgument, 2> guarded{
            native::HostedValue::fromLock(lock), native::HostedValue::fromAtomic(counter)
        };
        std::latch holding{1};
        std::jthread holder([&](std::stop_token stop) {
            auto budget = model::EvaluationBudget::forInvocation(4096);
            const auto guard = lock.acquire(budget);
            holding.count_down();
            while (!stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        holding.wait();
        model::TaskOptions limitedOptions;
        limitedOptions.evaluationStepsPerSegment = 128;
        model::TaskScope limited(limitedOptions);
        const auto blocked = instance.startMethod(limited, "UpdateWithLock", guarded);
        holder.request_stop();
        holder.join();
        require(blocked.completed() && blocked.result().is_failure() && counter.load() == 3,
                "native code did not observe the lock held by the C++ owner");

        const auto unlocked = instance.startMethod(scope, "UpdateWithLock", guarded);
        require(unlocked.completed() && take(unlocked.result()).integer() == 7 && counter.load() == 7,
                "native code lost the shared atomic value after releasing the lock");

        const auto flag = model::Atomic<bool>::Create(false);
        const auto wide = model::Atomic<double>::Create(-0.0);
        const auto narrow = model::Atomic<float>::Create(-0.0f);
        const std::array<native::HostedCallArgument, 3> cells{
            native::HostedValue::fromAtomic(flag), native::HostedValue::fromAtomic(wide),
            native::HostedValue::fromAtomic(narrow)
        };
        const auto changed = instance.startMethod(scope, "UpdateAtomics", cells);
        require(changed.completed() && take(changed.result()).boolean() && flag.load() &&
                wide.load() == 6.5 && narrow.load() == 1.25f,
                "the C++ owner did not observe the native atomic changes or exact floating encodings");

        const std::array<native::HostedCallArgument, 2> wrongAtomic{guarded[0], cells[0]};
        const auto rejectedAtomic = instance.startMethod(scope, "UpdateWithLock", wrongAtomic);
        require(rejectedAtomic.completed() && rejectedAtomic.result().is_failure() && counter.load() == 7,
                "an unrelated atomic shape reached the native method body");
        std::cout << "Native methods preserved the C++ shared synchronization and concurrent queue cells.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
