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

void mutationCallableLifetimeAndCategory() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto counter = transaction.create<CounterState>(CounterTag{});
    transaction.commit();

    struct Callback {
        int& copies;
        int& moves;
        int& calls;
        int& destructions;
        bool& refuseCopy;

        Callback(int& copyCount, int& moveCount, int& callCount, int& destructionCount,
                 bool& copyRefusal)
            : copies(copyCount), moves(moveCount), calls(callCount), destructions(destructionCount),
              refuseCopy(copyRefusal) {
        }

        Callback(const Callback& other)
            : copies(other.copies), moves(other.moves), calls(other.calls),
              destructions(other.destructions), refuseCopy(other.refuseCopy) {
            ++copies;
            if (refuseCopy)
                throw std::bad_alloc();
        }

        Callback(Callback&& other) noexcept
            : copies(other.copies), moves(other.moves), calls(other.calls),
              destructions(other.destructions), refuseCopy(other.refuseCopy) {
            ++moves;
        }

        ~Callback() {
            ++destructions;
        }

        void operator()(CounterState& state) & {
            ++calls;
            state.value += 1;
        }

        void operator()(CounterState&) && = delete;
        Callback* operator&() = delete;
        const Callback* operator&() const = delete;
    };

    int copies = 0;
    int moves = 0;
    int calls = 0;
    int destructions = 0;
    bool refuseCopy = true;
    Callback callback(copies, moves, calls, destructions, refuseCopy);
    ObjectOwner foreignOwner;
    GraphTransaction foreign(foreignOwner.domain());
    bool copyFailed = false;
    try {
        // Parameter copying must precede checkedReference, which would reject
        // this owner's counter if the callable had been admitted successfully.
        foreign.mutate<CounterState>(counter, callback);
    } catch (const std::bad_alloc&) {
        copyFailed = true;
    }
    assert(copyFailed && copies == 1 && moves == 0 && calls == 0 && destructions == 0);
    assert(transaction.read<CounterState>(counter).value == 0);
    refuseCopy = false;
    copies = 0;
    transaction.mutate<CounterState>(counter, callback);
    assert(copies == 1 && moves == 0 && calls == 1 && destructions == 1);
    transaction.mutate<CounterState>(counter, std::move(callback));
    assert(copies == 1 && moves == 1 && calls == 2 && destructions == 2);
    transaction.mutate<CounterState, CounterTag, Callback&>(counter, callback);
    assert(copies == 1 && moves == 1 && calls == 3 && destructions == 2);
    assert(transaction.read<CounterState>(counter).value == 3);

    auto moveOnly = [owned = std::make_unique<int>(4)](CounterState& state) mutable {
        state.value = *owned;
        owned.reset();
    };
    transaction.mutate<CounterState>(counter, std::move(moveOnly));
    assert(transaction.read<CounterState>(counter).value == 4);
    transaction.commit();
}

void failedMutationRetainsPreparedState() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto counter = transaction.create<CounterState>(CounterTag{});
    transaction.commit();
    transaction.mutate<CounterState>(counter, [](CounterState& state) {
        state.value = 7;
    });
    transaction.prepareCommit();

    int calls = 0;
    CounterState::refuseClone = true;
    bool cloneFailed = false;
    try {
        transaction.mutate<CounterState>(counter, [&](CounterState&) {
            ++calls;
        });
    } catch (const std::bad_alloc&) {
        cloneFailed = true;
    }
    CounterState::refuseClone = false;
    assert(cloneFailed && calls == 0 && transaction.read<CounterState>(counter).value == 7);

    bool cancelled = false;
    std::weak_ptr<int> abandonedPayload;
    try {
        transaction.mutate<CounterState>(counter, [&](CounterState& state) {
            ++calls;
            state.value = 99;
            state.lifetime = std::make_shared<int>(99);
            abandonedPayload = state.lifetime;
            throw OperationCancelled("mutation callback cancelled");
        });
    } catch (const OperationCancelled& failure) {
        cancelled = true;
        assert(std::string(failure.what()) == "mutation callback cancelled");
    }
    assert(cancelled && calls == 1 && transaction.read<CounterState>(counter).value == 7);
    assert(abandonedPayload.expired());
    // Both refusals occurred before publication, so the earlier preparation is
    // still usable. This overload intentionally does not prepare a second time.
    transaction.commit([]() noexcept {});
    GraphTransaction inspection(owner.domain());
    assert(inspection.read<CounterState>(counter).value == 7);

    ObjectOwner foreignOwner;
    GraphTransaction foreign(foreignOwner.domain());
    expectFailure([&] {
        foreign.mutate<CounterState>(counter, [&](CounterState&) {
            ++calls;
        });
    });
    assert(calls == 1);
}

