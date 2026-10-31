// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/object.hpp>
#include <pipelang/model/atomic.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define PIPELANG_TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PIPELANG_TEST_NOINLINE __declspec(noinline)
#else
#define PIPELANG_TEST_NOINLINE
#endif

thread_local int objectAllocationCountdown = -1;

PIPELANG_TEST_NOINLINE void* operator new(std::size_t size) {
    if (objectAllocationCountdown == 0)
        throw std::bad_alloc();
    if (objectAllocationCountdown > 0)
        --objectAllocationCountdown;
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

PIPELANG_TEST_NOINLINE void operator delete(void* memory) noexcept {
    std::free(memory);
}

PIPELANG_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

PIPELANG_TEST_NOINLINE void* operator new[](std::size_t size) {
    return ::operator new(size);
}

PIPELANG_TEST_NOINLINE void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}

PIPELANG_TEST_NOINLINE void operator delete[](void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

using namespace pipelang::model;
using IntResult = Result<std::int64_t, std::string>;

namespace {

struct CounterTag {
    static constexpr std::uint64_t typeId = 1;
    static bool accepts(std::uint64_t value) {
        return value == typeId;
    }
};

struct CounterInterface {
    [[maybe_unused]] static constexpr std::uint64_t typeId = 100;
    static bool accepts(std::uint64_t value) {
        return value == CounterTag::typeId;
    }
};

struct UnrelatedTag {
    static constexpr std::uint64_t typeId = 2;
    static bool accepts(std::uint64_t value) {
        return value == typeId;
    }
};

using Counter = ObjectRef<CounterTag>;

struct CounterState final : ObjectState {
    std::int64_t value = 0;
    Optional<ObjectEdge<CounterTag>> next;
    ObjectStorage<List<Counter>>::Stored children;
    std::vector<ObjectId> extraEdges;
    std::string text;
    std::shared_ptr<int> lifetime;
    bool required = false;
    ObjectEdge<CounterTag> mandatory;
    static inline bool refuseClone = false;

    std::unique_ptr<ObjectState> clone() const override {
        if (refuseClone)
            throw std::bad_alloc();
        return std::make_unique<CounterState>(*this);
    }

    void trace(ObjectTrace& trace) const override {
        if (next.has_value())
            trace.edge(next.value().id);
        ObjectStorage<List<Counter>>::trace(children, trace);
        for (auto id : extraEdges)
            trace.edge(id);
        if (required)
            trace.edge(mandatory.id);
    }

    std::size_t logicalBytes() const override {
        return sizeof(CounterState) + children.ownedByteCost() + text.size()
            + extraEdges.size() * sizeof(ObjectId);
    }

    void validate() const override {
        pipelang::model::validate(text);
        pipelang::model::validate(children);
        if (value < 0)
            throw ExecutionFailure("negative counter state");
    }
};

template<class Function>
void expectFailure(Function function) {
    bool failed = false;
    try {
        function();
    } catch (const ExecutionFailure&) {
        failed = true;
    }
    assert(failed);
}

void aliasingAndRollback() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto counter = transaction.create<CounterState>(CounterTag{});
    transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 1; });
    transaction.commit();
    auto alias = counter.as<CounterInterface>();
    assert(alias == counter);
    transaction.mutate<CounterState>(alias, [](CounterState& state) { state.value = 3; });
    assert(transaction.read<CounterState>(counter).value == 3);
    transaction.discard();
    assert(transaction.read<CounterState>(alias).value == 1);
    expectFailure([&] { counter.as<UnrelatedTag>(); });
    expectFailure([&] {
        transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = -1; });
    });
    assert(transaction.read<CounterState>(counter).value == 1);

    Counter provisional;
    Atomic<std::int64_t> sharedEffect;
    {
        auto construction = transaction.savepoint();
        provisional = transaction.create<CounterState>(CounterTag{});
        transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 9; });
        sharedEffect.increment();
    }
    assert(transaction.read<CounterState>(counter).value == 1);
    assert(sharedEffect.load() == 1);
    expectFailure([&] { provisional.require(); });
    {
        auto construction = transaction.savepoint();
        CounterState initial;
        initial.required = true;
        auto incomplete = transaction.create<CounterState>(CounterTag{}, std::move(initial));
        expectFailure([&] { transaction.validateInitialized(incomplete); });
        transaction.mutate<CounterState>(incomplete, [&](CounterState& state) {
            state.mandatory = ObjectStorage<Counter>::encode(counter, owner.domain());
        });
        transaction.validateInitialized(incomplete);
        construction.commit();
    }
    transaction.commit();
    owner.domain()->collect();
    assert(owner.domain()->liveCount() == 1);

    Counter afterCollection;
    {
        auto construction = transaction.savepoint();
        {
            auto transient = transaction.create<CounterState>(CounterTag{});
        }
        owner.domain()->collect();
        afterCollection = transaction.create<CounterState>(CounterTag{});
        transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 17; });
    }
    expectFailure([&] { afterCollection.require(); });
    assert(transaction.read<CounterState>(counter).value == 1);

    std::vector<std::unique_ptr<GraphTransaction>> journals;
    for (std::size_t index = 1; index < ObjectDomain::maximumTransactions; ++index)
        journals.push_back(std::make_unique<GraphTransaction>(owner.domain()));
    expectFailure([&] { GraphTransaction excess(owner.domain()); });
}

