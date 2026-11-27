// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "cancellation.hpp"
#include "publication.hpp"

#include <array>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace pipelang::model {

class ContextOwner;
class ContextLease;
class OperationLane;
class OperationTicket;
class InvocationPermit;
class TaskScope;

namespace context_detail {
inline void requireOwner(std::thread::id thread) {
    if (thread != std::this_thread::get_id())
        throw ExecutionFailure("context.wrong_owner_thread");
}

struct OwnerIdentity {
    const std::thread::id thread = std::this_thread::get_id();
};

struct ObserverSlot {
    explicit ObserverSlot(std::function<void()> callback)
        : callback(std::move(callback)), thread(std::this_thread::get_id()) {}

    std::function<void()> callback;
    std::thread::id thread;
};
struct RevocationAccess;
}

// A one-shot owner-thread subscription. The signal retains weak slots only, so
// unsubscribing cannot leave a callback or its QObject capture retained forever.
class RevocationSubscription {
public:
    RevocationSubscription() = default;
    RevocationSubscription(const RevocationSubscription&) = delete;
    RevocationSubscription& operator=(const RevocationSubscription&) = delete;
    RevocationSubscription(RevocationSubscription&&) noexcept = default;

    RevocationSubscription& operator=(RevocationSubscription&& other) noexcept {
        if (this != &other) {
            reset();
            slot_ = std::move(other.slot_);
        }
        return *this;
    }

    ~RevocationSubscription() { reset(); }

    void reset() noexcept {
        if (!slot_)
            return;
        if (slot_->thread != std::this_thread::get_id())
            std::terminate();
        slot_->callback = {};
        slot_.reset();
    }

private:
    friend struct context_detail::RevocationAccess;
    explicit RevocationSubscription(std::shared_ptr<context_detail::ObserverSlot> slot)
        : slot_(std::move(slot)) {}

    std::shared_ptr<context_detail::ObserverSlot> slot_;
};

namespace context_detail {
struct RevocationState {
    static constexpr std::size_t maximumObservers = 64;
    const std::thread::id thread = std::this_thread::get_id();
    CancellationSource source;
    std::array<std::weak_ptr<ObserverSlot>, maximumObservers> observers;
    std::array<RevocationSubscription, 2> parentSubscriptions;
    bool revoked = false;
    bool notifying = false;

    void revoke() noexcept {
        if (revoked)
            return;
        // Revocation happens before callbacks and is irreversible even when a
        // callback throws. Moving each callback permits safe self-unsubscription.
        revoked = true;
        source.cancel();
        for (auto& subscription : parentSubscriptions)
            subscription.reset();
        notifying = true;
        for (auto& observer : observers) {
            if (auto slot = observer.lock()) {
                auto callback = std::move(slot->callback);
                if (callback) {
                    try {
                        callback();
                    } catch (...) {
                    }
                }
            }
            observer.reset();
        }
        notifying = false;
    }
};

struct PermitState {
    std::thread::id thread;
    std::shared_ptr<const void> rootIdentity;
    CancellationToken cancellation;
    std::shared_ptr<RevocationState> revocation;
};

inline bool permitCurrent(const void* opaque) noexcept {
    const auto* state = static_cast<const PermitState*>(opaque);
    return state && state->thread == std::this_thread::get_id()
        && !state->cancellation.is_cancelled();
}

inline CancellationToken cancelledToken() {
    static const CancellationSource source = [] {
        CancellationSource value;
        value.cancel();
        return value;
    }();
    return source.token();
}
}

namespace context_detail {
struct RevocationAccess {
    static RevocationSubscription subscribe(
        const std::shared_ptr<context_detail::RevocationState>& state, std::function<void()> callback) {
        if (!callback)
            throw ExecutionFailure("context.empty_observer");
        if (!state) {
            try {
                callback();
            } catch (...) {
            }
            return {};
        }
        context_detail::requireOwner(state->thread);
        if (state->notifying)
            throw ExecutionFailure("context.revocation_reentry");
        if (state->revoked) {
            try {
                callback();
            } catch (...) {
            }
            return {};
        }
        for (auto& observer : state->observers) {
            if (observer.expired()) {
                auto slot = std::make_shared<context_detail::ObserverSlot>(std::move(callback));
                observer = slot;
                return RevocationSubscription(std::move(slot));
            }
        }
        throw ExecutionFailure("context.observer_capacity_exceeded");
    }
};
}

// Copies retain immutable admission authority. An invalid capture cannot acquire
// authority from a later owner incarnation or operation in the same lane.
class InvocationPermit {
public:
    InvocationPermit() = default;

