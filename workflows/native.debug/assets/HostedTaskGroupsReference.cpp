// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;

class Observer final : public reference::source::NativeProof::HostedGroups::Observer {
public:
    model::Result<std::int64_t> Settle(std::int64_t identity) override
    {
        seen.push_back(identity);
        return model::Result<std::int64_t>::Success(0);
    }

    std::vector<std::int64_t> seen;
};

void require(bool valid, const char* message)
{
    if (!valid)
        throw std::runtime_error(message);
}

int main()
{
    try {
        auto observer = std::make_shared<Observer>();
        reference::source::NativeProof::HostedGroups::Session instance(observer);
        auto first = instance.Wait(101, {});
        auto sibling = instance.Wait(202, {});
        require(!first.completed() && !sibling.completed(), "reference tasks did not suspend");

        auto pump = [&](auto& task) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!task.completed() && std::chrono::steady_clock::now() < deadline) {
                instance.poll();
                std::this_thread::yield();
            }
            require(task.completed(), "reference task did not settle");
        };

        first.cancel();
        pump(first);
        require(first.result().is_cancelled() && !sibling.completed(), "reference cancellation perturbed the sibling");
        require(instance.Release().is_success(), "reference sibling release failed");
        pump(sibling);
        require(sibling.result().is_success() && sibling.result().value() == 202, "reference sibling failed");

        auto last = instance.Wait(303, {});
        last.cancel();
        pump(last);
        require(last.result().is_cancelled(), "reference final cancellation failed");
        const auto count = instance.Settled();
        std::cout << "host";
        for (const auto identity : observer->seen)
            std::cout << ',' << identity;
        std::cout << '\n' << "published";
        for (std::int64_t index = 0; index < count; ++index) {
            const auto settlement = instance.Settlement(index);
            std::cout << ',' << settlement;
        }
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
