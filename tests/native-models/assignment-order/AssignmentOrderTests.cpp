// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompositionFactory.hpp"

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace pipelang::model;
using Startup = assignment_fixture::composition::AssignmentOrder::Startup;
std::size_t assertions = 0;

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

class Port final : public assignment_fixture::source::AssignmentOrder::AssignmentPort {
public:
    Result<std::int64_t> Acquire() override {
        ++acquired;
        if (onAcquire)
            onAcquire();
        return Result<std::int64_t>::Success(acquired);
    }
    Result<void> Release(std::int64_t resource, CancellationToken token) override {
        require(!token.is_cancelled(), "partial cleanup inherited the cancelled request token");
        released.push_back(resource);
        return Result<void>::Success();
    }
    Result<std::int64_t> Mark(std::int64_t phase) override {
        order.push_back(phase);
        return Result<std::int64_t>::Success(phase);
    }
    std::int64_t acquired = 0;
    std::function<void()> onAcquire;
    std::vector<std::int64_t> released;
    std::vector<std::int64_t> order;
};
}

int main() {
    try {
        auto port = std::make_shared<Port>();
        auto created = Startup::Create(port);
        require(created.is_success(), "assignment fixture root creation failed");
        auto root = created.value();
        auto scoped = root->CreateScope();
        require(scoped.is_success(), "assignment fixture scope creation failed");
        auto scope = scoped.value();
        auto resolved = scope->ResolveProbe();
        require(resolved.is_success(), "assignment fixture service resolution failed");
        auto probe = resolved.value();
        CancellationSource first;
        port->onAcquire = [&] { first.cancel(); };
        const auto local = probe.Run(first.token());
        require(local.is_cancelled(), "cancelled local acquisition lost its primary outcome");
        require(port->released == std::vector<std::int64_t>{1},
            "successful acquisition was lost before protected local assignment");
        CancellationSource second;
        port->onAcquire = [&] { second.cancel(); };
        const auto record = probe.RunRecord(second.token());
        require(record.is_cancelled(), "cancelled record acquisition lost its primary outcome");
        require(port->released == std::vector<std::int64_t>({1, 2}),
            "successful acquisition was lost before protected nested-record assignment");
        port->onAcquire = {};
        const auto ordered = probe.Ordered();
        require(ordered.is_success() && ordered.value() == 2, "source receiver assignment failed");
        require(port->order == std::vector<std::int64_t>({1, 2}),
            "source assignment receiver was evaluated twice or after its RHS");
        require(root->Close().is_success(), "assignment fixture close failed");
        std::cout << "assignment order assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
