// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_constructor_parallel;

native::HostedResult run(std::int64_t mode, std::size_t degree, std::size_t budget = model::defaultEvaluationSteps)
{
    const std::array<native::HostedConstructorArgument, 1> arguments{mode};
    const auto bound = native::HostedInstance::bind(proof_constructor_parallel, arguments, budget, {degree});
    if (!bound.is_success())
        return native::HostedResult::Failure(bound.error());

    model::TaskScope tasks;
    const auto called = bound.value().start(tasks, {});
    if (!called.completed())
        throw std::runtime_error("the constructed invocation did not complete");
    return called.result();
}

int main()
{
    try {
        const auto refused = run(1, 2);
        const auto serialized = run(2, 1);
        const auto budgetExceeded = run(0, 2, 16);
        const auto retried = run(0, 1);
        const auto parallel = run(2, 2);
        if (!refused.is_failure() || refused.error() != "constructor worker failed")
            throw std::runtime_error("constructor worker failure was lost: " + refused.error());
        if (!serialized.is_failure() || serialized.error() != "constructor worker handoff timed out")
            throw std::runtime_error("constructor ignored a one-worker limit");
        if (!budgetExceeded.is_failure() || budgetExceeded.error() != "source evaluation budget exceeded")
            throw std::runtime_error("constructor workers replaced the initialization budget");
        if (!retried.is_success() || retried.value() != 42 || !parallel.is_success() || parallel.value() != 42)
            throw std::runtime_error("constructor workers failed to share their SDK cells");
        std::cout << "Hosted constructor workers honored degree, budget, cell identity, and direct failure.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
