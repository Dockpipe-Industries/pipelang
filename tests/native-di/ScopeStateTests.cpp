// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/di/ScopeState.hpp>
#include <pipelang/model/object_model.hpp>
#include <array>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace pipelang::di;
using pipelang::model::Outcome;
std::size_t checks = 0;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
    ++checks;
}

void admissionsAndClose()
{
    ScopeState scope({2, 2});
    require(scope.requireOpen().is_success(), "new scope was not open");
    const auto operation = scope.beginOperation().value();
    const auto construction = scope.beginConstruction().value();
    require(scope.beginChild().error() == "di.activity_limit", "activity capacity was ignored");
    require(scope.finishOperation(construction).is_failure(), "wrong activity kind accepted");
    require(scope.finishOperation(operation).is_success(), "operation did not finish");
    require(scope.finishOperation(operation).is_failure(), "operation finished twice");
    const auto child = scope.beginChild().value();
    require(scope.finishOperation(operation).is_failure(), "reused slot accepted stale ticket");
    ScopeState foreign;
    require(foreign.finishChild(child).is_failure(), "foreign scope consumed child ticket");
    require(scope.beginClose().is_success(), "close transition failed");
    require(scope.beginClose().is_success(), "close transition was not idempotent");
    require(scope.status().value().phase == ScopePhase::Closing, "close did not become monotonic Closing");
    require(scope.beginOperation().error() == "di.scope_closing", "operation admitted during close");
    require(scope.beginConstruction().is_failure() && scope.beginChild().is_failure(), "new construction/child admitted during close");
    require(scope.claimCleanup().error() == "di.scope_busy", "cleanup began with active work");
    require(scope.finishClose().is_failure(), "busy scope closed");
    require(scope.finishChild(child).is_success(), "child accounting could not finish during close");
    require(scope.recordInstance(construction, InstanceOwnership::Borrowed, 7).is_success(), "admitted construction could not finish accounting");
    require(scope.finishConstruction(construction).is_success(), "construction could not finish during close");
    require(!scope.claimCleanup().value(), "borrowed instance created cleanup work");
    require(scope.finishClose().is_success() && scope.finishClose().is_success(), "closed result did not repeat safely");
    require(scope.status().value().phase == ScopePhase::Closed, "scope not closed");
    require(scope.requireOpen().error() == "di.scope_closed", "closed scope admitted resolution");
    require(scope.finishChild(ScopeTicket{}).is_failure(), "default ticket accepted");
}