struct ObservedCounterTag {
    static constexpr std::uint64_t typeId = CounterTag::typeId;
    static inline int calls = 0;
    static inline bool allow = true;
    static inline bool refuse = false;
    static inline ObjectDomain* closeDuringCheck = nullptr;
    static inline GraphTransaction* discardDuringCheck = nullptr;
    static inline ObjectRef<ObservedCounterTag>* replaceDuringCheck = nullptr;
    static inline const ObjectRef<ObservedCounterTag>* replacement = nullptr;
    static inline std::optional<ObjectRef<ObservedCounterTag>>* destroyDuringCheck = nullptr;

    static bool accepts(std::uint64_t value) {
        ++calls;
        if (auto* domain = std::exchange(closeDuringCheck, nullptr))
            domain->close();
        if (auto* transaction = std::exchange(discardDuringCheck, nullptr))
            transaction->discard();
        if (auto* target = std::exchange(replaceDuringCheck, nullptr)) {
            if (auto* source = std::exchange(replacement, nullptr))
                *target = *source;
            else
                *target = {};
        }
        if (auto* receiver = std::exchange(destroyDuringCheck, nullptr))
            receiver->reset();
        if (refuse)
            throw 23;
        return allow && value == typeId;
    }
};

struct GeneratedCounterTag {
    using PureObjectTagOwner = GeneratedCounterTag;
    static constexpr std::uint64_t typeId = CounterTag::typeId;

    static bool accepts(std::uint64_t value) noexcept {
        return value == typeId;
    }
};

struct DerivedCounterTag : GeneratedCounterTag {
    static inline int calls = 0;
    static inline std::optional<ObjectRef<DerivedCounterTag>>* destroyDuringCheck = nullptr;

    static bool accepts(std::uint64_t value) {
        ++calls;
        if (auto* receiver = std::exchange(destroyDuringCheck, nullptr))
            receiver->reset();
        return value == typeId;
    }
};

static_assert(std::is_same_v<GeneratedCounterTag::PureObjectTagOwner, GeneratedCounterTag>);
static_assert(!std::is_same_v<DerivedCounterTag::PureObjectTagOwner, DerivedCounterTag>);

template<class Function>
void expectObjectFailureMessage(Function function, const char* expected) {
    bool failed = false;
    try {
        function();
    } catch (const ExecutionFailure& failure) {
        failed = true;
        assert(std::string(failure.what()) == expected);
    }
    assert(failed);
}