    bool is_current() const noexcept {
        return !bound_ || context_detail::permitCurrent(state_.get());
    }

    bool is_bound() const noexcept { return bound_; }

    std::shared_ptr<const void> rootIdentity() const noexcept {
        return state_ ? state_->rootIdentity : nullptr;
    }

    static InvocationPermit combine(const InvocationPermit& left, const InvocationPermit& right);

    CancellationToken cancellation() const {
        if (state_)
            return state_->cancellation;
        return bound_ ? context_detail::cancelledToken() : CancellationToken{};
    }

    PublicationGuard publicationGuard() const noexcept {
        if (!bound_)
            return {};
        return PublicationGuard(state_, context_detail::permitCurrent);
    }

    void requireCurrent() const {
        if (state_)
            context_detail::requireOwner(state_->thread);
        if (!is_current())
            throw StaleContextCancelled();
    }

    RevocationSubscription observeRevocation(std::function<void()> callback) const {
        if (!bound_)
            return {};
        return context_detail::RevocationAccess::subscribe(state_ ? state_->revocation : nullptr,
            std::move(callback));
    }

private:
    friend class ContextLease;
    friend class ContextOwner;
    friend class OperationTicket;

    explicit InvocationPermit(std::shared_ptr<const context_detail::PermitState> state)
        : state_(std::move(state)), bound_(true) {}

    std::shared_ptr<const context_detail::PermitState> state_;
    bool bound_ = false;
};

namespace context_detail {
inline std::shared_ptr<const PermitState> makePermit(const InvocationPermit& parent,
    std::shared_ptr<const void> rootIdentity = {}) {
    if (!rootIdentity)
        rootIdentity = parent.rootIdentity();
    auto revocation = std::make_shared<RevocationState>();
    auto cancellation = CancellationToken::combine(parent.cancellation(), revocation->source.token());
    auto permit = std::make_shared<const PermitState>(PermitState{
        std::this_thread::get_id(), std::move(rootIdentity), std::move(cancellation), revocation});
    if (parent.is_bound()) {
        const std::weak_ptr<RevocationState> weak = revocation;
        revocation->parentSubscriptions.front() = parent.observeRevocation([weak] {
            if (const auto current = weak.lock())
                current->revoke();
        });
    }
    return permit;
}

struct Incarnation {
    Incarnation(std::shared_ptr<const OwnerIdentity> identity, const InvocationPermit& parent,
        std::shared_ptr<const void> rootIdentity)
        : identity(std::move(identity)), permit(makePermit(parent, std::move(rootIdentity))) {}

    std::shared_ptr<const OwnerIdentity> identity;
    std::shared_ptr<const PermitState> permit;
};
}

inline InvocationPermit InvocationPermit::combine(
    const InvocationPermit& left, const InvocationPermit& right) {
    if (left.state_)
        context_detail::requireOwner(left.state_->thread);
    if (right.state_)
        context_detail::requireOwner(right.state_->thread);
    if (!left.bound_)
        return right;
    if (!right.bound_)
        return left;
    if (!left.state_ || !right.state_)
        return InvocationPermit(nullptr);
    if (left.rootIdentity() != right.rootIdentity())
        throw ExecutionFailure("context.foreign_root");
    if (left.state_ == right.state_)
        return left;

    auto revocation = std::make_shared<context_detail::RevocationState>();
    auto cancellation = CancellationToken::combine(left.cancellation(), right.cancellation());
    cancellation = CancellationToken::combine(std::move(cancellation), revocation->source.token());
    auto state = std::make_shared<const context_detail::PermitState>(context_detail::PermitState{
        std::this_thread::get_id(), left.rootIdentity(), std::move(cancellation), revocation});
    const std::weak_ptr<context_detail::RevocationState> weak = revocation;
    const auto forwardRevocation = [weak] {
        if (const auto current = weak.lock())
            current->revoke();
    };
    revocation->parentSubscriptions[0] = left.observeRevocation(forwardRevocation);
    // A previously revoked left capture immediately revokes the compound. It
    // must not register a fresh right observer after that one-shot teardown.
    if (!revocation->revoked)
        revocation->parentSubscriptions[1] = right.observeRevocation(forwardRevocation);
    return InvocationPermit(std::move(state));
}

class ContextLease {
public:
    ContextLease() = default;

    bool is_current() const noexcept { return permit().is_current(); }
    CancellationToken cancellation() const { return permit().cancellation(); }

    InvocationPermit permit() const noexcept {
        return InvocationPermit(incarnation_ ? incarnation_->permit : nullptr);
    }

