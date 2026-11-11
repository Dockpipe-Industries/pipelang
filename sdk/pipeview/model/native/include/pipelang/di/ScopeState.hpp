// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CleanupWork.hpp"
#include "InstanceOwnership.hpp"
#include "ScopeLimits.hpp"
#include "ScopeStatus.hpp"
#include "ScopeTicket.hpp"
#include "detail/ScopeStampSequence.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

namespace pipelang::di {

// Explicit lifecycle bookkeeping, separate from ordinary graph transactions.
// It never invokes cleanup, closes children, drains tasks or revokes references.
class ScopeState {
public:
    explicit ScopeState(ScopeLimits limits = {})
        : owner_(std::this_thread::get_id()), limits_(limits) {
        if (limits.activities == 0 || limits.ownedRecords == 0 ||
            limits.activities > ScopeLimits::maximumEntries ||
            limits.ownedRecords > ScopeLimits::maximumEntries)
            throw model::ExecutionFailure("di.scope_limits_invalid");
        static detail::ScopeStampSequence stamps;
        const auto stamp = stamps.next();
        if (!stamp.is_success())
            throw model::ExecutionFailure(stamp.error());
        stamp_ = stamp.value();
    }
    ScopeState(const ScopeState&) = delete;
    ScopeState& operator=(const ScopeState&) = delete;
    ScopeState(ScopeState&&) = delete;
    ScopeState& operator=(ScopeState&&) = delete;
    // Deliberately inert: generated orchestration must explicitly close on owner.
    ~ScopeState() = default;

    model::Result<void> requireOpen() const {
        if (!onOwner())
            return failure("di.scope_wrong_owner");
        if (phase_ == ScopePhase::Closing)
            return failure("di.scope_closing");
        if (phase_ == ScopePhase::Closed)
            return failure("di.scope_closed");
        return success();
    }
    model::Result<ScopeTicket> beginConstruction() { return begin(ScopeTicket::Kind::Construction); }
    model::Result<ScopeTicket> beginOperation() { return begin(ScopeTicket::Kind::Operation); }
    model::Result<ScopeTicket> beginChild() { return begin(ScopeTicket::Kind::Child); }
    model::Result<void> finishConstruction(const ScopeTicket& ticket) {
        return finish(ticket, ScopeTicket::Kind::Construction);
    }
    model::Result<void> finishOperation(const ScopeTicket& ticket) {
        return finish(ticket, ScopeTicket::Kind::Operation);
    }
    model::Result<void> finishChild(const ScopeTicket& ticket) {
        return finish(ticket, ScopeTicket::Kind::Child);
    }

    model::Result<void> recordInstance(const ScopeTicket& construction,
                                     InstanceOwnership ownership, std::uint64_t cleanupKey) {
        if (!onOwner())
            return failure("di.scope_wrong_owner");
        if (!active(construction, ScopeTicket::Kind::Construction))
            return failure("di.construction_ticket_invalid");
        if (ownership == InstanceOwnership::Borrowed)
            return success();
        if (ownership != InstanceOwnership::Owned)
            return failure("di.instance_ownership_invalid");
        if (owned_.size() == limits_.ownedRecords)
            return failure("di.owned_record_limit");
        if (nextCleanup_ == std::numeric_limits<std::uint64_t>::max())
            return failure("di.cleanup_identity_exhausted");
        // An admitted construction may finish accounting after Closing begins.
        owned_.push_back(CleanupWork(stamp_, nextCleanup_, cleanupKey, construction.serial_));
        ++nextCleanup_;
        return success();
    }

