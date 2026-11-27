// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace model = pipelang::model;
namespace source = reference::source::NativeProof::ConstructorSharedInputs;

int main()
{
    try {
        const model::ConcurrentQueue<std::string> queue;
        const model::ConcurrentDictionary<std::string, source::Value> dictionary;
        const auto calls = model::Atomic<std::int64_t>::Create(0);
        bool failed = false;
        try {
            source::Session rejected(queue, dictionary, calls, 1);
        } catch (const model::ExecutionFailure& failure) {
            failed = std::string_view(failure.what()) == "constructor failed after shared effects";
        }
        if (!failed || queue.count() != 1 || dictionary.at("constructor").id != 17 || calls.load() != 1)
            throw std::runtime_error("reference constructor lost its shared effects");
        source::Session bound(queue, dictionary, calls, 0);
        if (bound.Run().value() != 8 || queue.count() != 3 || dictionary.at("method").id != 42 || calls.load() != 3)
            throw std::runtime_error("reference constructor did not retain host-owned cells");
        std::cout << "The C++ reference borrowed the host cells and preserved failed-constructor effects.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
