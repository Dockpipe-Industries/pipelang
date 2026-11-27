// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;

extern "C" const native::HostedModel proof_evaluation_budget;

namespace {

template<class Operation>
std::size_t requiredSteps(std::int64_t expected, bool expectsSuspension, Operation operation)
{
    for (std::size_t budget = 1; budget <= 256; ++budget) {
        const auto [result, suspended] = operation(budget);
        if (result.is_success()) {
            if (result.value() != expected || suspended != expectsSuspension)
                throw std::runtime_error("unexpected authored value or suspension status");
            return budget;
        }
        if (!result.is_failure() || result.error() != "source evaluation budget exceeded")
            throw std::runtime_error(result.error());
    }
    throw std::runtime_error("authored call exceeded the test's maximum budget");
}

template<class Task, class Poll>
auto settle(Task task, Poll poll)
{
    const bool suspended = !task.completed();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("budget fixture did not complete");
        poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::pair{task.result(), suspended};
}

struct BudgetCase {
    std::string_view label;
    std::int64_t expected;
    bool suspends = false;
};

constexpr std::array cases{
    BudgetCase{"simple result", 42},
    BudgetCase{"loop with local source calls", 6},
    BudgetCase{"list construction and iteration", 6},
    BudgetCase{"record assignment", 12},
    BudgetCase{"authored catch and cleanup", 7},
    BudgetCase{"ordinary object construction and call", 5},
    BudgetCase{"immediately completed wait", 11},
    BudgetCase{"implicit model receiver", 1},
    BudgetCase{"explicit model receiver", 1},
    BudgetCase{"call assigned to a local", 1},
    BudgetCase{"loop with arithmetic", 3},
    BudgetCase{"conditional local assignment", 1},
    BudgetCase{"immediate child wait", 4},
    BudgetCase{"suspended child wait", 13, true},
    BudgetCase{"two suspended propagated children", 28, true},
    BudgetCase{"suspended assignment with finally", 16, true},
    BudgetCase{"immediate propagated child and field assignment", 7},
    BudgetCase{"concrete collection iterated through a source interface", 42},
    BudgetCase{"source interface collection and alias identity", 29},
    BudgetCase{"concurrent stack snapshot iteration", 35},
    BudgetCase{"concurrent dictionary snapshot iteration", 26},
    BudgetCase{"array snapshot iteration", 6},
    BudgetCase{"queue snapshot iteration", 23},
    BudgetCase{"stack snapshot iteration", 32},
    BudgetCase{"hash set snapshot iteration", 5},
    BudgetCase{"dictionary snapshot with optional items", 5},
    BudgetCase{"byte buffer with optional widened items", 3},
    BudgetCase{"concurrent queue snapshot iteration", 23},
};

}

int main()
{
    try {
        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto input = static_cast<std::int64_t>(index);
            const auto& test = cases[index];
            const auto referenceSteps = requiredSteps(test.expected, test.suspends, [&](std::size_t budget) {
                model::TaskOptions options;
                options.evaluationStepsPerSegment = budget;
                reference::source::NativeProof::EvaluationBudget::Session session(options);
                return settle(session.Run(input), [&] { session.poll(); });
            });
            const auto nativeSteps = requiredSteps(test.expected, test.suspends, [&](std::size_t budget) {
                model::TaskOptions options;
                options.evaluationStepsPerSegment = budget;
                model::TaskScope scope(options);
                const auto hosted = native::HostedInstance::bind(proof_evaluation_budget, {});
                if (!hosted.is_success())
                    throw std::runtime_error(hosted.error());
                const std::array<native::HostedArgument, 1> arguments{input};
                return settle(hosted.value().start(scope, arguments), [&] { scope.poll(); });
            });
            if (nativeSteps != referenceSteps) {
                throw std::runtime_error(std::string(test.label) + " needs " + std::to_string(nativeSteps) +
                    " native steps and " + std::to_string(referenceSteps) + " reference steps");
            }
        }
        std::cout << "Native and reference evaluation boundaries agree for " << cases.size() << " source paths.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
