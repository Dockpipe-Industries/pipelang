// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using namespace pipelang::model;
using IntegerResult = Result<std::int64_t>;

template<class Entry>
IntegerResult run(Entry entry, std::int64_t input, std::string_view mode)
{
    constexpr bool hasExplicitTokens = std::is_invocable_v<Entry, ObjectExecution&, std::int64_t,
                                                            CancellationToken, CancellationToken, CancellationToken>;
    const bool cancelBody = mode == "--cancel-body-after-suspension";
    const bool cancelCleanup = mode == "--cancel-cleanup-after-second-suspension";
    if ((cancelBody || cancelCleanup) && !hasExplicitTokens)
        throw ExecutionFailure("reference entry has no explicit cancellation tokens");

    const CancellationSource body;
    const CancellationSource cleanup;
    TaskScope scope;
    EvaluationBudget budget;
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    ObjectExecution execution(scope, budget, transaction, {});
    const auto task = scope.start<IntegerResult>([&](CancellationToken token) {
        if constexpr (hasExplicitTokens)
            return entry(execution, input, body.token(), cleanup.token(), std::move(token));
        else
            return entry(execution, input, std::move(token));
    });
    if (!mode.empty() && task.completed())
        throw ExecutionFailure("reference task did not suspend");
    if (mode == "--cancel-after-suspension")
        task.cancel();
    if (cancelBody)
        body.cancel();

    bool observedSecondWait = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw ExecutionFailure("reference task exceeded the test deadline");
        const auto resumed = scope.poll();
        if (!task.completed()) {
            if (resumed != 0 && !observedSecondWait) {
                observedSecondWait = true;
                if (cancelCleanup)
                    cleanup.cancel();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if ((mode == "--require-two-suspensions" || cancelCleanup) && !observedSecondWait)
        throw ExecutionFailure("reference task did not suspend twice");
    return task.result();
}

}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
        return 64;
    const std::string_view argument(argv[1]);
    const std::string_view mode = argc == 3 ? argv[2] : "";
    std::int64_t input = 0;
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), input);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() ||
        (!mode.empty() && mode != "--require-suspension" && mode != "--require-two-suspensions" &&
         mode != "--cancel-after-suspension" && mode != "--cancel-body-after-suspension" &&
         mode != "--cancel-cleanup-after-second-suspension"))
        return 64;

    try {
        const auto entry = [](ObjectExecution& execution, std::int64_t value, auto&&... tokens)
            -> decltype(reference::pl_object_function_0(execution, value, std::forward<decltype(tokens)>(tokens)...)) {
            return reference::pl_object_function_0(execution, value, std::forward<decltype(tokens)>(tokens)...);
        };
        const auto result = run(entry, input, mode);
        if (result.is_success()) {
            std::cout << result.value() << '\n';
            return 0;
        }
        std::cerr << result.error() << '\n';
        return result.is_cancelled() ? 2 : 1;
    } catch (const std::exception& error) {
        std::cerr << safeErrorMessage(error) << '\n';
        return 1;
    }
}
