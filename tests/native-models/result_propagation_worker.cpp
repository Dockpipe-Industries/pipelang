// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ResultPropagationWorker.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main() {
    result_propagation_worker::App model;
    const auto outcome = model.Run();
    assert(outcome.is_success() && outcome.value() == 8);
}