void existingRootValidationHasNoAllocations() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto reference = transaction.create<CounterState>(ObservedCounterTag{});
    transaction.commit();
    const auto identity = reference.identity();
    ObservedCounterTag::calls = 0;
    objectAllocationCountdown = 0;
    try {
        for (int repetition = 0; repetition < 64; ++repetition) {
            reference.requireValid();
            assert(reference.identity() == identity);
            assert(reference.typeId() == CounterTag::typeId);
            assert(reference.ownerDomain() == owner.domain());
            auto sameLease = owner.domain()->root<ObservedCounterTag>(identity);
            assert(sameLease == reference);
        }
    } catch (...) {
        objectAllocationCountdown = -1;
        throw;
    }
    assert(objectAllocationCountdown == 0);
    objectAllocationCountdown = -1;
    assert(ObservedCounterTag::calls == 64 * 5);

    ObservedCounterTag::calls = 0;
    auto alias = reference.as<CounterInterface>();
    assert(ObservedCounterTag::calls == 1 && alias == reference);
    reference = {};
    owner.domain()->collect();
    assert(alias.identity() == identity && owner.domain()->liveCount() == 1);
    alias = {};
    owner.domain()->collect();
    assert(owner.domain()->liveCount() == 0);
}

void rootValidationPreservesTagFailuresAndAdmissionOrder() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto reference = transaction.create<CounterState>(ObservedCounterTag{});
    transaction.commit();
    const auto identity = reference.identity();

    ObservedCounterTag::calls = 0;
    ObservedCounterTag::allow = false;
    expectObjectFailureMessage([&] { reference.requireValid(); },
                               "object does not implement the requested type");
    assert(ObservedCounterTag::calls == 1);
    expectObjectFailureMessage([&] { owner.domain()->root<ObservedCounterTag>(identity); },
                               "object does not implement the requested type");
    assert(ObservedCounterTag::calls == 2);
    ObservedCounterTag::allow = true;

    ObservedCounterTag::calls = 0;
    ObservedCounterTag::refuse = true;
    bool refused = false;
    try {
        reference.requireValid();
    } catch (int value) {
        refused = true;
        assert(value == 23);
    }
    ObservedCounterTag::refuse = false;
    assert(refused && ObservedCounterTag::calls == 1);
    assert(reference.identity() == identity);

    ObservedCounterTag::calls = 0;
    ObjectRef<ObservedCounterTag> empty;
    expectObjectFailureMessage([&] { empty.requireValid(); }, "object reference is uninitialized");
    assert(ObservedCounterTag::calls == 0);
    expectObjectFailureMessage([&] { owner.domain()->root<ObservedCounterTag>({}); },
                               "object reference is uninitialized");
    assert(ObservedCounterTag::calls == 0);

    std::jthread foreign([&] {
        expectObjectFailureMessage([&] { reference.requireValid(); },
                                   "object accessed from a different owner thread");
    });
    foreign.join();
    assert(ObservedCounterTag::calls == 0);
    owner.domain()->close();
    expectObjectFailureMessage([&] { reference.requireValid(); }, "object domain is closed");
    assert(ObservedCounterTag::calls == 0);
    std::jthread foreignClosed([&] {
        expectObjectFailureMessage([&] { reference.requireValid(); },
                                   "object accessed from a different owner thread");
    });
    foreignClosed.join();
    assert(ObservedCounterTag::calls == 0);
}

void rootValidationPreservesGenerationAndColdAllocation() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto stale = transaction.create<CounterState>(ObservedCounterTag{});
    const auto previous = stale.identity();
    transaction.discard();
    auto replacement = transaction.create<CounterState>(ObservedCounterTag{});
    const auto current = replacement.identity();
    assert(current.slot == previous.slot && current.generation == previous.generation + 1);
    transaction.commit();

    ObservedCounterTag::calls = 0;
    expectObjectFailureMessage([&] { stale.requireValid(); }, "object identity is no longer valid");
    expectObjectFailureMessage([&] { owner.domain()->root<ObservedCounterTag>(previous); },
                               "object identity is no longer valid");
    assert(ObservedCounterTag::calls == 0);
    replacement.requireValid();
    assert(ObservedCounterTag::calls == 1);

    replacement = {};
    ObservedCounterTag::calls = 0;
    bool refused = false;
    objectAllocationCountdown = 0;
    try {
        owner.domain()->root<ObservedCounterTag>(current);
    } catch (const std::bad_alloc&) {
        refused = true;
    } catch (...) {
        objectAllocationCountdown = -1;
        throw;
    }
    objectAllocationCountdown = -1;
    assert(refused && ObservedCounterTag::calls == 1);
    auto recovered = owner.domain()->root<ObservedCounterTag>(current);
    assert(ObservedCounterTag::calls == 2);
    assert(recovered.identity() == current);
    assert(transaction.read<CounterState>(recovered).value == 0);
}

