// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ConstructionPolicy.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {
    using Startup = di_fixture::composition::DiFixture::Bootstrap::Startup;
    struct Outcome { bool success = false; std::string error; };
    std::array<Outcome, 2> outcomes;
    std::array<std::thread, 2> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            try {
                auto policy = std::make_shared<di_tests::ConstructionPolicy>();
                policy->trace = false;
                auto root = Startup::Create(policy);
                if (!root.is_success())
                    throw std::runtime_error(root.error());
                di_fixture::source::DiFixture::Contracts::ScopeStamp stamp;
                stamp.identity = static_cast<std::int64_t>(index + 1);
                stamp.tenant = "independent-owner";
                auto scope = root.value()->CreateScope(stamp);
                if (!scope.is_success())
                    throw std::runtime_error(scope.error());
                auto service = scope.value()->ResolveIUseCase();
                if (!service.is_success())
                    throw std::runtime_error(service.error());
                auto observed = service.value().ScopeIdentity();
                if (!observed.is_success() || observed.value() != stamp.identity)
                    throw std::runtime_error("independent owner observed another scope");
                if (!scope.value()->Close().is_success() || !root.value()->Close().is_success())
                    throw std::runtime_error("independent owner close failed");
                outcomes[index].success = true;
            } catch (const std::exception& error) {
                outcomes[index].error = error.what();
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    for (const auto& outcome : outcomes) {
        if (!outcome.success) {
            std::cerr << "independent owner failure: " << outcome.error << '\n';
            return 1;
        }
    }
    std::cout << "independent owner threads=2 passed\n";
    return 0;
}