    bool sameIdentity(const ContextLease& other) const noexcept {
        return incarnation_ == other.incarnation_;
    }

    std::shared_ptr<const void> ownerIdentity() const noexcept {
        return incarnation_ ? incarnation_->identity : nullptr;
    }

    std::shared_ptr<const void> rootIdentity() const noexcept {
        return permit().rootIdentity();
    }

    RevocationSubscription observeRevocation(std::function<void()> callback) const {
        return permit().observeRevocation(std::move(callback));
    }

private:
    friend class ContextOwner;
    friend class OperationLane;

    explicit ContextLease(std::shared_ptr<const context_detail::Incarnation> incarnation)
        : incarnation_(std::move(incarnation)) {}

    std::shared_ptr<const context_detail::Incarnation> incarnation_;
};

class ContextOwner {
public:
    explicit ContextOwner(InvocationPermit parent = {})
        : identity_(std::make_shared<const context_detail::OwnerIdentity>()), parent_(std::move(parent)) {
        if (parent_.state_)
            context_detail::requireOwner(parent_.state_->thread);
        rootIdentity_ = parent_.is_bound() ? parent_.rootIdentity() : identity_;
        current_ = std::make_shared<context_detail::Incarnation>(identity_, parent_, rootIdentity_);
    }

    ContextOwner(const ContextOwner&) = delete;
    ContextOwner& operator=(const ContextOwner&) = delete;
    ContextOwner(ContextOwner&&) = delete;
    ContextOwner& operator=(ContextOwner&&) = delete;

    ~ContextOwner() noexcept {
        if (identity_->thread != std::this_thread::get_id())
            std::terminate();
        // A parent's revocation observer may destroy the source model holding
        // this owner while its own signal is already notifying. Destruction is
        // idempotent lifecycle cleanup, not a reentrant public mutation.
        closed_ = true;
        auto previous = std::exchange(current_, {});
        if (previous)
            previous->permit->revocation->revoke();
    }

    void requireOwner() const { context_detail::requireOwner(identity_->thread); }

    std::shared_ptr<const void> ownerIdentity() const noexcept { return identity_; }

    std::shared_ptr<const void> rootIdentity() const noexcept { return rootIdentity_; }

    ContextLease capture() const {
        requireOwner();
        return ContextLease(current_);
    }

    ContextLease replace() {
        requireMutation();
        if (closed_)
            throw ExecutionFailure("context.closed");
        // Allocate before revoking. Existing captures retain the old identity,
        // so allocator address reuse cannot revive an earlier incarnation.
        auto replacement = std::make_shared<context_detail::Incarnation>(identity_, parent_, rootIdentity_);
        invalidate();
        current_ = std::move(replacement);
        return capture();
    }

    void invalidate() {
        requireMutation();
        auto previous = std::exchange(current_, {});
        if (previous) {
            notifying_ = true;
            previous->permit->revocation->revoke();
            notifying_ = false;
        }
    }

    void close() {
        requireMutation();
        closed_ = true;
        invalidate();
    }

    RevocationSubscription observeRevocation(std::function<void()> callback) const {
        requireMutation();
        return capture().observeRevocation(std::move(callback));
    }

private:
    friend class OperationLane;
    friend class TaskScope;

    struct LifetimeRoot {};

    // Only the task owner can separate lifetime cancellation from logical root
    // provenance. Public context construction always inherits both together.
    ContextOwner(LifetimeRoot, InvocationPermit parent, std::shared_ptr<const void> rootIdentity)
        : identity_(std::make_shared<const context_detail::OwnerIdentity>()),
          rootIdentity_(std::move(rootIdentity)), parent_(std::move(parent)) {
        if (parent_.state_)
            context_detail::requireOwner(parent_.state_->thread);
        if (!rootIdentity_)
            rootIdentity_ = identity_;
        current_ = std::make_shared<context_detail::Incarnation>(identity_, parent_, rootIdentity_);
    }

    void requireMutation() const {
        requireOwner();
        if (notifying_ || (current_ && current_->permit->revocation->notifying))
            throw ExecutionFailure("context.revocation_reentry");
    }

    std::shared_ptr<const context_detail::OwnerIdentity> identity_;
    std::shared_ptr<const void> rootIdentity_;
    InvocationPermit parent_;
    std::shared_ptr<context_detail::Incarnation> current_;
    bool closed_ = false;
    bool notifying_ = false;
};

inline void validate(const std::shared_ptr<ContextOwner>& value) {
    if (!value)
        throw ExecutionFailure("context.null_owner");
    value->requireOwner();
}

