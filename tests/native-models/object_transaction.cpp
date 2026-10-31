// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/object_model.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>

using namespace pipelang::model;

namespace {

struct CounterTag {
    static constexpr std::uint64_t typeId = 1;

    static bool accepts(std::uint64_t value) {
        return value == typeId;
    }
};

struct CounterState final : ObjectState {
    std::int64_t value = 5;
    static inline bool failPreparation = false;
    static inline int clones = 0;
    static inline int traces = 0;
    static inline int validations = 0;

    std::unique_ptr<ObjectState> clone() const override {
        ++clones;
        return std::make_unique<CounterState>(*this);
    }

    void trace(ObjectTrace&) const override {
        ++traces;
    }

    std::size_t logicalBytes() const override {
        return sizeof(CounterState);
    }

    void validate() const override {
        ++validations;
        if (value < 0)
            throw ExecutionFailure("negative counter");
    }

    void validateInitialized() const override {
        validate();
        // This represents a failure in graph preparation, after root validation
        // and before either root state or graph state has been published.
        if (failPreparation)
            throw std::bad_alloc();
    }
};

struct RootState {
    std::string first = "original first";
    std::string second = "original second";
    ObjectRef<CounterTag> child;
    static inline bool failCopy = false;
    static inline bool failValidation = false;
    static inline int copyAssignments = 0;

    RootState() = default;

    RootState(const RootState& other) : first(other.first) {
        if (failCopy)
            throw std::bad_alloc();
        second = other.second;
        child = other.child;
    }

    RootState& operator=(const RootState& other) {
        ++copyAssignments;
        first = other.first;
        if (failCopy)
            throw std::bad_alloc();
        second = other.second;
        child = other.child;
        return *this;
    }

    RootState(RootState&&) noexcept = default;
    RootState& operator=(RootState&&) noexcept = default;

    friend void swap(RootState& left, RootState& right) noexcept {
        using std::swap;
        swap(left.first, right.first);
        swap(left.second, right.second);
        swap(left.child, right.child);
    }

    friend void validate(const RootState& value) {
        if (failValidation)
            throw ExecutionFailure("root preparation refused");
        pipelang::model::validate(value.first);
        pipelang::model::validate(value.second);
        value.child.require();
    }
};

struct Fixture {
    ObjectOwner owner;
    RootState live;

    Fixture() {
        GraphTransaction initial(owner.domain());
        live.child = initial.create<CounterState>(CounterTag{});
        initial.commit();
    }

