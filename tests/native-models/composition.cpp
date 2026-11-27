// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Composition.hpp"
#include "InitializerOverloads.hpp"
#include "Modules.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

int main() {
    composition::Composition first;
    assert(first.snapshot().Seed == 0);
    composition::Composition seeded{23};
    assert(seeded.snapshot().Seed == 23);
    assert(first.Evaluate() == 17);
    assert(first.Floating() == 1.5);
    assert(first.IndependentDefaults());
    modules::Entry second;
    assert(second.Evaluate() == 31);
    assert(second.EnumValue());

    initializers::InitializerModel integerInitializer{std::int64_t{7}};
    assert(integerInitializer.snapshot().Value == 1);
    initializers::InitializerModel floatingInitializer{7.0};
    assert(floatingInitializer.snapshot().Value == 2);
    initializers::InitializerModel nanInitializer{std::numeric_limits<double>::quiet_NaN()};
    assert(nanInitializer.snapshot().Value == 2);
    bool overflowRefused = false;
    try {
        initializers::InitializerModel overflowing{std::numeric_limits<std::int64_t>::min()};
        (void)overflowing;
    } catch (const pipelang::model::ExecutionFailure& error) {
        const std::string message = error.what();
        overflowRefused = message.find("integer") != std::string::npos &&
                          message.find("overflow") != std::string::npos;
    }
    assert(overflowRefused);
}