void cyclesAndStorage() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    {
        auto first = transaction.create<CounterState>(CounterTag{});
        auto second = transaction.create<CounterState>(CounterTag{});
        transaction.mutate<CounterState>(first, [&](CounterState& state) {
            state.next = Optional<ObjectEdge<CounterTag>>::Some(
                ObjectStorage<Counter>::encode(second, owner.domain()));
        });
        transaction.mutate<CounterState>(second, [&](CounterState& state) {
            state.next = Optional<ObjectEdge<CounterTag>>::Some(
                ObjectStorage<Counter>::encode(first, owner.domain()));
        });
        transaction.commit();
        auto list = List<Counter>::Empty();
        list.add(first);
        list.add(second);
        auto stored = ObjectStorage<List<Counter>>::encode(list, owner.domain());
        auto decoded = ObjectStorage<List<Counter>>::decode(stored, owner.domain());
        assert(decoded.at(0) == first && decoded.at(1) == second);
        transaction.mutate<CounterState>(first, [&](CounterState& state) { state.children = stored; });
        transaction.commit();

        auto array = Array<Counter>::Create(2, first);
        auto arrayCopy = ObjectStorage<Array<Counter>>::decode(
            ObjectStorage<Array<Counter>>::encode(array, owner.domain()), owner.domain());
        assert(arrayCopy.at(1) == first);
        auto emptyArray = ObjectStorage<Array<Counter>>::decode({}, owner.domain());
        assert(emptyArray.count() == 0);
        auto dictionary = Dictionary<std::string, Counter>::Empty();
        dictionary.add("second", second);
        auto dictionaryCopy = ObjectStorage<Dictionary<std::string, Counter>>::decode(
            ObjectStorage<Dictionary<std::string, Counter>>::encode(dictionary, owner.domain()), owner.domain());
        assert(dictionaryCopy.at("second") == second);
        auto queue = Queue<Counter>::Empty();
        queue.enqueue(first);
        queue.enqueue(second);
        auto queueCopy = ObjectStorage<Queue<Counter>>::decode(
            ObjectStorage<Queue<Counter>>::encode(queue, owner.domain()), owner.domain());
        assert(queueCopy.dequeue() == first && queueCopy.dequeue() == second);
        auto stack = Stack<Counter>::Empty();
        stack.push(first);
        stack.push(second);
        auto stackCopy = ObjectStorage<Stack<Counter>>::decode(
            ObjectStorage<Stack<Counter>>::encode(stack, owner.domain()), owner.domain());
        assert(stackCopy.pop() == second && stackCopy.pop() == first);
        auto result = Result<Optional<Counter>, std::string>::Success(Optional<Counter>::Some(first));
        auto resultCopy = ObjectStorage<decltype(result)>::decode(
            ObjectStorage<decltype(result)>::encode(result, owner.domain()), owner.domain());
        assert(resultCopy.value().value() == first);
        auto voidResult = Result<void, std::string>::Failure("owned error");
        auto voidCopy = ObjectStorage<decltype(voidResult)>::decode(
            ObjectStorage<decltype(voidResult)>::encode(voidResult, owner.domain()), owner.domain());
        assert(voidCopy.error() == "owned error");
        owner.domain()->collect();
        assert(owner.domain()->liveCount() == 2);
    }
    owner.domain()->collect();
    assert(owner.domain()->liveCount() == 0);
}

