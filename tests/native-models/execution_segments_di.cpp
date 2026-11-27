// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionFactory.hpp"
#include "ExecutionSegmentsHost.hpp"

#include <pipelang/di/ScopeExecution.hpp>
#include <iostream>

namespace {
using namespace segment_test;
using Worker = execution_segments::source::Segments::SegmentWork;
using Service = execution_segments::source::Segments::ISegmentWork;
using Seed = execution_segments::source::Segments::WorkSeed;
using Constructor = execution_segments::construction::SourceConstructor<Worker>;
using Methods = execution_segments::construction::SourceMethods<Service>;

void profile(std::size_t allowance, bool admitted) {
    auto created = pipelang::di::ExecutionOwner::Create(options(allowance));
    require(created.is_success(), "DI owner refused valid segment profile");
    auto owner = created.value();
    const auto root = owner->rootExecution();
    require(root.is_success(), "DI root execution unavailable");
    auto scope = root.value();
    const auto constructed = scope->construct<Worker>({}, [](ObjectExecution& execution, pipelang::di::ConstructionAttempt&) {
        Seed seed;
        seed.iterations = 0;
        return Constructor::Create(execution, seed);
    });
    require(constructed.is_success(), "bounded ordinary construction failed");
    const auto service = Service(constructed.value());
    const auto queried = scope->invoke<std::int64_t>({}, [&](ObjectExecution& execution) {
        return Methods::Read(execution, service, 10000);
    });
    require(queried.is_success() == admitted, "DI invoke ignored selected segment profile");
    if (admitted)
        require(queried.value() == 10000, "DI source query result changed");
    else
        require(queried.error() == "source evaluation budget exceeded", "DI query exhaustion identity changed");

    const auto largeConstruction = scope->construct<Worker>({}, [](ObjectExecution& execution, pipelang::di::ConstructionAttempt&) {
        Seed seed;
        seed.iterations = 10000;
        return Constructor::Create(execution, seed);
    });
    require(largeConstruction.is_success() == admitted, "DI construct ignored its root's selected profile");
    if (admitted) {
        const auto observed = scope->invoke<std::int64_t>({}, [&](ObjectExecution& execution) {
            return Methods::Seed(execution, Service(largeConstruction.value()));
        });
        require(observed.is_success() && observed.value() == 10000, "DI source constructor result changed");
    }

    const auto started = scope->start<std::int64_t>({}, [service](ObjectExecution& execution, CancellationToken cancellation) {
        return Methods::ReadAsync(execution, service, 10000, cancellation);
    });
    require(started.is_success(), "DI async operation was not admitted");
    const auto task = started.value();
    pumpUntil([&] { require(owner->poll().is_success(), "DI poll failed"); }, [&] { return task.completed(); });
    require(task.result().is_success() == admitted, "DI async root ignored selected segment profile");
    if (admitted)
        require(task.result().value() == 10000, "DI source async result changed");
    else
        require(task.result().error() == "source evaluation budget exceeded", "DI async exhaustion identity changed");

    CancellationSource cancellation;
    cancellation.cancel();
    bool entered = false;
    const auto cancelled = scope->invoke<std::int64_t>(cancellation.token(), [&](ObjectExecution&) {
        entered = true;
        return std::int64_t{0};
    });
    require(cancelled.is_cancelled() && !entered, "DI admission cancellation changed");
    require(owner->close().is_success(), "DI close/independent cleanup roots failed");
}
} // namespace

int main() {
    try {
        const auto invalid = pipelang::di::ExecutionOwner::Create(options(0));
        require(invalid.is_failure() && invalid.error() == "evaluation steps per segment must be positive",
                "DI owner admitted zero segment allowance");
        profile(65536, false);
        profile(131072, true);
        std::cout << "generated source DI invoke/construct/async execution segment contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
