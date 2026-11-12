// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ResultPropagation.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ResultPropagationChecks.hpp"
#include "ResultPropagationAsyncChecks.hpp"

int main() {
    using namespace propagation_checks;
    result_propagation::ResultPropagation model;
    verify(model);
    assert(model.Pure(IntResult::Success(8)).value() == 9);
    assert(model.Pure(IntResult::Cancelled(error(88))).is_cancelled());
    result_propagation::AsyncPropagation asynchronous;
    verifyAsync(asynchronous, [](auto task) { return task; });
#ifndef __EMSCRIPTEN__
    verifySuspended();
#endif
}
