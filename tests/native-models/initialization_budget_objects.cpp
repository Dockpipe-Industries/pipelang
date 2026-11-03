// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "InitializationObjects.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>

using namespace pipelang::model;
using initialization_objects::App;

namespace {
template<class Callback>
void expectLimit(Callback callback) {
    bool failed = false;
    try {
        callback();
    } catch (const EvaluationLimitFailure& error) {
        failed = true;
        assert(error.what() == std::string("source evaluation budget exceeded"));
    }
    assert(failed);
}
}

int main() {
    expectLimit([] { App refused(false); });
    TaskOptions selected;
    selected.initializationSteps = 131072;
    App firstOnly(false, selected);
    assert(firstOnly.First() == 9000 && !firstOnly.HasSecond());
    // The root field initializer and a nested child's field constructor share
    // one allowance. Neither a new object nor a field initializer resets it.
    expectLimit([&] { App refused(true, selected); });
    selected.initializationSteps = 262144;
    App complete(true, selected);
    assert(complete.First() == 9000 && complete.HasSecond());
}