void cleanupProgress()
{
    ScopeState scope({3, 2});
    const auto first = scope.beginConstruction().value();
    require(scope.recordInstance(first, InstanceOwnership::Owned, 10).is_success(), "first owned record failed");
    require(scope.recordInstance(first, InstanceOwnership::Borrowed, 99).is_success(), "borrowed record failed");
    const auto second = scope.beginConstruction().value();
    require(scope.recordInstance(second, InstanceOwnership::Owned, 20).is_success(), "second owned record failed");
    require(scope.recordInstance(second, InstanceOwnership::Owned, 30).error() == "di.owned_record_limit", "owned capacity ignored");
    require(scope.recordInstance(second, static_cast<InstanceOwnership>(99), 30).is_failure(), "invalid ownership accepted");
    require(scope.finishConstruction(first).is_success() && scope.finishConstruction(second).is_success(), "construction accounting failed");
    require(scope.recordInstance(first, InstanceOwnership::Owned, 40).is_failure(), "finished construction registered ownership");
    require(scope.claimCleanup().error() == "di.scope_not_closing", "cleanup claimed while open");
    require(scope.beginClose().is_success(), "close failed");
    const auto last = scope.claimCleanup().value().value();
    require(last.key() == 20, "cleanup was not reverse creation order");
    require(scope.claimCleanup().error() == "di.cleanup_in_progress", "cleanup was claimed twice");
    require(scope.completeCleanup(last, static_cast<Outcome>(99)).is_failure(), "invalid cleanup outcome accepted");
    ScopeState foreign;
    require(foreign.completeCleanup(last, Outcome::Success).is_failure(), "foreign cleanup accepted");
    require(scope.finishClose().is_failure(), "scope closed before cleanup completion");
    require(scope.completeCleanup(last, Outcome::Failure).is_success(), "failed cleanup did not advance progress");
    require(scope.completeCleanup(last, Outcome::Success).is_failure(), "cleanup completed twice");
    const auto earlier = scope.claimCleanup().value().value();
    require(earlier.key() == 10 && earlier.constructionAttempt() != last.constructionAttempt(), "attempt identity was lost");
    require(scope.completeCleanup(earlier, Outcome::Cancelled).is_success(), "cancelled cleanup did not advance progress");
    require(!scope.claimCleanup().value(), "completed cleanup replayed");
    require(scope.finishClose().error() == "di.cleanup_failed", "failure precedence was lost");
    require(scope.finishClose().error() == "di.cleanup_failed", "close outcome was not stable");
    const auto status = scope.status().value();
    require(status.phase == ScopePhase::Closed && status.cleanupCompleted == 2 &&
            status.cleanupFailures == 1 && status.cleanupCancellations == 1 && status.ownedRemaining == 0,
            "cleanup status lost exact progress");

    ScopeState cancelled;
    const auto attempt = cancelled.beginConstruction().value();
    require(cancelled.recordInstance(attempt, InstanceOwnership::Owned, 0).is_success(), "zero compiler key rejected");
    require(cancelled.finishConstruction(attempt).is_success(), "cancel fixture construction failed");
    require(cancelled.beginClose().is_success(), "cancel fixture close failed");
    const auto work = cancelled.claimCleanup().value().value();
    require(cancelled.completeCleanup(work, Outcome::Cancelled).is_success(), "cancel cleanup failed");
    require(cancelled.finishClose().is_cancelled() && cancelled.finishClose().error() == "di.cleanup_cancelled", "cleanup cancellation relabelled");
}

void survivesGraphRollback()
{
    ScopeState scope;
    const auto attempt = scope.beginConstruction().value();
    require(scope.recordInstance(attempt, InstanceOwnership::Owned, 1).is_success(), "rollback fixture ownership failed");
    require(scope.finishConstruction(attempt).is_success(), "rollback fixture construction failed");
    int applicationState = 7;
    pipelang::model::ObjectOwner owner;
    try {
        pipelang::model::GraphModelTransaction<int> transaction(applicationState, owner.domain());
        transaction.state() = 99;
        require(scope.beginClose().is_success(), "rollback fixture close failed");
        const auto work = scope.claimCleanup().value().value();
        require(scope.completeCleanup(work, Outcome::Failure).is_success(), "rollback fixture cleanup failed");
        throw pipelang::model::ExecutionFailure("fixture rollback");
    } catch (const pipelang::model::ExecutionFailure&) {
    }
    require(applicationState == 7, "ordinary graph transaction did not roll back");
    require(scope.status().value().phase == ScopePhase::Closing && scope.status().value().cleanupCompleted == 1,
            "ordinary rollback reopened scope or replayed cleanup");
    require(scope.finishClose().is_failure() && scope.status().value().phase == ScopePhase::Closed,
            "cleanup failure prevented monotonic close");
}

void ownerAndIdentity()
{
    ScopeState scope;
    const auto operation = scope.beginOperation().value();
    bool rejected = false;
    std::thread wrongOwner([&] {
        rejected = scope.status().error() == "di.scope_wrong_owner" &&
            scope.beginClose().is_failure() && scope.beginOperation().is_failure() &&
            scope.finishOperation(operation).is_failure();
    });
    wrongOwner.join();
    require(rejected, "worker accessed owner-confined lifecycle");
    require(scope.status().value().phase == ScopePhase::Open && scope.status().value().operations == 1,
            "wrong-owner calls mutated lifecycle");
    require(scope.finishOperation(operation).is_success(), "owner could not finish accounting");

    alignas(ScopeState) std::byte storage[sizeof(ScopeState)];
    auto* oldScope = std::construct_at(reinterpret_cast<ScopeState*>(storage));
    const auto stale = oldScope->beginOperation().value();
    std::destroy_at(oldScope);
    auto* replacement = std::construct_at(reinterpret_cast<ScopeState*>(storage));
    const auto current = replacement->beginOperation().value();
    require(replacement->finishOperation(stale).is_failure(), "reused address accepted stale scope identity");
    require(replacement->finishOperation(current).is_success(), "replacement owner ticket failed");
    std::destroy_at(replacement);

    // Destruction invokes no cleanup callback and mutates no owner/parent state.
    auto abandoned = std::make_unique<ScopeState>();
    std::thread destroyer([owned = std::move(abandoned)]() mutable { owned.reset(); });
    destroyer.join();
    require(scope.status().value().phase == ScopePhase::Open, "foreign destructor mutated another owner");
}

