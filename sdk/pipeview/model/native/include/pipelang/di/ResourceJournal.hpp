// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ScopeContext.hpp"
#include <pipelang/model/object.hpp>
#include <functional>
#include <limits>

namespace pipelang::di {
class ConstructionAttempt;

// One bounded, owner-confined journal across all composition lifetimes.
// It records ownership, not service lookup or a dynamically extensible registry.
class ResourceJournal {
public:
    explicit ResourceJournal(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0 || capacity > ScopeLimits::maximumEntries)
            throw model::ExecutionFailure("di.owned_record_limit_invalid");
        records_.reserve(capacity);
    }
    ResourceJournal(const ResourceJournal&) = delete;
    ResourceJournal& operator=(const ResourceJournal&) = delete;

    template<class Ref>
    class Reservation {
    public:
        Reservation(const Reservation&) = default;
        Reservation& operator=(const Reservation&) = default;
    private:
        friend class ResourceJournal;
        Reservation(std::shared_ptr<const void> owner, std::uint64_t serial,
                    std::shared_ptr<std::optional<Ref>> slot)
            : owner_(std::move(owner)), serial_(serial), slot_(std::move(slot)) {}
        std::shared_ptr<const void> owner_;
        std::uint64_t serial_;
        std::shared_ptr<std::optional<Ref>> slot_;
    };

    template<class Ref, class Cleanup>
    Reservation<Ref> reserve(const std::shared_ptr<detail::ScopeContext>& scope,
                             std::uint64_t attempt, std::string key, Cleanup cleanup) {
        const auto open = scope->lifecycle.requireOpen();
        if (!open.is_success())
            throw model::ExecutionFailure(open.error());
        model::validate(key);
        if (records_.size() == capacity_)
            throw model::ExecutionFailure("di.owned_record_limit");
        if (nextSerial_ == std::numeric_limits<std::uint64_t>::max() ||
            nextOrder_ > std::numeric_limits<std::uint64_t>::max() - capacity_)
            throw model::ExecutionFailure("di.ownership_identity_exhausted");
        auto slot = std::make_shared<std::optional<Ref>>();
        Record record;
        record.scope = scope;
        record.attempt = attempt;
        record.key = std::move(key);
        record.serial = nextSerial_;
        record.cleanup = [slot, cleanup = std::move(cleanup)](
            model::ObjectExecution& execution, model::CancellationToken cancellation) mutable {
            return std::invoke(cleanup, **slot, execution, std::move(cancellation));
        };
        // Every allocation and callback capture is admitted before acquisition.
        records_.push_back(std::move(record));
        ++nextSerial_;
        return Reservation<Ref>(identity_, records_.back().serial, std::move(slot));
    }

    template<class Ref>
    void adopt(const Reservation<Ref>& reservation, const Ref& reference,
               model::GraphTransaction& graph, std::uint64_t mark, std::uint64_t attempt) {
        static_assert(std::is_nothrow_copy_constructible_v<Ref>);
        if (reservation.owner_ != identity_)
            throw model::ExecutionFailure("di.ownership_reservation_foreign");
        auto& record = checkedReservation(reservation.serial_, attempt);
        if (record.state != State::Reserved)
            throw model::ExecutionFailure("di.ownership_reservation_consumed");
        requireFresh(reference, graph, mark);
        reservation.slot_->emplace(reference);
        record.identity = reference.identity();
        record.order = nextOrder_++;
        record.state = State::Live;
        ++record.scope->cleanup.pending;
    }

    template<class Ref>
    void requireFresh(const Ref& reference, const model::GraphTransaction& graph,
                      std::uint64_t mark) const {
        if (!graph.allocatedSince(reference, mark))
            throw model::ExecutionFailure("di.factory_result_not_fresh");
        const auto identity = reference.identity();
        for (const auto& record : records_) {
            if (record.state != State::Reserved && record.identity == identity)
                throw model::ExecutionFailure("di.resource_already_owned");
        }
    }

    void publish(std::uint64_t attempt) noexcept {
        for (auto& record : records_) {
            if (record.attempt == attempt && record.state == State::Live)
                record.published = true;
        }
        eraseFinished(attempt);
    }

    // The caller owns one cleanup budget/token/session. A pre-claim exhaustion
    // leaves the record pending; a fault inside a claimed callback is terminal.
    template<class Select>
    bool cleanup(model::ObjectExecution& execution, model::CancellationToken cancellation,
                 Select select) {
        auto region = execution.budget.cleanup();
        for (;;) {
            Record* next = nullptr;
            for (auto& record : records_) {
                if (record.state == State::Live && select(record.scope, record.attempt, record.published) &&
                    (!next || record.order > next->order))
                    next = &record;
            }
            if (!next)
                return true;
            try {
                execution.budget.step();
            } catch (const model::EvaluationLimitFailure&) {
                return false;
            }
            next->state = State::Claimed;
            auto& status = next->scope->cleanup;
            --status.pending;
            ++status.attempted;
            try {
                const auto outcome = next->cleanup(execution, cancellation);
                if (outcome.is_success()) {
                    ++status.succeeded;
                } else if (outcome.is_cancelled()) {
                    ++status.cancelled;
                    ++status.unknown;
                } else {
                    ++status.failed;
                    ++status.unknown;
                }
            } catch (const model::OperationCancelled&) {
                ++status.cancelled;
                ++status.unknown;
            } catch (...) {
                ++status.failed;
                ++status.unknown;
            }
            next->state = State::Done;
        }
    }

    void eraseFinished(std::uint64_t attempt = 0) noexcept {
        records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const auto& record) {
            return (attempt == 0 || record.attempt == attempt) &&
                   (record.state == State::Reserved || record.state == State::Done);
        }), records_.end());
    }

private:
    enum class State { Reserved, Live, Claimed, Done };
    struct Record {
        std::shared_ptr<detail::ScopeContext> scope;
        std::function<model::Result<void>(model::ObjectExecution&, model::CancellationToken)> cleanup;
        model::ObjectId identity;
        std::uint64_t attempt = 0;
        std::string key;
        std::uint64_t serial = 0;
        std::uint64_t order = 0;
        State state = State::Reserved;
        bool published = false;
    };
    Record& checkedReservation(std::uint64_t serial, std::uint64_t attempt) {
        for (auto& record : records_) {
            if (record.serial == serial && record.attempt == attempt)
                return record;
        }
        throw model::ExecutionFailure("di.ownership_reservation_invalid");
    }
    const std::size_t capacity_;
    const std::shared_ptr<const void> identity_ = std::make_shared<unsigned char>(0);
    std::vector<Record> records_;
    std::uint64_t nextSerial_ = 1;
    std::uint64_t nextOrder_ = 1;
};
}