void limitsAndTransientCollection() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    std::vector<Counter> roots;
    roots.reserve(ObjectDomain::maximumCells);
    for (std::size_t index = 0; index < ObjectDomain::maximumCells; ++index)
        roots.push_back(transaction.create<CounterState>(CounterTag{}));
    expectFailure([&] { transaction.create<CounterState>(CounterTag{}); });
    assert(owner.domain()->liveCount() == ObjectDomain::maximumCells);
    roots.clear();
    owner.domain()->collect();
    assert(owner.domain()->liveCount() == 0);
    transaction.discard();
    for (int index = 0; index < 4200; ++index) {
        auto transient = transaction.create<CounterState>(CounterTag{});
    }
    owner.domain()->collect();
    assert(owner.domain()->liveCount() == 0);
    transaction.discard();

    auto reference = transaction.create<CounterState>(CounterTag{});
    transaction.commit();
    expectFailure([&] {
        transaction.mutate<CounterState>(reference, [](CounterState& state) {
            state.text.assign(ObjectDomain::maximumBytes, 'x');
        });
    });
    assert(transaction.read<CounterState>(reference).text.empty());
    auto edgeState = [&](std::size_t count) {
        CounterState state;
        state.extraEdges.assign(count, reference.identity());
        return state;
    };
    auto first = transaction.create<CounterState>(CounterTag{}, edgeState(8192));
    auto second = transaction.create<CounterState>(CounterTag{}, edgeState(8192));
    expectFailure([&] { transaction.create<CounterState>(CounterTag{}, edgeState(1)); });
    transaction.commit();
    assert(first != second);
}

void allocationFailures() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto counter = transaction.create<CounterState>(CounterTag{});
    transaction.commit();
    CounterState::refuseClone = true;
    bool failed = false;
    try {
        transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 7; });
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    CounterState::refuseClone = false;
    assert(failed && transaction.read<CounterState>(counter).value == 0);
    int failures = 0;
    for (int count = 0; count < 16; ++count) {
        objectAllocationCountdown = count;
        failed = false;
        try {
            transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 8; });
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        objectAllocationCountdown = -1;
        assert(transaction.read<CounterState>(counter).value == (failed ? 0 : 8));
        failures += failed ? 1 : 0;
        transaction.discard();
        assert(transaction.read<CounterState>(counter).value == 0);

        objectAllocationCountdown = count;
        failed = false;
        try {
            auto provisional = transaction.create<CounterState>(CounterTag{});
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        objectAllocationCountdown = -1;
        transaction.discard();
        assert(owner.domain()->liveCount() == 1);
        failures += failed ? 1 : 0;
    }
    assert(failures != 0);
}

Coroutine<IntResult> retained(TaskScope& scope, std::shared_ptr<ObjectDomain> domain,
                              Counter reference, bool& ready) {
    GraphTransaction transaction(std::move(domain));
    co_await scope.pollUntil([&]() -> std::optional<bool> {
        return ready ? std::optional<bool>(true) : std::nullopt;
    });
    transaction.refresh();
    co_return IntResult::Success(transaction.read<CounterState>(reference).value);
}

void retainedArgumentsAndOwners() {
    Counter escaped;
    std::weak_ptr<int> payload;
    {
        ObjectOwner owner;
        GraphTransaction transaction(owner.domain());
        TaskScope scope;
        CounterState initial;
        initial.lifetime = std::make_shared<int>(4);
        payload = initial.lifetime;
        auto counter = transaction.create<CounterState>(CounterTag{}, std::move(initial));
        transaction.commit();
        const auto retainedId = counter.identity();
        bool ready = false;
        auto task = scope.start<IntResult>([&](CancellationToken) {
            return retained(scope, owner.domain(), counter, ready);
        });
        transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 22; });
        transaction.commit();
        counter = {};
        owner.domain()->collect();
        ready = true;
        scope.poll();
        assert(task.result().value() == 22);
        assert(!payload.expired());
        escaped = owner.domain()->root<CounterTag>(retainedId);

        std::atomic<bool> refused{false};
        std::jthread foreign([&] {
            try {
                escaped.require();
            } catch (const ExecutionFailure&) {
                refused.store(true);
            }
        });
        foreign.join();
        assert(refused.load());
        ObjectOwner otherOwner;
        GraphTransaction other(otherOwner.domain());
        expectFailure([&] { other.read<CounterState>(escaped); });
        expectFailure([&] { ObjectStorage<Counter>::encode(escaped, otherOwner.domain()); });
    }
    assert(payload.expired());
    expectFailure([&] { escaped.require(); });
}

}

int main() {
    aliasingAndRollback();
    cyclesAndStorage();
    limitsAndTransientCollection();
    allocationFailures();
    retainedArgumentsAndOwners();
}
