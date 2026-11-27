// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Boundary.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace pipelang::model;
using namespace empty_interface_fixture;
std::size_t assertions = 0;

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

template<class Function>
void refused(Function function) {
    try {
        function();
    } catch (const ExecutionFailure&) {
        ++assertions;
        return;
    }
    throw std::runtime_error("invalid empty-interface reference was admitted");
}

struct BoundaryState final : ObjectState {
    std::unique_ptr<ObjectState> clone() const override {
        return std::make_unique<BoundaryState>();
    }

    void trace(ObjectTrace&) const override {
    }

    std::size_t logicalBytes() const override {
        return sizeof(BoundaryState);
    }

    void validate() const override {
    }
};

void nativeBoundaries() {
    for (const auto typeId : concreteIds) {
        require(!UnusedTag::accepts(typeId), "empty interface accepted a concrete type");
        require(!TestCaseTag::accepts(typeId), "async-only consumer contains a synchronous implementer");
        require(!ChildTag::accepts(typeId), "base implementation became an empty child implementation");
    }
    require(!UnusedTag::accepts(0), "reserved zero became an empty-interface inhabitant");
    require(!UnusedTag::accepts(std::numeric_limits<std::uint64_t>::max()),
        "unknown type became an empty-interface inhabitant");

    TaskScope scope;
    EvaluationBudget budget;
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    ObjectExecution execution(scope, budget, transaction, {});
    refused([&] { (void)directRead(execution, Unused{}); });
    refused([&] { (void)directLater(execution, Unused{}); });
    require(scope.pending() == 0, "empty async dispatcher admitted a task/frame");

    const auto live = transaction.create<BoundaryState>(LiveTag{});
    require(LiveTag::accepts(live.typeId()), "real closed source class was rejected");
    const auto before = owner.domain()->liveCount();
    refused([&] { (void)live.as<UnusedTag>(); });
    refused([&] { (void)live.as<ChildTag>(); });
    refused([&] { (void)owner.domain()->root<UnusedTag>(live.identity()); });
    refused([&] { (void)transaction.create<BoundaryState>(UnusedTag{}); });
    require(owner.domain()->liveCount() == before, "failed empty-tag construction leaked a provisional cell");

    bool wrongOwner = false;
    std::thread other([&] {
        try {
            (void)live.as<UnusedTag>();
        } catch (const ExecutionFailure&) {
            wrongOwner = true;
        }
    });
    other.join();
    require(wrongOwner, "empty interface conversion bypassed owner confinement");
}

void sourceConsumer() {
    App app;
    require(app.EmptySuite().value() == 0, "imported synchronous runner lost empty-suite behavior");
    require(app.EmptyNullable(), "nullable empty interface required an invented implementation");
    const auto invalidCase = app.InvalidCase(TestCase{});
    require(invalidCase.is_failure(), "native empty-interface argument entered source code");
    require(app.snapshot().marker == 0, "invalid native reference reached an authored side effect");
    refused([&] { (void)app.InvalidPure(Unused{}); });
    refused([&] { (void)app.InvalidChild(Child{}); });
    require(app.Required().is_failure(), "required empty reference produced a valid object");

    const auto invalidAsync = app.InvalidLater(Unused{});
    require(invalidAsync.completed() && invalidAsync.result().is_failure(),
        "async native empty-interface argument was accepted or left pending");

    const auto task = app.Run();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        app.poll();
        require(std::chrono::steady_clock::now() < deadline, "async-only source case failed to settle");
        std::this_thread::yield();
    }
    require(task.result().is_success() && task.result().value() == 17,
        "shared synchronous module prevented the async-only consumer journey");
    require(app.pending() == 0, "empty/inhabited interface operations retained task entries");
}
}

int main() {
    try {
        nativeBoundaries();
        sourceConsumer();
        std::cout << "empty source interfaces: " << assertions << " runtime assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