    std::int64_t publishedCounter() {
        GraphTransaction inspection(owner.domain());
        return inspection.read<CounterState>(live.child).value;
    }
};

void jointPublicationAndPreparation() {
    Fixture fixture;
    GraphModelTransaction<RootState> transaction(fixture.live, fixture.owner.domain());
    transaction.state().first = "prepared first";
    transaction.state().second = "prepared second";
    transaction.graph().mutate<CounterState>(fixture.live.child,
        [](CounterState& state) { state.value = 7; });

    RootState::failValidation = true;
    bool refused = false;
    try {
        transaction.commit();
    } catch (const ExecutionFailure&) {
        refused = true;
    }
    RootState::failValidation = false;
    assert(refused);
    assert(fixture.live.first == "original first" && fixture.live.second == "original second");
    assert(fixture.publishedCounter() == 5);
    assert(transaction.graph().read<CounterState>(fixture.live.child).value == 7);

    CounterState::failPreparation = true;
    bool allocationFailed = false;
    try {
        transaction.commit();
    } catch (const std::bad_alloc&) {
        allocationFailed = true;
    }
    CounterState::failPreparation = false;
    assert(allocationFailed);
    assert(fixture.live.first == "original first" && fixture.live.second == "original second");
    assert(fixture.publishedCounter() == 5);

    transaction.commit();
    assert(fixture.live.first == "prepared first" && fixture.live.second == "prepared second");
    assert(fixture.publishedCounter() == 7);

    transaction.refresh();
    TaskScope scope;
    EvaluationBudget budget;
    auto execution = transaction.execution(scope, budget);
    transaction.state().first = "callback first";
    transaction.graph().mutate<CounterState>(fixture.live.child,
        [](CounterState& state) { state.value = 8; });
    execution.commitSegment();
    assert(fixture.live.first == "callback first" && fixture.publishedCounter() == 8);
    execution.refreshSegment();
    assert(transaction.state().first == "callback first");
    assert(transaction.graph().read<CounterState>(fixture.live.child).value == 8);

    const auto clonesBefore = CounterState::clones;
    const auto tracesBefore = CounterState::traces;
    const auto validationsBefore = CounterState::validations;
    fixture.owner.domain()->collect();
    assert(CounterState::clones == clonesBefore);
    assert(CounterState::traces == tracesBefore);
    assert(CounterState::validations == validationsBefore);
}

void refreshStagesBeforeMutation() {
    Fixture fixture;
    GraphModelTransaction<RootState> retained(fixture.live, fixture.owner.domain());
    retained.state().first = "retained first";
    retained.state().second = "retained second";
    retained.graph().mutate<CounterState>(fixture.live.child,
        [](CounterState& state) { state.value = 9; });
    {
        GraphModelTransaction<RootState> newer(fixture.live, fixture.owner.domain());
        newer.state().first = "new live first";
        newer.state().second = "new live second";
        newer.graph().mutate<CounterState>(fixture.live.child,
            [](CounterState& state) { state.value = 11; });
        newer.commit();
    }
    RootState::copyAssignments = 0;
    RootState::failCopy = true;
    bool failed = false;
    try {
        retained.refresh();
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    RootState::failCopy = false;
    assert(failed);
    assert(retained.state().first == "retained first");
    assert(retained.state().second == "retained second");
    assert(retained.graph().read<CounterState>(fixture.live.child).value == 9);
    assert(fixture.live.first == "new live first" && fixture.publishedCounter() == 11);
    assert(RootState::copyAssignments == 0);

    retained.refresh();
    assert(retained.state().first == "new live first" && retained.state().second == "new live second");
    assert(retained.graph().read<CounterState>(fixture.live.child).value == 11);
    assert(RootState::copyAssignments == 0);
}

struct Cleanup {
    std::function<void()> function;

    ~Cleanup() noexcept(false) {
        function();
    }
};

void returnCommitOrdering() {
    Fixture fixture;
    GraphModelTransaction<RootState> transaction(fixture.live, fixture.owner.domain());
    bool abandoning = false;
    bool cleaned = false;
    {
        ObjectCommitOnReturn commit([&] {
            assert(cleaned);
            transaction.commit();
        }, [&]() noexcept { return abandoning; });
        commit.arm();
        Cleanup cleanup{[&] {
            transaction.state().first = "cleanup published";
            transaction.graph().mutate<CounterState>(fixture.live.child,
                [](CounterState& state) { state.value = 12; });
            cleaned = true;
        }};
    }
    assert(fixture.live.first == "cleanup published" && fixture.publishedCounter() == 12);
    transaction.refresh();

    try {
        ObjectCommitOnReturn commit([&] { transaction.commit(); },
            [&]() noexcept { return abandoning; });
        commit.arm();
        transaction.state().first = "unwinding edit";
        transaction.graph().mutate<CounterState>(fixture.live.child,
            [](CounterState& state) { state.value = 13; });
        throw ExecutionFailure("unwind");
    } catch (const ExecutionFailure&) {
    }
    assert(fixture.live.first == "cleanup published" && fixture.publishedCounter() == 12);
    transaction.refresh();
    abandoning = true;
    {
        ObjectCommitOnReturn commit([&] { transaction.commit(); },
            [&]() noexcept { return abandoning; });
        commit.arm();
        transaction.state().first = "abandoned edit";
    }
    assert(fixture.live.first == "cleanup published");
    transaction.refresh();
    abandoning = false;

    bool caughtPreparation = false;
    try {
        ObjectCommitOnReturn commit([&] { transaction.commit(); },
            [&]() noexcept { return abandoning; });
        commit.arm();
        transaction.state().first = "failed preparation";
        RootState::failValidation = true;
    } catch (const ExecutionFailure&) {
        caughtPreparation = true;
    }
    RootState::failValidation = false;
    assert(caughtPreparation && fixture.live.first == "cleanup published");
    assert(fixture.publishedCounter() == 12);
}

}

int main() {
    jointPublicationAndPreparation();
    refreshStagesBeforeMutation();
    returnCommitOrdering();
}
