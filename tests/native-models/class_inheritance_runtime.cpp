// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ClassInheritance.hpp"
#include "ClassModules.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace {
using class_inheritance::App;
using IntResult = pipelang::model::Result<std::int64_t, std::string>;

template<class Task>
void finish(App& model, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void stateAndDispatch() {
    App model;
    assert(model.Read() == 115);
    assert(model.InterfaceRead() == 115);
    assert(model.ViaThis() == 115);
    assert(model.BaseOnly() == 5);
    assert(model.ProtectedRead() == 25);
    assert(model.Fixed() == 3);
    assert(model.Identity());
    assert(model.Tag() == 7);
    assert(model.Overload() == 2);
    assert(model.InheritedOverload() == 7);
    assert(model.ConstructorOrder().value() == 10001 * 100000 + 10111 + 11111);
    assert(model.Required().value() == 115);
    assert(model.Missing().value() == 7);
    assert(model.ConstructorDispatch().value() == 212);
    assert(model.ConversionContexts(true).value() == 230);
    assert(model.ConversionContexts(false).value() == 230);
    assert(model.Work(10).value() == 11);
    assert(model.Work(20000).is_failure());
    assert(model.Read() == 115);
    assert(model.Set(20).is_success());
    assert(model.Read() == 130);
    assert(model.InterfaceRead() == 130);
    assert(model.BaseOnly() == 20);
    assert(model.ProtectedRead() == 40);
    assert(model.SetTag(19).is_success());
    assert(model.Tag() == 19);
    assert(model.Link().is_success());
    assert(model.Linked());
    assert(model.Reference() == model.Reference());

    class_modules::App imported;
    assert(imported.Read() == 12);
}

void constructorSavepointsAndRollback() {
    App model;
    for (int iteration = 0; iteration < 4200; ++iteration) {
        const auto recovered = model.Recover(iteration % 2 == 0);
        assert(recovered.is_success());
        assert(recovered.value() == 115);
    }
    assert(model.snapshot().marker == 4200);
    assert(model.Effects().value() == 4200);
    assert(model.Tag() == 7);
    const auto failure = model.Rollback();
    assert(failure.is_failure());
    assert(model.Read() == 115);
    assert(model.Tag() == 7);
    assert(model.snapshot().marker == 4200);
}

void suspendedBaseAndInterfaceViews() {
    App model;
    auto first = model.Later();
    model.poll();
    assert(!first.completed());
    assert(model.Set(30).is_success());
    assert(model.Open().is_success());
    finish(model, first);
    assert(first.result().is_success());
    assert(first.result().value() == 1140);

    auto throughInterface = model.InterfaceLater();
    model.poll();
    assert(!throughInterface.completed());
    assert(model.Set(40).is_success());
    assert(model.Open().is_success());
    finish(model, throughInterface);
    assert(throughInterface.result().value() == 1150);

    auto cancelled = model.Later();
    model.poll();
    assert(!cancelled.completed());
    cancelled.cancel();
    finish(model, cancelled);
    assert(cancelled.result().is_cancelled());
    assert(model.Read() == 150);
}

void ownerAndTaskLifetime() {
    using Reference = decltype(std::declval<App&>().Reference());
    std::optional<Reference> escaped;
    {
        App model;
        escaped.emplace(model.Reference());
        assert(escaped->typeId() != 0);
        bool foreignOwnerRefused = false;
        std::thread worker([&] {
            try {
                escaped->require();
            } catch (const pipelang::model::ExecutionFailure&) {
                foreignOwnerRefused = true;
            }
        });
        worker.join();
        assert(foreignOwnerRefused);
        auto pending = model.Later();
        model.poll();
        assert(!pending.completed());
    }
    bool expiredOwnerRefused = false;
    try {
        escaped->require();
    } catch (const pipelang::model::ExecutionFailure&) {
        expiredOwnerRefused = true;
    }
    assert(expiredOwnerRefused);
}

void physicalStateAndLimits() {
    using namespace class_inheritance;
    using BaseReference = pipelang::model::ObjectRef<pl_object_tag_Base>;
    pipelang::model::ObjectOwner owner;
    {
        pipelang::model::GraphTransaction transaction(owner.domain());
        pl_object_state_Deep initial;
        initial.pl_field_extra = 37;
        auto derived = transaction.create<pl_object_state_Deep>(pl_object_tag_Deep{}, initial);
        auto base = derived.as<pl_object_tag_Base>();
        assert(base.identity() == derived.identity());
        assert(base.typeId() == pl_object_tag_Deep::typeId);
        transaction.mutate<pl_object_state_Base>(base, [](auto& state) { state.pl_field_count = 42; });
        assert(transaction.read<pl_object_state_Deep>(derived).pl_field_extra == 37);
        assert(transaction.read<pl_object_state_Deep>(derived).pl_field_count == 42);
        transaction.commit();
        {
            auto savepoint = transaction.savepoint();
            transaction.mutate<pl_object_state_Base>(base, [](auto& state) { state.pl_field_count = 77; });
            transaction.mutate<pl_object_state_Deep>(derived, [](auto& state) { state.pl_field_extra = 88; });
        }
        assert(transaction.read<pl_object_state_Base>(base).pl_field_count == 42);
        assert(transaction.read<pl_object_state_Deep>(derived).pl_field_extra == 37);
        transaction.mutate<pl_object_state_Base>(base, [&](auto& state) {
            state.pl_field_Next = pipelang::model::ObjectStorage<pipelang::model::Optional<BaseReference>>::encode(
                pipelang::model::Optional<BaseReference>::Some(base), owner.domain());
        });
        transaction.commit();
    }
    owner.domain()->collect();
    assert(owner.domain()->liveCount() == 0);

    {
        pipelang::model::GraphTransaction transaction(owner.domain());
        std::vector<BaseReference> references;
        references.reserve(pipelang::model::ObjectDomain::maximumCells);
        for (std::size_t index = 0; index < pipelang::model::ObjectDomain::maximumCells; ++index) {
            auto derived = transaction.create<pl_object_state_Deep>(pl_object_tag_Deep{});
            references.push_back(derived.as<pl_object_tag_Base>());
        }
        assert(owner.domain()->liveCount() == pipelang::model::ObjectDomain::maximumCells);
        bool refused = false;
        try {
            transaction.create<pl_object_state_Deep>(pl_object_tag_Deep{});
        } catch (const pipelang::model::ExecutionFailure&) {
            refused = true;
        }
        assert(refused);
        assert(owner.domain()->liveCount() == references.size());
        transaction.commit();
    }
    owner.domain()->collect();
    assert(owner.domain()->liveCount() == 0);
}
}

int main() {
    stateAndDispatch();
    constructorSavepointsAndRollback();
    suspendedBaseAndInterfaceViews();
    ownerAndTaskLifetime();
    physicalStateAndLimits();
}
