// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Mixed.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>

int main() {
    for (std::size_t degree : {1U, 4U}) {
        mixed::Mixed model({}, {degree});
        for (int repetition = 0; repetition < 8; ++repetition) {
            const auto array = model.Run();
            assert(array.is_success() && array.value() == 12);
            const auto queue = model.RunQueue();
            assert(queue.is_success() && queue.value() == 9);
            const auto dictionary = model.RunDictionary();
            assert(dictionary.is_success() && dictionary.value() == 11);
            assert(model.pending() == 0);
        }
    }
}
