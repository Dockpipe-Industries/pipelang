// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/object_model.hpp>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>

thread_local int gcAllocationCountdown = -1;
thread_local bool gcFailAfterScratch = false;
thread_local bool gcSawScratch = false;

#if defined(__GNUC__) || defined(__clang__)
#define GC_TEST_NOINLINE __attribute__((noinline))
#else
#define GC_TEST_NOINLINE
#endif
GC_TEST_NOINLINE void* operator new(std::size_t bytes) {
    if (gcAllocationCountdown == 0)
        throw std::bad_alloc();
    if (gcAllocationCountdown > 0)
        --gcAllocationCountdown;
    if (gcFailAfterScratch) {
        if (gcSawScratch)
            throw std::bad_alloc();
        if (bytes == pipelang::model::ObjectDomain::maximumCells * sizeof(pipelang::model::ObjectId))
            gcSawScratch = true;
    }
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes))
        return memory;
    throw std::bad_alloc();
}
GC_TEST_NOINLINE void operator delete(void* memory) noexcept { std::free(memory); }
GC_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
GC_TEST_NOINLINE void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
GC_TEST_NOINLINE void operator delete[](void* memory) noexcept { ::operator delete(memory); }
GC_TEST_NOINLINE void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
using namespace pipelang::model;
struct GcTag {
    static constexpr std::uint64_t typeId = 1;
    static bool accepts(std::uint64_t value) { return value == typeId; }
};
using GcRef = ObjectRef<GcTag>;

// The native strong lease deliberately differs from a traced ObjectEdge. Its
// passive destruction models a root disappearing while another cell is swept.
class GcState final : public ObjectState {
public:
    ObjectId edge;
    std::optional<GcRef> nativeRoot;
    std::vector<unsigned char> payload;
    std::shared_ptr<int> lifetime;
    std::unique_ptr<ObjectState> clone() const override { return std::make_unique<GcState>(*this); }
    void trace(ObjectTrace& trace) const override {
        if (!edge.isNull())
            trace.edge(edge);
    }
    std::size_t logicalBytes() const override { return sizeof(GcState) + payload.size(); }
    void validate() const override {}
};

std::size_t checks = 0;
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
    ++checks;
}

template<class Function>
bool allocationRefused(Function function) {
    gcAllocationCountdown = 0;
    bool refused = false;
    try {
        function();
    } catch (const std::bad_alloc&) {
        refused = true;
    } catch (...) {
        gcAllocationCountdown = -1;
        throw;
    }
    gcAllocationCountdown = -1;
    return refused;
}

template<class Function>
void fails(Function function) {
    bool refused = false;
    try {
        function();
    } catch (const ExecutionFailure&) {
        refused = true;
    }
    check(refused, "expected owner/identity/construction refusal");
}

GcRef create(GraphTransaction& transaction, ObjectId edge = {}, std::size_t bytes = 0,
             std::shared_ptr<int> lifetime = {}) {
    GcState state;
    state.edge = edge;
    state.payload.resize(bytes);
    state.lifetime = std::move(lifetime);
    return transaction.create<GcState>(GcTag{}, std::move(state));
}

void readOnlyAndRootPublication() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto rooted = create(transaction);
    transaction.commit();
    owner.domain()->collect();
    std::size_t publications = 0;
    const bool refused = allocationRefused([&] {
        transaction.prepareCommit();
        transaction.commit([&]() noexcept { ++publications; });
    });
#ifdef PIPELANG_EXPECT_ROOT_FASTPATH
    check(!refused && publications == 1, "stable read-only commit allocated or skipped root callback");
    for (int iteration = 0; iteration < 1024; ++iteration)
        check(!allocationRefused([&] { transaction.commit(); }), "steady read-only commit allocated");
    for (int iteration = 0; iteration < 1024; ++iteration) {
        check(!allocationRefused([&] {
            GraphTransaction temporary(owner.domain());
            temporary.commit();
        }), "fresh read-only transaction allocated or dirtied teardown");
    }
    check(!allocationRefused([&] { transaction.commit(); }), "empty transaction destructor dirtied tracing roots");
#else
    check(refused && publications == 0, "baseline control unexpectedly skipped collector scratch");