void reentrantTagInvalidationPreservesLeaseFailure() {
    for (bool close : {false, true}) {
        ObjectOwner owner;
        GraphTransaction transaction(owner.domain());
        auto reference = transaction.create<CounterState>(ObservedCounterTag{});
        if (close)
            transaction.commit();
        ObservedCounterTag::calls = 0;
        if (close)
            ObservedCounterTag::closeDuringCheck = owner.domain().get();
        else
            ObservedCounterTag::discardDuringCheck = &transaction;

        // accepts() invalidates the cached lease after checked(id). The original
        // root() path still attempts a lease allocation, without repeating either
        // checked(id) or accepts(). Keep this observable failure boundary intact.
        bool refused = false;
        objectAllocationCountdown = 0;
        try {
            reference.requireValid();
        } catch (const std::bad_alloc&) {
            refused = true;
        } catch (...) {
            objectAllocationCountdown = -1;
            throw;
        }
        objectAllocationCountdown = -1;
        assert(refused && ObservedCounterTag::calls == 1);
        if (close) {
            expectObjectFailureMessage([&] { reference.requireValid(); }, "object domain is closed");
        } else {
            expectObjectFailureMessage([&] { reference.requireValid(); },
                                       "object identity is no longer valid");
        }
        assert(ObservedCounterTag::calls == 1);
    }
}

void reentrantTagReplacementPreservesCapturedIdentity() {
    for (bool replace : {false, true}) {
        ObjectOwner owner;
        GraphTransaction transaction(owner.domain());
        auto reference = transaction.create<CounterState>(ObservedCounterTag{});
        auto other = transaction.create<CounterState>(ObservedCounterTag{});
        transaction.commit();
        const auto originalId = reference.identity();
        const auto otherId = other.identity();
        ObservedCounterTag::calls = 0;
        ObservedCounterTag::replaceDuringCheck = &reference;
        if (replace)
            ObservedCounterTag::replacement = &other;

        // The receiver object stays alive, but accepts() replaces its lease.
        // Validation must retain the original id and reacquire that cell's lease,
        // rather than validating or retaining the replacement reference's cell.
        bool refused = false;
        objectAllocationCountdown = 0;
        try {
            reference.requireValid();
        } catch (const std::bad_alloc&) {
            refused = true;
        } catch (...) {
            objectAllocationCountdown = -1;
            throw;
        }
        objectAllocationCountdown = -1;
        assert(refused && ObservedCounterTag::calls == 1);
        if (replace)
            assert(reference.identity() == otherId);
        else
            assert(reference.isNull());
        auto recovered = owner.domain()->root<ObservedCounterTag>(originalId);
        assert(recovered.identity() == originalId && recovered != other);
        assert(owner.domain()->liveCount() == 2);
    }
}

