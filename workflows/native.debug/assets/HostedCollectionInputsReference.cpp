// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace source = reference::source::NativeProof::HostedCollectionInputs;
using namespace std::chrono_literals;

int main()
{
    try {
        source::Session session;
        const model::ConcurrentQueue<std::string> queue;
        const model::ConcurrentStack<source::Row> stack;
        queue.enqueue("host");
        stack.push({5});

        if (session.Run(queue, stack).value() != 2 || queue.tryPeek().value() != "native" ||
            stack.tryPeek().value().id != 42)
            throw std::runtime_error("reference source did not retain shared queues and stacks");

        const auto gate = model::SemaphoreSlim::Create(0, 1);
        const auto parked = session.Park(queue, gate, {});
        if (parked.completed())
            throw std::runtime_error("reference source did not suspend");
        gate.release();
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!parked.completed() && std::chrono::steady_clock::now() < deadline)
            session.poll();
        if (!parked.completed() || parked.result().value() != 2 || queue.items().at(1) != "resumed")
            throw std::runtime_error("reference source resumed with a different queue");

        if (session.Workers(queue, stack).value() != 7 || queue.count() != 4 || stack.count() != 3 ||
            session.View(stack).value().at(2).id != 42)
            throw std::runtime_error("reference worker cells or stack order changed");

        const model::ConcurrentDictionary<std::string, source::Row> dictionary;
        dictionary.set("host", {5});
        if (session.Map(dictionary).value() != 42 || dictionary.count() != 2 || dictionary.at("host").id != 17 ||
            session.Remove(dictionary).value() != 17 || dictionary.containsKey("host"))
            throw std::runtime_error("reference dictionary mutating operations lost the host cell");
        if (session.Ensure(dictionary).value() != 83 || dictionary.at("staged").id != 83 ||
            session.MapWorkers(dictionary).value() != 4 || dictionary.at("left").id != 9 || dictionary.at("right").id != 9)
            throw std::runtime_error("reference workers lost the host dictionary");
        if (!session.Clear(dictionary).is_success() || dictionary.count() != 0)
            throw std::runtime_error("reference clear did not affect the host dictionary");

        std::cout << "The C++ reference preserved shared collection identity through workers and async calls.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