#endif
    int nativeState = 7;
    GraphModelTransaction<int> rootState(nativeState, owner.domain());
    rootState.state() = 9;
    const bool rootRefused = allocationRefused([&] { rootState.commit(); });
#ifdef PIPELANG_EXPECT_ROOT_FASTPATH
    check(!rootRefused && nativeState == 9, "graph-empty native root state did not publish allocation-free");
#else
    check(rootRefused && nativeState == 7, "baseline root publication control did not retain failed state");
    rootState.commit();
    check(nativeState == 9, "baseline root retry failed");
#endif
    auto construction = transaction.savepoint();
    fails([&] { transaction.prepareCommit(); });
    construction.commit();
    check(rooted.identity().slot < ObjectDomain::maximumCells, "read-only commit invalidated rooted object");
}

void sameCommitDestructionAndRootStateDrop() {
    ObjectOwner owner;
    std::weak_ptr<int> payload;
    ObjectId identity;
    {
        GraphTransaction setup(owner.domain());
        auto life = std::make_shared<int>(1);
        payload = life;
        auto reference = create(setup, {}, 0, std::move(life));
        identity = reference.identity();
        setup.commit();
        reference = {};
    }
    GraphTransaction observer(owner.domain());
    observer.commit();
    check(payload.expired() && owner.domain()->liveCount() == 0, "last-lease drop missed same-commit destruction");
    fails([&] { (void)owner.domain()->root<GcTag>(identity); });

    Optional<GcRef> root;
    {
        GraphTransaction setup(owner.domain());
        auto reference = create(setup);
        identity = reference.identity();
        root = Optional<GcRef>::Some(reference);
        setup.commit();
    }
    owner.domain()->collect();
    {
        GraphModelTransaction<Optional<GcRef>> publication(root, owner.domain());
        publication.state() = Optional<GcRef>::None();
        publication.commit();
        check(!root.has_value(), "native root reference removal did not publish");
    }
    observer.commit();
    check(owner.domain()->liveCount() == 0, "root-state-only publication failed to dirty next commit");
    fails([&] { (void)owner.domain()->root<GcTag>(identity); });
}

void rootDropDuringSweep() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto child = create(transaction);
    const auto identity = child.identity();
    GcState holder;
    holder.nativeRoot = child;
    auto parent = transaction.create<GcState>(GcTag{}, std::move(holder));
    transaction.commit();
    child = {};
    parent = {};
    transaction.commit();
    check(owner.domain()->liveCount() == 1, "sweep did not retain child marked by passive native lease");
    transaction.commit();
    check(owner.domain()->liveCount() == 0, "sweep-time root drop was cleared at collector exit");
    fails([&] { (void)owner.domain()->root<GcTag>(identity); });
}

void publicationDiscardAndOverlayReplacement() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        ObjectOwner owner;
        GraphTransaction writer(owner.domain());
        auto child = create(writer);
        const auto identity = child.identity();
        auto parent = create(writer, scenario == 0 ? identity : ObjectId{});
        writer.commit();
        child = {};
        if (scenario != 0) {
            writer.mutate<GcState>(parent, [&](GcState& state) { state.edge = identity; });
            owner.domain()->collect();
        }
        if (scenario == 1)
            writer.discard();
        else
            writer.mutate<GcState>(parent, [](GcState& state) { state.edge = {}; });
        if (scenario == 0) {
            writer.commit();
            check(owner.domain()->liveCount() == 2, "prepublication union did not retain old edge");
        }
        GraphTransaction observer(owner.domain());
        observer.commit();
        check(owner.domain()->liveCount() == 1, "publication/discard/overlay replacement missed next empty trace");
        fails([&] { (void)owner.domain()->root<GcTag>(identity); });
    }
}