void generatedPredicateValidationPreservesChecks() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto reference = transaction.create<CounterState>(GeneratedCounterTag{});
    transaction.commit();
    const auto identity = reference.identity();
    objectAllocationCountdown = 0;
    try {
        for (int repetition = 0; repetition < 64; ++repetition) {
            reference.requireValid();
            assert(reference.identity() == identity);
            assert(reference.typeId() == CounterTag::typeId);
            assert(reference.ownerDomain() == owner.domain());
        }
    } catch (...) {
        objectAllocationCountdown = -1;
        throw;
    }
    assert(objectAllocationCountdown == 0);
    objectAllocationCountdown = -1;

    ObjectRef<GeneratedCounterTag> empty;
    expectObjectFailureMessage([&] { empty.requireValid(); }, "object reference is uninitialized");
    auto unrelated = transaction.create<CounterState>(UnrelatedTag{});
    transaction.commit();
    expectObjectFailureMessage([&] { unrelated.as<GeneratedCounterTag>(); },
                               "object does not implement the requested type");
    auto stale = transaction.create<CounterState>(GeneratedCounterTag{});
    transaction.discard();
    expectObjectFailureMessage([&] { stale.requireValid(); }, "object identity is no longer valid");
    std::jthread foreign([&] {
        expectObjectFailureMessage([&] { reference.requireValid(); },
                                   "object accessed from a different owner thread");
    });
    foreign.join();
    owner.domain()->close();
    expectObjectFailureMessage([&] { reference.requireValid(); }, "object domain is closed");
}

template<class Tag>
void customPredicateMayDestroyReceiver() {
    for (int allocationBudget : {0, 1}) {
        ObjectOwner owner;
        GraphTransaction transaction(owner.domain());
        std::optional<ObjectRef<Tag>> receiver(transaction.create<CounterState>(Tag{}));
        transaction.commit();
        const auto identity = receiver->identity();
        Tag::calls = 0;
        Tag::destroyDuringCheck = &receiver;

        // Destroying the receiver is valid here: the original root() path has
        // already captured its domain/id and never accesses that receiver again.
        // The live cell has lost its last lease, so root() must reacquire it.
        bool refused = false;
        objectAllocationCountdown = allocationBudget;
        try {
            receiver->requireValid();
        } catch (const std::bad_alloc&) {
            refused = true;
        } catch (...) {
            objectAllocationCountdown = -1;
            throw;
        }
        const auto remaining = objectAllocationCountdown;
        objectAllocationCountdown = -1;
        assert(refused == (allocationBudget == 0));
        assert(remaining == 0 && !receiver.has_value() && Tag::calls == 1);
        auto recovered = owner.domain()->root<Tag>(identity);
        assert(recovered.identity() == identity);
        assert(transaction.read<CounterState>(recovered).value == 0);
    }
}

void collectEmptyWithoutAllocations(const std::shared_ptr<ObjectDomain>& domain) {
    assert(domain->liveCount() == 0);
    objectAllocationCountdown = 0;
    try {
        for (int repetition = 0; repetition < 512; ++repetition)
            domain->collect();
    } catch (...) {
        objectAllocationCountdown = -1;
        throw;
    }
    objectAllocationCountdown = -1;
}

void emptyDomainCollection() {
    ObjectOwner owner;
    collectEmptyWithoutAllocations(owner.domain());
    GraphTransaction transaction(owner.domain());
    auto counter = transaction.create<CounterState>(CounterTag{});
    assert(owner.domain()->liveCount() == 1);
    transaction.commit();
    transaction.mutate<CounterState>(counter, [](CounterState& state) { state.value = 8; });
    transaction.commit();
    assert(owner.domain()->liveCount() == 1 && transaction.read<CounterState>(counter).value == 8);
    counter = {};
    owner.domain()->collect();
    collectEmptyWithoutAllocations(owner.domain());

    Counter provisional;
    {
        auto construction = transaction.savepoint();
        provisional = transaction.create<CounterState>(CounterTag{});
        owner.domain()->collect();
        assert(owner.domain()->liveCount() == 1);
    }
    expectFailure([&] { provisional.require(); });
    collectEmptyWithoutAllocations(owner.domain());

    // Each failed create, including root-lease failure after cell publication,
    // must leave the independently scanned cell count and empty fastpath intact.
    int failedAllocations = 0;
    for (int count = 0; count < 16; ++count) {
        objectAllocationCountdown = count;
        try {
            auto transient = transaction.create<CounterState>(CounterTag{});
        } catch (const std::bad_alloc&) {
            ++failedAllocations;
        }
        objectAllocationCountdown = -1;
        transaction.discard();
        collectEmptyWithoutAllocations(owner.domain());
    }
    assert(failedAllocations != 0);

    CounterState invalid;
    invalid.extraEdges.push_back({0, 1});
    expectFailure([&] { transaction.create<CounterState>(CounterTag{}, invalid); });
    collectEmptyWithoutAllocations(owner.domain());
    CounterState nullEdge;
    nullEdge.extraEdges.push_back({});
    auto nullable = transaction.create<CounterState>(CounterTag{}, nullEdge);
    expectFailure([&] { transaction.validateInitialized(nullable); });
    transaction.discard();
    expectFailure([&] { nullable.require(); });
    collectEmptyWithoutAllocations(owner.domain());

    std::atomic<bool> refused{false};
    std::jthread foreign([&] {
        try {
            owner.domain()->collect();
        } catch (const ExecutionFailure& error) {
            refused.store(error.what() == std::string("object accessed from a different owner thread"));
        }
    });
    foreign.join();
    assert(refused.load());
    owner.domain()->close();
    expectFailure([&] { owner.domain()->collect(); });
}