    model::Result<void> beginClose() {
        if (!onOwner())
            return failure("di.scope_wrong_owner");
        if (phase_ == ScopePhase::Open)
            phase_ = ScopePhase::Closing;
        return success();
    }
    model::Result<std::optional<CleanupWork>> claimCleanup() {
        using Claim = model::Result<std::optional<CleanupWork>>;
        if (!onOwner())
            return Claim::Failure("di.scope_wrong_owner");
        if (phase_ == ScopePhase::Open)
            return Claim::Failure("di.scope_not_closing");
        if (hasActivities())
            return Claim::Failure("di.scope_busy");
        if (inFlight_)
            return Claim::Failure("di.cleanup_in_progress");
        if (owned_.empty())
            return Claim::Success(std::nullopt);
        inFlight_ = owned_.back();
        return Claim::Success(inFlight_);
    }
    model::Result<void> completeCleanup(const CleanupWork& work, model::Outcome outcome) {
        if (!onOwner())
            return failure("di.scope_wrong_owner");
        if (!inFlight_ || work.scope_ != stamp_ || work != *inFlight_)
            return failure("di.cleanup_ticket_invalid");
        if (outcome != model::Outcome::Success && outcome != model::Outcome::Failure &&
            outcome != model::Outcome::Cancelled)
            return failure("di.cleanup_outcome_invalid");
        if (outcome == model::Outcome::Failure)
            ++cleanupFailures_;
        if (outcome == model::Outcome::Cancelled)
            ++cleanupCancellations_;
        ++cleanupCompleted_;
        owned_.pop_back();
        inFlight_.reset();
        return success();
    }
    model::Result<void> finishClose() {
        if (!onOwner())
            return failure("di.scope_wrong_owner");
        if (phase_ == ScopePhase::Open)
            return failure("di.scope_not_closing");
        if (hasActivities() || inFlight_ || !owned_.empty())
            return failure("di.scope_busy");
        phase_ = ScopePhase::Closed;
        if (cleanupFailures_ != 0)
            return failure("di.cleanup_failed");
        if (cleanupCancellations_ != 0)
            return model::Result<void>::Cancelled("di.cleanup_cancelled");
        return success();
    }
    model::Result<ScopeStatus> status() const {
        if (!onOwner())
            return model::Result<ScopeStatus>::Failure("di.scope_wrong_owner");
        ScopeStatus result;
        result.phase = phase_;
        result.ownedRemaining = owned_.size();
        result.cleanupCompleted = cleanupCompleted_;
        result.cleanupFailures = cleanupFailures_;
        result.cleanupCancellations = cleanupCancellations_;
        result.cleanupInProgress = inFlight_.has_value();
        for (const auto& entry : activities_) {
            if (!entry.active)
                continue;
            if (entry.ticket.kind_ == ScopeTicket::Kind::Construction)
                ++result.constructions;
            else if (entry.ticket.kind_ == ScopeTicket::Kind::Operation)
                ++result.operations;
            else
                ++result.children;
        }
        return model::Result<ScopeStatus>::Success(result);
    }

private:
    struct ActivityRecord {
        ScopeTicket ticket;
        bool active = false;
    };
    static model::Result<void> failure(const char* code) { return model::Result<void>::Failure(code); }
    static model::Result<void> success() { return model::Result<void>::Success(); }
    bool onOwner() const noexcept { return owner_ == std::this_thread::get_id(); }
    bool hasActivities() const {
        return std::any_of(activities_.begin(), activities_.end(), [](const auto& entry) { return entry.active; });
    }
    bool active(const ScopeTicket& ticket, ScopeTicket::Kind kind) const {
        if (ticket.scope_ != stamp_ || ticket.kind_ != kind)
            return false;
        return std::any_of(activities_.begin(), activities_.end(), [&](const auto& entry) {
            return entry.active && entry.ticket == ticket;
        });
    }
    model::Result<ScopeTicket> begin(ScopeTicket::Kind kind) {
        using Admission = model::Result<ScopeTicket>;
        const auto admitted = requireOpen();
        if (!admitted.is_success())
            return Admission::Failure(admitted.error());
        auto free = std::find_if(activities_.begin(), activities_.end(), [](const auto& entry) { return !entry.active; });
        if (free == activities_.end() && activities_.size() == limits_.activities)
            return Admission::Failure("di.activity_limit");
        if (nextActivity_ == std::numeric_limits<std::uint64_t>::max())
            return Admission::Failure("di.activity_identity_exhausted");
        const ScopeTicket ticket(stamp_, nextActivity_, kind);
        if (free == activities_.end())
            activities_.push_back({ticket, true});
        else
            *free = {ticket, true};
        ++nextActivity_;
        return Admission::Success(ticket);
    }
    model::Result<void> finish(const ScopeTicket& ticket, ScopeTicket::Kind kind) {
        if (!onOwner())
            return failure("di.scope_wrong_owner");
        if (!active(ticket, kind))
            return failure("di.activity_ticket_invalid");
        for (auto& entry : activities_) {
            if (entry.active && entry.ticket == ticket) {
                entry.active = false;
                return success();
            }
        }
        return failure("di.activity_ticket_invalid");
    }

    const std::thread::id owner_;
    const ScopeLimits limits_;
    std::uint64_t stamp_ = 0;
    ScopePhase phase_ = ScopePhase::Open;
    std::uint64_t nextActivity_ = 1;
    std::uint64_t nextCleanup_ = 1;
    std::vector<ActivityRecord> activities_;
    std::vector<CleanupWork> owned_;
    std::optional<CleanupWork> inFlight_;
    std::size_t cleanupCompleted_ = 0;
    std::size_t cleanupFailures_ = 0;
    std::size_t cleanupCancellations_ = 0;
};
}