enum class OperationPolicy { LatestWins, RejectBusy };

namespace context_detail {
struct OperationState {
    explicit OperationState(const ContextLease& lease) : permit(makePermit(lease.permit())) {}

    std::shared_ptr<const PermitState> permit;
    bool completed = false;
};
}

class OperationTicket {
public:
    OperationTicket() = default;

    bool is_current() const noexcept { return permit().is_current(); }
    CancellationToken cancellation() const { return permit().cancellation(); }

    InvocationPermit permit() const noexcept {
        return InvocationPermit(state_ ? state_->permit : nullptr);
    }

    RevocationSubscription observeRevocation(std::function<void()> callback) const {
        return permit().observeRevocation(std::move(callback));
    }

    Result<void> complete() const {
        if (!state_)
            return Result<void>::Cancelled("context.stale_operation");
        context_detail::requireOwner(state_->permit->thread);
        if (!is_current() || state_->completed)
            return Result<void>::Cancelled("context.stale_operation");
        state_->completed = true;
        state_->permit->revocation->revoke();
        return Result<void>::Success();
    }

private:
    friend class OperationLane;
    explicit OperationTicket(std::shared_ptr<context_detail::OperationState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<context_detail::OperationState> state_;
};

// One retained active operation per lane; old tickets own only cancelled state.
class OperationLane {
public:
    explicit OperationLane(const ContextOwner& owner) : identity_(owner.identity_) {
        context_detail::requireOwner(identity_->thread);
    }
    OperationLane(const OperationLane&) = delete;
    OperationLane& operator=(const OperationLane&) = delete;
    OperationLane(OperationLane&&) = delete;
    OperationLane& operator=(OperationLane&&) = delete;

    ~OperationLane() {
        if (identity_->thread != std::this_thread::get_id())
            std::terminate();
        notifying_ = true;
        if (active_)
            active_->permit->revocation->revoke();
    }

    Result<OperationTicket> begin(const ContextLease& lease,
        OperationPolicy policy = OperationPolicy::LatestWins) {
        context_detail::requireOwner(identity_->thread);
        if (notifying_)
            return Result<OperationTicket>::Failure("context.revocation_reentry");
        if (!lease.incarnation_)
            return Result<OperationTicket>::Cancelled("context.stale");
        if (lease.incarnation_->identity != identity_)
            return Result<OperationTicket>::Failure("context.foreign_owner");
        if (!lease.is_current())
            return Result<OperationTicket>::Cancelled("context.stale");
        if (policy != OperationPolicy::LatestWins && policy != OperationPolicy::RejectBusy)
            return Result<OperationTicket>::Failure("context.invalid_operation_policy");
        if (policy == OperationPolicy::RejectBusy && active_
            && !active_->permit->cancellation.is_cancelled())
            return Result<OperationTicket>::Failure("context.busy");
        auto replacement = std::make_shared<context_detail::OperationState>(lease);
        if (active_) {
            notifying_ = true;
            active_->permit->revocation->revoke();
            notifying_ = false;
        }
        active_ = std::move(replacement);
        if (active_->permit->cancellation.is_cancelled())
            return Result<OperationTicket>::Cancelled("context.stale");
        return Result<OperationTicket>::Success(OperationTicket(active_));
    }

private:
    std::shared_ptr<const context_detail::OwnerIdentity> identity_;
    std::shared_ptr<context_detail::OperationState> active_;
    bool notifying_ = false;
};

namespace context_detail {
inline thread_local InvocationPermit invocationPermit;
}

inline InvocationPermit currentPermit() noexcept {
    return context_detail::invocationPermit;
}

// Dynamic invocation state is installed per owner turn, never across suspension.
class InvocationContextScope {
public:
    explicit InvocationContextScope(InvocationPermit permit)
        : previous_(currentPermit()), publication_(permit.publicationGuard()),
          thread_(std::this_thread::get_id()) {
        context_detail::invocationPermit = std::move(permit);
    }

    InvocationContextScope(const InvocationContextScope&) = delete;
    InvocationContextScope& operator=(const InvocationContextScope&) = delete;
    InvocationContextScope(InvocationContextScope&&) = delete;
    InvocationContextScope& operator=(InvocationContextScope&&) = delete;

    ~InvocationContextScope() {
        if (thread_ != std::this_thread::get_id())
            std::terminate();
        context_detail::invocationPermit = std::move(previous_);
    }

private:
    InvocationPermit previous_;
    PublicationContextScope publication_;
    std::thread::id thread_;
};

}