void savepointsAndProvisionalBaseEdges() {
    for (bool committed : {false, true}) {
        ObjectOwner owner;
        GraphTransaction writer(owner.domain());
        auto child = create(writer);
        const auto identity = child.identity();
        auto parent = create(writer);
        writer.commit();
        child = {};
        if (committed)
            writer.mutate<GcState>(parent, [&](GcState& state) { state.edge = identity; });
        {
            auto savepoint = writer.savepoint();
            writer.mutate<GcState>(parent, [&](GcState& state) {
                state.edge = committed ? ObjectId{} : identity;
            });
            owner.domain()->collect();
            if (committed)
                savepoint.commit();
        }
        GraphTransaction observer(owner.domain());
        observer.commit();
        check(owner.domain()->liveCount() == 1, "savepoint release missed lost overlay reachability");
        fails([&] { (void)owner.domain()->root<GcTag>(identity); });
    }
    ObjectOwner owner;
    GraphTransaction writer(owner.domain());
    auto child = create(writer);
    const auto identity = child.identity();
    writer.commit();
    child = {};
    GcRef provisional;
    {
        auto savepoint = writer.savepoint();
        provisional = create(writer, identity);
        owner.domain()->collect();
    }
    fails([&] { provisional.require(); });
    GraphTransaction observer(owner.domain());
    observer.commit();
    check(owner.domain()->liveCount() == 0, "provisional base-edge destruction did not dirty reachability");
    fails([&] { (void)owner.domain()->root<GcTag>(identity); });
}

void failureRestoreAndTemporaryCandidate() {
    ObjectOwner owner;
    ObjectId identity;
    {
        GraphTransaction setup(owner.domain());
        auto reference = create(setup);
        identity = reference.identity();
        setup.commit();
        reference = {};
    }
    GraphTransaction retry(owner.domain());
    check(allocationRefused([&] { retry.commit(); }), "collector failure injection did not run");
    check(owner.domain()->liveCount() == 1, "failed mark unexpectedly swept cells");
    retry.commit();
    check(owner.domain()->liveCount() == 0, "failed collector did not restore dirty state");
    fails([&] { (void)owner.domain()->root<GcTag>(identity); });

    {
        GraphTransaction setup(owner.domain());
        auto child = create(setup);
        identity = child.identity();
        auto garbage = create(setup, {}, ObjectDomain::maximumBytes * 3 / 4);
        setup.commit();
        child = {};
        garbage = {};
    }
    GcState candidate;
    candidate.edge = identity;
    candidate.payload.resize(ObjectDomain::maximumBytes / 2);
    GraphTransaction construction(owner.domain());
    gcSawScratch = false;
    gcFailAfterScratch = true;
    bool refused = false;
    try {
        (void)construction.create<GcState>(GcTag{}, std::move(candidate));
    } catch (const std::bad_alloc&) {
        refused = true;
    } catch (...) {
        gcFailAfterScratch = false;
        throw;
    }
    gcFailAfterScratch = false;
    check(refused && gcSawScratch && owner.domain()->liveCount() == 1, "candidate pressure failure did not retain temporary root");
    retry.commit();
    check(owner.domain()->liveCount() == 0, "temporary candidate root disappearance did not dirty trace");
    fails([&] { (void)owner.domain()->root<GcTag>(identity); });
}

void reacquisitionAndGuardChecks() {
    ObjectOwner owner;
    GraphTransaction transaction(owner.domain());
    auto child = create(transaction);
    const auto identity = child.identity();
    auto parent = create(transaction, identity);
    transaction.commit();
    child = {};
    owner.domain()->collect();
    auto reacquired = owner.domain()->root<GcTag>(identity);
    check(allocationRefused([&] { transaction.commit(); }), "new RootLease did not invalidate tracing snapshot");
    transaction.commit();
    bool refused = false;
    std::thread foreign([&] {
        try {
            transaction.prepareCommit();
        } catch (const ExecutionFailure&) {
            refused = true;
        }
    });
    foreign.join();
    check(refused, "clean snapshot bypassed owner-thread check");
    owner.domain()->close();
    fails([&] { transaction.prepareCommit(); });
    (void)parent;
    (void)reacquired;
}
}

int main() {
    try {
        readOnlyAndRootPublication();
        sameCommitDestructionAndRootStateDrop();
        rootDropDuringSweep();
        publicationDiscardAndOverlayReplacement();
        savepointsAndProvisionalBaseEdges();
        failureRestoreAndTemporaryCandidate();
        reacquisitionAndGuardChecks();
        std::cout << "PASS " << checks << " root-set collection assertions\n";
        return 0;
    } catch (const std::exception& error) {
        gcAllocationCountdown = -1;
        gcFailAfterScratch = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