void capacitiesAndStampExhaustion()
{
    for (const ScopeLimits invalid : {ScopeLimits{0, 1}, ScopeLimits{1, 0},
             ScopeLimits{ScopeLimits::maximumEntries + 1, 1}, ScopeLimits{1, ScopeLimits::maximumEntries + 1}}) {
        bool refused = false;
        try {
            ScopeState scope(invalid);
        } catch (const pipelang::model::ExecutionFailure& error) {
            refused = std::string(error.what()) == "di.scope_limits_invalid";
        }
        require(refused, "invalid capacity admitted");
    }
    ScopeState maximum({ScopeLimits::maximumEntries, ScopeLimits::maximumEntries});
    std::vector<ScopeTicket> tickets;
    for (std::size_t index = 0; index < ScopeLimits::maximumEntries; ++index)
        tickets.push_back(maximum.beginOperation().value());
    require(maximum.beginOperation().error() == "di.activity_limit", "maximum activity bound ignored");
    for (const auto& ticket : tickets)
        require(maximum.finishOperation(ticket).is_success(), "configured activity accounting failed");
    const auto attempt = maximum.beginConstruction().value();
    for (std::size_t index = 0; index < ScopeLimits::maximumEntries; ++index)
        require(maximum.recordInstance(attempt, InstanceOwnership::Owned, index).is_success(), "configured owned limit failed early");
    require(maximum.recordInstance(attempt, InstanceOwnership::Owned, 99999).is_failure(), "maximum owned limit ignored");
    require(maximum.finishConstruction(attempt).is_success() && maximum.beginClose().is_success(), "maximum fixture close failed");
    for (std::size_t remaining = ScopeLimits::maximumEntries; remaining != 0; --remaining) {
        const auto work = maximum.claimCleanup().value().value();
        require(work.key() == remaining - 1, "large cleanup order changed");
        require(maximum.completeCleanup(work, Outcome::Success).is_success(), "large cleanup progress failed");
    }
    require(maximum.finishClose().is_success(), "maximum fixture did not close");

    detail::ScopeStampSequence exhausted(std::numeric_limits<std::uint64_t>::max() - 1);
    require(exhausted.next().value() == std::numeric_limits<std::uint64_t>::max() - 1, "last stamp mismatch");
    require(exhausted.next().error() == "di.scope_identity_exhausted" && exhausted.next().is_failure(), "scope stamp wrapped or exhaustion was unstable");
    detail::ScopeStampSequence invalid(0);
    require(invalid.next().is_failure(), "zero scope stamp admitted");
    detail::ScopeStampSequence shared;
    std::array<std::array<std::uint64_t, 64>, 4> stamps{};
    std::array<std::thread, 4> threads;
    for (std::size_t worker = 0; worker < threads.size(); ++worker) {
        threads[worker] = std::thread([&, worker] {
            for (auto& value : stamps[worker])
                value = shared.next().value();
        });
    }
    for (auto& thread : threads)
        thread.join();
    std::set<std::uint64_t> unique;
    for (const auto& values : stamps)
        unique.insert(values.begin(), values.end());
    require(unique.size() == 256 && *unique.begin() == 1 && *unique.rbegin() == 256, "technical identity sequence duplicated stamps");
}
}

int main()
{
    try {
        admissionsAndClose();
        cleanupProgress();
        survivesGraphRollback();
        ownerAndIdentity();
        capacitiesAndStampExhaustion();
        std::cout << "PASS " << checks << " optional DI lifecycle assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
