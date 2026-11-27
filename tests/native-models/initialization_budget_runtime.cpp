// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/collection.hpp>
#include <pipelang/model/task.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>

using namespace pipelang::model;

namespace {
template<class Callback>
void expectFailure(Callback callback, const char* expected) {
    bool failed = false;
    try {
        callback();
    } catch (const ExecutionFailure& error) {
        failed = true;
        assert(error.what() == std::string(expected));
    }
    assert(failed);
}

template<class Callback>
void expectCancelled(Callback callback) {
    bool cancelled = false;
    try {
        callback();
    } catch (const OperationCancelled& error) {
        cancelled = true;
        assert(error.what() == std::string("parallel operation cancelled"));
    }
    assert(cancelled);
}

void consume(EvaluationBudget& budget, std::size_t steps) {
    for (std::size_t step = 0; step < steps; ++step)
        budget.step();
}
}

int main() {
    static_assert(TaskOptions{}.initializationSteps == 65536);
    static_assert(EvaluationBudget::maximumSteps == 65536);
    static_assert(TaskOptions::maximumInitializationSteps == 1048576);
    for (const auto invalid : {std::size_t(0), maximumInitializationSteps + 1}) {
        expectFailure([&] { (void)EvaluationBudget::forInitialization(invalid); },
            "initialization steps must be between 1 and 1048576");
        expectFailure([&] { TaskScope refused(TaskOptions{4, invalid}); },
            "initialization steps must be between 1 and 1048576");
    }
    {
        auto local = EvaluationBudget::forInitialization(1);
        local.step();
        int checks = 0;
        local.setCancellationCheck([&] { ++checks; return true; });
        expectCancelled([&] { local.step(); });
        assert(checks == 1);
        {
            auto cleanup = local.cleanup();
            local.step();
            assert(checks == 1);
        }
        local.setCancellationCheck([] { return false; });
        expectCancelled([&] { local.step(); });
        assert(checks == 2);
    }
    {
        auto local = EvaluationBudget::forInitialization(2);
        int checks = 0;
        local.setCancellationCheck([&] { ++checks; return false; });
        local.step();
        local.step();
        expectFailure([&] { local.step(); }, "source evaluation budget exceeded");
        assert(checks == 3);
        local.prepareParallel();
        auto child = local.fork();
        expectFailure([&] { child.step(); }, "source evaluation budget exceeded");
        assert(checks == 4);
    }
    auto maximum = EvaluationBudget::forInitialization(maximumInitializationSteps);
    consume(maximum, maximumInitializationSteps);
    expectFailure([&] { maximum.step(); }, "source evaluation budget exceeded");
    {
        auto cleanup = maximum.cleanup();
        consume(maximum, defaultEvaluationSteps);
        expectFailure([&] { maximum.step(); }, "cleanup evaluation budget exceeded");
    }
    auto shared = EvaluationBudget::forInitialization(100000);
    shared.prepareParallel();
    auto first = shared.fork();
    auto second = shared.fork();
    consume(first, 60000);
    consume(second, 40000);
    expectFailure([&] { shared.step(); }, "source evaluation budget exceeded");
    expectFailure([&] { first.step(); }, "source evaluation budget exceeded");
    {
        auto cleanup = second.cleanup();
        consume(second, defaultEvaluationSteps);
        expectFailure([&] { second.step(); }, "cleanup evaluation budget exceeded");
    }
    {
        auto cleanup = first.cleanup();
        expectFailure([&] { first.step(); }, "cleanup evaluation budget exceeded");
    }
    shared.reset();
    consume(second, defaultEvaluationSteps);
    expectFailure([&] { shared.step(); }, "source evaluation budget exceeded");
}
