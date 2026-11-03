// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "InitializationBudget.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>

using namespace pipelang::model;
using pipelang_initialization::InitializationBudget;

namespace {
template<class Callback>
void expectFailure(Callback callback, const char* message) {
    bool failed = false;
    try {
        callback();
    } catch (const ExecutionFailure& error) {
        failed = true;
        assert(error.what() == std::string(message));
    }
    assert(failed);
}
}

int main() {
    expectFailure([] { InitializationBudget refused(5000); }, "source evaluation budget exceeded");
    TaskOptions selected;
    selected.initializationSteps = 131072;
    InitializationBudget model(5000, selected);
    assert(model.snapshot().value == 10000);
    assert(model.QueryWork(4) == 4);
    const auto before = model.snapshot();
    const auto exhausted = model.Consume(10000);
    assert(exhausted.is_failure() && exhausted.error() == "source evaluation budget exceeded");
    assert(model.snapshot() == before);
    expectFailure([&] { (void)model.QueryWork(10000); }, "source evaluation budget exceeded");
    assert(model.snapshot() == before);
    for (const auto invalid : {std::size_t(0), maximumInitializationSteps + 1}) {
        selected.initializationSteps = invalid;
        expectFailure([&] { InitializationBudget refused(0, selected); },
            "initialization steps must be between 1 and 1048576");
    }
}
