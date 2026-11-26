// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_composition.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_root;

class Recorder final : public native::HostedService {
public:
    std::string_view interfaceName() const noexcept override
    {
        return "NativeProof.CompositionCleanup.Recorder";
    }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Outcome = model::Result<native::HostedServiceValue>;
        if (signature == "Wait(CancellationToken)" && arguments.size() == 1) {
            const auto& token = std::get<model::CancellationToken>(arguments.front());
            entered.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (!release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!token.is_cancelled())
                return Outcome::Failure("deferred close did not cancel the parked task");
            return Outcome::Cancelled("parked task cancelled");
        }
        if (signature != "Record(int)" || arguments.size() != 1)
            return Outcome::Failure("unexpected cleanup recorder method");
        sequence.push_back(std::get<std::int64_t>(arguments.front()));
        auto callback = std::exchange(duringRecord, {});
        if (callback)
            callback();
        return Outcome::Success(std::int64_t{1});
    }

    std::vector<std::int64_t> sequence;
    std::function<void()> duringRecord;
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
};

template<class Outcome>
auto take(Outcome result)
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

int main()
{
    try {
        using Composition = native::HostedComposition;
        constexpr std::string_view stamp = "NativeProof.CompositionCleanup.Stamp";
        constexpr std::string_view resource = "NativeProof.CompositionCleanup.Resource";
        const auto recorder = std::make_shared<Recorder>();
        const std::array<Composition::Input, 1> shared{{{recorder->interfaceName(), recorder}}};
        const auto root = take(Composition::bind(proof_root, {}, shared));
        const auto scopeFor = [&](std::int64_t identity) {
            const std::array<Composition::Input, 1> inputs{{
                {stamp, native::HostedValue::fromRecord(std::string(stamp),
                    {{"identity", native::HostedValue::fromInteger(identity)}})}
            }};
            return take(root.createScope(inputs));
        };

        const auto first = scopeFor(101);
        const auto second = scopeFor(202);
        const auto independent = scopeFor(303);
        const auto firstHandle = take(first.resolve(resource));
        const auto secondHandle = take(second.resolve(resource));
        (void)take(independent.resolve(resource));
        (void)take(root.resolve("NativeProof.CompositionCleanup.RootResource"));
        require(!firstHandle.sameObject(take(first.resolve(resource))), "transient was unexpectedly cached");
        require(take(root.cleanupStatus()).pending == 5, "composition cleanup journal missed a resource");

        require(independent.close().is_success() && recorder->sequence == std::vector<std::int64_t>{303},
                "child close touched another scope's resource");
        const auto read = take(take(second.tryStartServiceMethod(secondHandle, "Read()", {})).result());
        require(read.integer() == 202, "sibling could not use its preexisting handle");
        const auto closed = root.close();
        require(closed.is_failure() && closed.error() == "di.cleanup_failed",
                "common close outcome lost an authored resource cleanup failure");
        require(recorder->sequence == std::vector<std::int64_t>{303, 101, 900, 202, 101},
                "root close did not follow global reverse acquisition order");
        const auto status = take(root.cleanupStatus());
        require(status.phase == pipelang::di::ScopePhase::Closed && status.pending == 0 && status.attempted == 5 &&
                status.failed == 2 && status.succeeded == 3 && status.unknown == 2,
                "closed root did not preserve resource cleanup outcomes");
        require(root.close().is_failure() && recorder->sequence.size() == 5,
                "root close replayed an already attempted cleanup");
        const auto abortedRecorder = std::make_shared<Recorder>();
        const std::array<Composition::Input, 1> abortedRootInputs{{{abortedRecorder->interfaceName(), abortedRecorder}}};
        const auto abortedRoot = take(Composition::bind(proof_root, {}, abortedRootInputs));
        const std::array<Composition::Input, 1> abortedInputs{{
            {stamp, native::HostedValue::fromRecord(std::string(stamp),
                {{"identity", native::HostedValue::fromInteger(101)}})}
        }};
        const auto abortedScope = take(abortedRoot.createScope(abortedInputs));
        const auto refused = abortedScope.resolve("NativeProof.CompositionCleanup.Failing");
        require(refused.is_failure() && refused.error() == "later constructor failed",
                "unwind replaced the original authored construction failure");
        require(abortedRecorder->sequence == std::vector<std::int64_t>{900, 101},
                "unwind lost a provisional receiver or its reverse creation order");
        const auto aborted = take(abortedRoot.cleanupStatus());
        require(aborted.pending == 0 && aborted.attempted == 2 && aborted.succeeded == 1 &&
                aborted.failed == 1 && aborted.unknown == 1, "unwind failed to record cleanup separately");
        (void)take(abortedRoot.resolve("NativeProof.CompositionCleanup.RootResource"));
        require(abortedRoot.close().is_failure() && abortedRecorder->sequence == std::vector<std::int64_t>{900, 101, 900},
                "failed construction was published or replayed during later closure");

        const auto deferredRecorder = std::make_shared<Recorder>();
        {
            const std::array<Composition::Input, 1> deferredRootInputs{{{deferredRecorder->interfaceName(), deferredRecorder}}};
            const auto deferredRoot = take(Composition::bind(proof_root, {}, deferredRootInputs));
            const std::array<Composition::Input, 1> deferredInputs{{
                {stamp, native::HostedValue::fromRecord(std::string(stamp),
                    {{"identity", native::HostedValue::fromInteger(707)}})}
            }};
            const auto deferred = take(deferredRoot.createScope(deferredInputs));
            const auto receiver = take(deferred.resolve(resource));
            const std::array<Composition::Argument, 1> token{native::HostedCallArgument{model::CancellationToken{}}};
            const auto held = take(deferred.tryStartServiceMethod(receiver, "Hold(CancellationToken)", token));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!deferredRecorder->entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                (void)take(deferredRoot.poll());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(deferredRecorder->entered.load(std::memory_order_acquire) && take(deferred.pending()) == 1,
                    "deferred close fixture never reached pending host work");
            const auto completion = take(deferred.closeAsync());
            require(!completion.completed() && deferred.resolve(resource).is_failure(),
                    "deferred close blocked or admitted new source work");
            deferredRecorder->release.store(true, std::memory_order_release);
            while (!completion.completed() && std::chrono::steady_clock::now() < deadline) {
                (void)take(deferredRoot.poll());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(completion.completed() && completion.result().is_success() && held.result().is_cancelled() &&
                    deferredRecorder->sequence == std::vector<std::int64_t>{707},
                    "deferred close did not settle the callback before disposing its receiver");
            {
                const std::array<Composition::Input, 1> automaticInputs{{
                    {stamp, native::HostedValue::fromRecord(std::string(stamp),
                        {{"identity", native::HostedValue::fromInteger(808)}})}
                }};
                auto automatic = take(deferredRoot.createScope(automaticInputs));
                const auto copy = automatic;
                (void)take(automatic.resolve(resource));
                automatic = deferredRoot;
                require(deferredRecorder->sequence == std::vector<std::int64_t>{707} && take(copy.pending()) == 0,
                        "one facade copy released a context that another copy still owns");
            }
            require(deferredRecorder->sequence == std::vector<std::int64_t>{707, 808},
                    "the final child facade did not request cleanup while its root was alive");
            (void)take(deferredRoot.resolve("NativeProof.CompositionCleanup.RootResource"));
        }
        require(deferredRecorder->sequence == std::vector<std::int64_t>{707, 808, 900},
                "the final native binding did not request resource cleanup");

        const auto reentrantRecorder = std::make_shared<Recorder>();
        const std::array<Composition::Input, 1> reentrantRootInputs{{{reentrantRecorder->interfaceName(), reentrantRecorder}}};
        std::optional<Composition> reentrantRoot(take(Composition::bind(proof_root, {}, reentrantRootInputs)));
        const auto reentrantScope = [&](std::int64_t identity) {
            const std::array<Composition::Input, 1> inputs{{
                {stamp, native::HostedValue::fromRecord(std::string(stamp),
                    {{"identity", native::HostedValue::fromInteger(identity)}})}
            }};
            return take(reentrantRoot->createScope(inputs));
        };
        const auto closing = reentrantScope(802);
        const auto child = reentrantScope(803);
        (void)take(closing.resolve(resource));
        (void)take(child.resolve(resource));
        bool rootClosedToWork = false;
        reentrantRecorder->duringRecord = [&] {
            reentrantRoot.reset();
            const auto refused = child.resolve(resource);
            rootClosedToWork = refused.is_failure() && refused.error() == "di.scope_closing";
        };
        require(closing.close().is_success() && rootClosedToWork,
                "dropping the root in a callback left it open to new work");
        (void)take(child.poll());
        require(reentrantRecorder->sequence == std::vector<std::int64_t>{802, 803},
                "deferred root release did not return after disposing its child");

        const auto budgetRecorder = std::make_shared<Recorder>();
        const std::array<Composition::Input, 1> budgetRootInputs{{{budgetRecorder->interfaceName(), budgetRecorder}}};
        const auto budgetRoot = take(Composition::bind(proof_root, {}, budgetRootInputs));
        const auto budgetScope = [&](std::int64_t value) {
            const std::array<Composition::Input, 1> inputs{{
                {stamp, native::HostedValue::fromRecord(std::string(stamp),
                    {{"identity", native::HostedValue::fromInteger(value)}})}
            }};
            return take(budgetRoot.createScope(inputs));
        };
        const auto earlier = budgetScope(605);
        const auto slow = budgetScope(606);
        (void)take(earlier.resolve(resource));
        (void)take(budgetRoot.resolve("NativeProof.CompositionCleanup.RootResource"));
        (void)take(slow.resolve(resource));
        const auto partial = budgetRoot.close();
        const auto partialStatus = take(budgetRoot.cleanupStatus());
        require(partial.is_failure() && partial.error() == "di.cleanup_pending" &&
                partialStatus.pending == 2 && partialStatus.attempted == 1 &&
                budgetRecorder->sequence == std::vector<std::int64_t>{606},
                "cleanup budget did not retain its unattempted resources");
        const auto budgetRetry = take(budgetRoot.closeAsync());
        require(budgetRetry.completed() && budgetRetry.result().is_failure() &&
                budgetRecorder->sequence == std::vector<std::int64_t>{606, 900, 605},
                "cleanup retry replayed the exhausted callback or lost pending work");

        const auto unwindRecorder = std::make_shared<Recorder>();
        const std::array<Composition::Input, 1> unwindRootInputs{{{unwindRecorder->interfaceName(), unwindRecorder}}};
        const auto unwindRoot = take(Composition::bind(proof_root, {}, unwindRootInputs));
        const std::array<Composition::Input, 1> unwindInputs{{
            {stamp, native::HostedValue::fromRecord(std::string(stamp),
                {{"identity", native::HostedValue::fromInteger(606)}})}
        }};
        const auto stable = take(unwindRoot.resolve("NativeProof.CompositionCleanup.Stable"));
        const auto unwindScope = take(unwindRoot.createScope(unwindInputs));
        const auto exhausted = unwindScope.resolve("NativeProof.CompositionCleanup.FailingSlow");
        require(exhausted.is_failure() && exhausted.error() == "later constructor failed after a slow resource" &&
                take(unwindRoot.cleanupStatus()).pending == 1 &&
                unwindRecorder->sequence == std::vector<std::int64_t>{606},
                "failed construction lost its own outcome or discarded pending cleanup");
        const auto cachedDuringCleanup = unwindRoot.resolve("NativeProof.CompositionCleanup.Stable");
        const auto guardedDuringCleanup = unwindRoot.tryStartServiceMethod(stable, "Read()", {});
        require(cachedDuringCleanup.is_failure() && cachedDuringCleanup.error() == "di.construction_cleanup_pending" &&
                guardedDuringCleanup.is_failure() && guardedDuringCleanup.error() == "di.construction_cleanup_pending",
                "pending failed construction admitted cached resolution or a method");
        require(unwindRoot.close().is_failure() &&
                unwindRecorder->sequence == std::vector<std::int64_t>{606, 900},
                "later close did not finish provisional cleanup exactly once");

        std::weak_ptr<Recorder> releasedPort;
        const auto observed = [&] {
            const auto observedPort = std::make_shared<Recorder>();
            releasedPort = observedPort;
            const std::array<Composition::Input, 1> inputs{{{observedPort->interfaceName(), observedPort}}};
            const auto observedRoot = take(Composition::bind(proof_root, {}, inputs));
            const std::array<Composition::Input, 1> owned{{
                {stamp, native::HostedValue::fromRecord(std::string(stamp),
                    {{"identity", native::HostedValue::fromInteger(101)}})}
            }};
            const auto observedScope = take(observedRoot.createScope(owned));
            (void)take(observedScope.resolve(resource));
            const auto session = take(observedRoot.observeCleanup());
            require(take(session.status()).pending == 1 && !session.task().completed(),
                    "observing cleanup triggered premature disposal");
            return session;
        }();
        const auto observedStatus = take(observed.status());
        require(releasedPort.expired() && observedStatus.phase == pipelang::di::ScopePhase::Closed &&
                observedStatus.failed == 1 && observed.task().completed() && observed.task().result().is_failure(),
                "cleanup observer retained its owner or lost terminal diagnostics");
        bool rejectedThread = false;
        std::thread foreign([&] { rejectedThread = observed.status().is_failure(); });
        foreign.join();
        require(rejectedThread, "cleanup observation accepted a non-owner thread");

        std::cout << "Checked native scopes isolated child closure and recorded reverse root cleanup without replay.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