struct PaddedTag {
    static constexpr std::uint64_t typeId = 3;
    static bool accepts(std::uint64_t value) {
        return value == typeId;
    }
};

struct PaddedState final : ObjectState {
    std::vector<unsigned char> payload;
    ObjectId edge;
    static inline int traces = 0;

    std::unique_ptr<ObjectState> clone() const override {
        return std::make_unique<PaddedState>(*this);
    }
    void trace(ObjectTrace& trace) const override {
        ++traces;
        if (!edge.isNull())
            trace.edge(edge);
    }
    std::size_t logicalBytes() const override {
        return sizeof(PaddedState) + payload.size();
    }
    void validate() const override {
    }
};

void capacityCollectionKeepsCandidateRoots() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto child = transaction.create<CounterState>(CounterTag{});
    transaction.commit();
    const auto childId = child.identity();
    PaddedState garbage;
    garbage.payload.resize(ObjectDomain::maximumBytes * 3 / 4);
    auto discarded = transaction.create<PaddedState>(PaddedTag{}, std::move(garbage));
    transaction.commit();
    discarded = {};
    child = {};

    // Only the prospective parent's trace retains this child while capacity
    // preparation collects the unrooted large object.
    PaddedState parent;
    parent.payload.resize(ObjectDomain::maximumBytes / 2);
    parent.edge = childId;
    PaddedState::traces = 0;
    auto rooted = transaction.create<PaddedState>(PaddedTag{}, std::move(parent));
    transaction.commit();
    assert(PaddedState::traces != 0 && owner.domain()->liveCount() == 2);
    auto retained = owner.domain()->root<CounterTag>(childId);
    assert(transaction.read<CounterState>(retained).value == 0);
    retained = {};
    rooted = {};
    owner.domain()->collect();
    collectEmptyWithoutAllocations(owner.domain());
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
    existingRootValidationHasNoAllocations();
    rootValidationPreservesTagFailuresAndAdmissionOrder();
    rootValidationPreservesGenerationAndColdAllocation();
    reentrantTagInvalidationPreservesLeaseFailure();
    reentrantTagReplacementPreservesCapturedIdentity();
    generatedPredicateValidationPreservesChecks();
    customPredicateMayDestroyReceiver<ObservedCounterTag>();
    customPredicateMayDestroyReceiver<DerivedCounterTag>();
    emptyDomainCollection();
    capacityCollectionKeepsCandidateRoots();
    aliasingAndRollback();
    cyclesAndStorage();
    limitsAndTransientCollection();
    allocationFailures();
    mutationCallableLifetimeAndCategory();
    failedMutationRetainsPreparedState();
    retainedArgumentsAndOwners();
}
