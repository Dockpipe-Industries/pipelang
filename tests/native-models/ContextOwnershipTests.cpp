// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/context.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

using namespace pipelang::model;

namespace {
template<class Callback>
void expectFailure(Callback callback, const char* expected) {
    bool rejected = false;
    try {
        callback();
    } catch (const ExecutionFailure& error) {
        rejected = true;
        assert(std::string(error.what()) == expected);
    }
    assert(rejected);
}

void expectStale(const InvocationPermit& permit) {
    bool rejected = false;
    try {
        permit.requireCurrent();
    } catch (const StaleContextCancelled& error) {
        rejected = true;
        assert(std::string(error.what()) == "context.stale");
    }
    assert(rejected);
}

OperationTicket begin(OperationLane& lane, const ContextLease& lease,
    OperationPolicy policy = OperationPolicy::LatestWins) {
    const auto result = lane.begin(lease, policy);
    assert(result.is_success());
    return result.value();
}

void emptyAndClosedContextsFailClosed() {
    InvocationPermit ordinary;
    assert(!ordinary.is_bound() && ordinary.is_current());
    assert(!ordinary.cancellation().is_cancelled());
    ordinary.requireCurrent();

    ContextLease empty;
    assert(!empty.is_current() && empty.permit().is_bound());
    assert(empty.cancellation().is_cancelled());
    expectStale(empty.permit());
    OperationTicket missing;
    assert(!missing.is_current() && missing.cancellation().is_cancelled());
    assert(missing.complete().is_cancelled());

    ContextOwner owner;
    OperationLane lane(owner);
    const auto lease = owner.capture();
    assert(lease.is_current());
    owner.close();
    owner.close();
    assert(!lease.is_current() && lease.cancellation().is_cancelled());
    assert(!owner.capture().is_current());
    assert(lane.begin(lease).is_cancelled());
    assert(lane.begin(empty).is_cancelled());
    expectFailure([&] { (void)owner.replace(); }, "context.closed");
}

void replacementsNeverRefreshAnOldCapture() {
    ContextOwner owner;
    const auto first = owner.capture();
    const auto firstPermit = first.permit();
    const auto second = owner.replace();
    assert(!first.is_current() && first.cancellation().is_cancelled());
    assert(second.is_current());
    expectStale(firstPermit);
    owner.invalidate();
    owner.invalidate();
    assert(!second.is_current() && !owner.capture().is_current());
    const auto third = owner.replace();
    assert(third.is_current() && !first.is_current() && !second.is_current());
    // Repeated replacements retain a fixed cancellation depth and no counter
    // rollover can make a retained earlier incarnation current again.
    for (int index = 0; index < 128; ++index)
        assert(owner.replace().is_current());
    assert(!third.is_current());
    expectStale(firstPermit);
}

void ownerDestructionRevokesAllDescendants() {
    ContextLease retained;
    OperationTicket ticket;
    std::unique_ptr<OperationLane> retainedLane;
    {
        auto owner = std::make_unique<ContextOwner>();
        retained = owner->capture();
        retainedLane = std::make_unique<OperationLane>(*owner);
        ticket = begin(*retainedLane, retained);
        assert(retained.is_current() && ticket.is_current());
    }
    assert(!retained.is_current() && retained.cancellation().is_cancelled());
    assert(!ticket.is_current() && ticket.cancellation().is_cancelled());
    assert(retainedLane->begin(retained).is_cancelled());
    expectStale(ticket.permit());

    ContextOwner replacementOwner;
    const auto foreign = retainedLane->begin(replacementOwner.capture());
    assert(foreign.is_failure() && foreign.error() == "context.foreign_owner");
}

void latestCompletionRejectsReorderedAndDuplicateDelivery() {
    ContextOwner owner;
    OperationLane lane(owner);
    const auto lease = owner.capture();
    auto first = begin(lane, lease);
    auto second = begin(lane, lease);
    auto duplicate = second;
    assert(!first.is_current() && first.cancellation().is_cancelled());
    assert(second.is_current());
    int published = 0;
    const auto deliver = [&](const OperationTicket& ticket, int value) {
        // Simulate a transport which completely ignores cancellation. Freshness
        // admission at publication is independently sufficient for correctness.
        if (!ticket.permit().is_current())
            return false;
        published = value;
        assert(ticket.complete().is_success());
        return true;
    };
    assert(deliver(second, 2));
    assert(!deliver(first, 1));
    assert(!deliver(duplicate, 3));
    assert(published == 2);
    assert(first.complete().is_cancelled());
    assert(duplicate.complete().is_cancelled());
    assert(second.cancellation().is_cancelled());
    assert(begin(lane, lease).is_current());
}

void lanesAndAdmissionPoliciesRemainIndependent() {
    ContextOwner owner;
    ContextOwner foreignOwner;
    OperationLane firstLane(owner);
    OperationLane secondLane(owner);
    const auto lease = owner.capture();
    const auto first = begin(firstLane, lease, OperationPolicy::RejectBusy);
    const auto second = begin(secondLane, lease);
    const auto busy = firstLane.begin(lease, OperationPolicy::RejectBusy);
    assert(busy.is_failure() && busy.error() == "context.busy");
    assert(first.is_current() && second.is_current());
    const auto foreign = firstLane.begin(foreignOwner.capture());
    assert(foreign.is_failure() && foreign.error() == "context.foreign_owner");
    assert(first.is_current());
    assert(first.complete().is_success());
    const auto replacement = begin(firstLane, lease, OperationPolicy::RejectBusy);
    assert(replacement.is_current() && second.is_current());
    const auto nextLease = owner.replace();
    assert(!replacement.is_current() && !second.is_current());
    assert(firstLane.begin(lease).is_cancelled());
    assert(begin(firstLane, nextLease, OperationPolicy::RejectBusy).is_current());
    assert(begin(secondLane, nextLease).is_current());
}

void laneDestructionRevokesItsCapturedOperations() {
    ContextOwner owner;
    OperationTicket retained;
    {
        OperationLane lane(owner);
        retained = begin(lane, owner.capture());
    }
    assert(!retained.is_current() && retained.cancellation().is_cancelled());
    assert(owner.capture().is_current());
    assert(retained.complete().is_cancelled());
}

void queuedCommandsKeepTheirCapturedAuthority() {
    ContextOwner owner;
    const auto captured = owner.capture().permit();
    int effects = 0;
    const std::function<void()> queued = [captured, &effects] {
        InvocationContextScope invocation(captured);
        if (currentPermit().is_current())
            ++effects;
    };
    queued();
    assert(effects == 1);
    owner.replace();
    queued();
    assert(effects == 1 && owner.capture().is_current());
    assert(currentPermit().is_current() && !currentPermit().is_bound());
}

void nestedInvocationScopesRestoreDuringExceptions() {
    static_assert(!std::is_copy_constructible_v<InvocationContextScope>);
    static_assert(!std::is_move_constructible_v<InvocationContextScope>);
    ContextOwner outerOwner;
    ContextOwner innerOwner;
    const auto outer = outerOwner.capture().permit();
    const auto inner = innerOwner.capture().permit();
    assert(!currentPermit().is_bound());
    {
        InvocationContextScope outerScope(outer);
        assert(currentPermit().is_bound() && currentPermit().is_current());
        try {
            InvocationContextScope innerScope(inner);
            innerOwner.invalidate();
            assert(!currentPermit().is_current());
            throw ExecutionFailure("test");
        } catch (const ExecutionFailure&) {
        }
        assert(currentPermit().is_current());
        outerOwner.invalidate();
        assert(!currentPermit().is_current());
    }
    assert(!currentPermit().is_bound() && currentPermit().is_current());
}

void ownerOperationsAndPublicationAreThreadConfined() {
    ContextOwner owner;
    OperationLane lane(owner);
    const auto lease = owner.capture();
    const auto ticket = begin(lane, lease);
    const auto permit = lease.permit();
    InvocationContextScope ownerInvocation(permit);
    std::thread worker([&] {
        assert(!lease.is_current() && !ticket.is_current());
        // Cancellation observation is safe on workers, while state authority
        // belongs to the owner thread. Dynamic invocation state is thread-local.
        assert(!lease.cancellation().is_cancelled());
        assert(!currentPermit().is_bound());
        expectFailure([&] { (void)owner.capture(); }, "context.wrong_owner_thread");
        expectFailure([&] { (void)owner.replace(); }, "context.wrong_owner_thread");
        expectFailure([&] { owner.invalidate(); }, "context.wrong_owner_thread");
        expectFailure([&] { owner.close(); }, "context.wrong_owner_thread");
        expectFailure([&] { (void)lane.begin(lease); }, "context.wrong_owner_thread");
        expectFailure([&] { (void)ticket.complete(); }, "context.wrong_owner_thread");
        expectFailure([&] { permit.requireCurrent(); }, "context.wrong_owner_thread");
        expectFailure([&] { OperationLane foreignLane(owner); }, "context.wrong_owner_thread");
        {
            InvocationContextScope capturedScope(permit);
            assert(currentPermit().is_bound() && !currentPermit().is_current());
        }
        assert(!currentPermit().is_bound());
    });
    worker.join();
    assert(lease.is_current() && ticket.is_current() && currentPermit().is_current());
}

void cancellationDoesNotRewriteAuthoritativeReceipts() {
    ContextOwner owner;
    OperationLane lane(owner);
    const auto ticket = begin(lane, owner.capture());
    const auto externalReceipt = Result<std::string>::Success("committed");
    owner.invalidate();
    assert(!ticket.permit().is_current());
    assert(externalReceipt.is_success() && externalReceipt.value() == "committed");
}

void leaseIdentityAndSourceOwnerValidation() {
    auto owner = std::make_shared<ContextOwner>();
    validate(owner);
    const auto first = owner->capture();
    const auto copy = first;
    assert(first.sameIdentity(copy));
    assert(first.ownerIdentity() == owner->ownerIdentity());
    assert(ContextLease{}.sameIdentity(ContextLease{}));
    const auto second = owner->replace();
    assert(!first.sameIdentity(second));
    assert(second.ownerIdentity() == first.ownerIdentity());
    owner->close();
    validate(owner);
    expectFailure([] { validate(std::shared_ptr<ContextOwner>{}); }, "context.null_owner");
}

void inheritedRevocationNotifiesChildrenAndOperationsImmediately() {
    ContextOwner parent;
    ContextOwner child(parent.capture().permit());
    OperationLane lane(child);
    const auto lease = child.capture();
    const auto ticket = begin(lane, lease);
    int childNotifications = 0;
    int operationNotifications = 0;
    auto childSubscription = lease.observeRevocation([&] {
        assert(!lease.is_current());
        ++childNotifications;
    });
    auto operationSubscription = ticket.observeRevocation([&] {
        assert(!ticket.is_current());
        ++operationNotifications;
    });
    parent.invalidate();
    assert(childNotifications == 1 && operationNotifications == 1);
    assert(lease.cancellation().is_cancelled() && ticket.cancellation().is_cancelled());
    child.replace();
    assert(!child.capture().is_current());
    parent.replace();
    assert(!child.replace().is_current());
    assert(childNotifications == 1 && operationNotifications == 1);
    int alreadyRevoked = 0;
    auto late = lease.observeRevocation([&] { ++alreadyRevoked; });
    assert(alreadyRevoked == 1);
}

void revocationObserversAreOneShotBoundedAndReentrySafe() {
    ContextOwner owner;
    const auto lease = owner.capture();
    int notifications = 0;
    {
        auto cancelled = lease.observeRevocation([&] { ++notifications; });
    }
    for (int index = 0; index < 256; ++index) {
        auto transient = lease.observeRevocation([&] { ++notifications; });
        transient.reset();
    }
    auto throwing = lease.observeRevocation([&] {
        assert(!lease.is_current());
        expectFailure([&] { owner.replace(); }, "context.revocation_reentry");
        expectFailure([&] {
            auto reentrant = owner.observeRevocation([] {});
        }, "context.revocation_reentry");
        throw ExecutionFailure("observer failed");
    });
    auto surviving = lease.observeRevocation([&] { ++notifications; });
    owner.replace();
    assert(notifications == 1 && !lease.is_current() && owner.capture().is_current());
    owner.invalidate();
    assert(notifications == 1);

    const auto current = owner.replace();
    std::array<RevocationSubscription, context_detail::RevocationState::maximumObservers> subscriptions;
    for (auto& subscription : subscriptions)
        subscription = current.observeRevocation([] {});
    expectFailure([&] {
        auto overflow = current.observeRevocation([] {});
    }, "context.observer_capacity_exceeded");
    subscriptions.front().reset();
    subscriptions.front() = current.observeRevocation([] {});
    owner.invalidate();
    assert(!current.is_current());
}

void latestOperationObserversReleaseParentSlots() {
    ContextOwner owner;
    OperationLane lane(owner);
    const auto lease = owner.capture();
    auto previous = begin(lane, lease);
    int notifications = 0;
    for (int index = 0; index < 256; ++index) {
        auto observer = previous.observeRevocation([&] { ++notifications; });
        auto replacement = begin(lane, lease);
        assert(!previous.is_current());
        previous = std::move(replacement);
    }
    assert(notifications == 256 && previous.is_current());
    auto completion = previous.observeRevocation([&] { ++notifications; });
    assert(previous.complete().is_success());
    assert(notifications == 257);
    assert(previous.complete().is_cancelled() && notifications == 257);
}

void compoundPermitsRetainBothParentsAndOneLogicalRoot() {
    ContextOwner root;
    ContextOwner first(root.capture().permit());
    ContextOwner second(root.capture().permit());
    const auto firstLease = first.capture();
    const auto secondLease = second.capture();
    assert(firstLease.rootIdentity() == root.rootIdentity());
    assert(secondLease.rootIdentity() == root.capture().permit().rootIdentity());
    assert(firstLease.ownerIdentity() != secondLease.ownerIdentity());
    auto combined = InvocationPermit::combine(firstLease.permit(), secondLease.permit());
    assert(combined.is_current() && combined.rootIdentity() == root.rootIdentity());
    assert(InvocationPermit::combine({}, combined).is_current());
    assert(InvocationPermit::combine(combined, {}).is_current());
    assert(InvocationPermit::combine(firstLease.permit(), firstLease.permit()).is_current());
    const auto invalid = InvocationPermit::combine(ContextLease{}.permit(), combined);
    assert(invalid.is_bound() && !invalid.is_current());

    ContextOwner unrelated;
    expectFailure([&] {
        (void)InvocationPermit::combine(firstLease.permit(), unrelated.capture().permit());
    }, "context.foreign_root");

    int notifications = 0;
    auto observer = combined.observeRevocation([&] { ++notifications; });
    first.invalidate();
    assert(!combined.is_current() && combined.cancellation().is_cancelled());
    assert(notifications == 1 && secondLease.is_current());
    first.replace();
    assert(!combined.is_current());
    auto replacement = InvocationPermit::combine(first.capture().permit(), secondLease.permit());
    auto replacementObserver = replacement.observeRevocation([&] { ++notifications; });
    second.invalidate();
    assert(!replacement.is_current() && first.capture().is_current());
    assert(notifications == 2);

    bool wrongThread = false;
    const auto firstPermit = first.capture().permit();
    std::thread worker([&] {
        try {
            (void)InvocationPermit::combine(firstPermit, {});
        } catch (const ExecutionFailure& error) {
            wrongThread = std::string(error.what()) == "context.wrong_owner_thread";
        }
    });
    worker.join();
    assert(wrongThread);
}

void oldOperationsCannotAcquireAuthorityFromNewChildContexts() {
    ContextOwner root;
    ContextOwner entity(root.capture().permit());
    OperationLane lane(root);
    const auto lease = root.capture();
    const auto oldOperation = begin(lane, lease);
    const auto oldBinding = InvocationPermit::combine(oldOperation.permit(), entity.capture().permit());
    const auto newOperation = begin(lane, lease);
    const auto newEntity = entity.replace();
    assert(!oldBinding.is_current() && newOperation.is_current() && newEntity.is_current());
    const auto obsolete = InvocationPermit::combine(oldOperation.permit(), newEntity.permit());
    assert(!obsolete.is_current() && obsolete.cancellation().is_cancelled());
    const auto current = InvocationPermit::combine(newOperation.permit(), newEntity.permit());
    assert(current.is_current());
    ContextOwner descendant(current);
    assert(descendant.capture().is_current() && descendant.rootIdentity() == root.rootIdentity());
    int descendantNotifications = 0;
    auto observer = descendant.observeRevocation([&] { ++descendantNotifications; });
    newOperation.complete();
    assert(!current.is_current() && !descendant.capture().is_current());
    assert(descendantNotifications == 1);
}

void sourceOwnersCanDestructDuringInheritedRevocation() {
    ContextOwner parent;
    auto child = std::make_unique<ContextOwner>(parent.capture().permit());
    const auto retained = child->capture();
    auto observer = retained.observeRevocation([&] { child.reset(); });
    parent.invalidate();
    assert(!child && !retained.is_current());
}
}

int main() {
    emptyAndClosedContextsFailClosed();
    replacementsNeverRefreshAnOldCapture();
    ownerDestructionRevokesAllDescendants();
    latestCompletionRejectsReorderedAndDuplicateDelivery();
    lanesAndAdmissionPoliciesRemainIndependent();
    laneDestructionRevokesItsCapturedOperations();
    queuedCommandsKeepTheirCapturedAuthority();
    nestedInvocationScopesRestoreDuringExceptions();
    ownerOperationsAndPublicationAreThreadConfined();
    cancellationDoesNotRewriteAuthoritativeReceipts();
    leaseIdentityAndSourceOwnerValidation();
    inheritedRevocationNotifiesChildrenAndOperationsImmediately();
    revocationObserversAreOneShotBoundedAndReentrySafe();
    latestOperationObserversReleaseParentSlots();
    compoundPermitsRetainBothParentsAndOneLogicalRoot();
    oldOperationsCannotAcquireAuthorityFromNewChildContexts();
    sourceOwnersCanDestructDuringInheritedRevocation();
}
