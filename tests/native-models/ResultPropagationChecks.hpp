// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>

namespace propagation_checks {
using namespace result_propagation;
using pipelang::model::Result;
using IntResult = Result<std::int64_t, FailureData>;

inline FailureData error(std::int64_t request) {
    FailureData value;
    value.code = FailureCode::Conflict;
    value.origin = "caller";
    value.request = request;
    return value;
}

template<class Outcome>
void domainError(const Outcome& outcome, bool cancelled, const std::string& origin, std::int64_t request) {
    assert(outcome.is_success());
    assert(cancelled ? outcome.value().is_cancelled() : outcome.value().is_failure());
    const auto& detail = outcome.value().error();
    assert(detail.code == (cancelled ? FailureCode::Missing : FailureCode::Conflict));
    assert(detail.origin == origin && detail.request == request);
}

template<class Model>
void verify(Model& model) {
    for (std::int64_t mode = 0; mode < 3; ++mode) {
        const auto arguments = model.Arguments(mode);
        if (mode == 0)
            assert(arguments.is_success() && arguments.value().value() == 123);
        else
            domainError(arguments, mode == 2, "reader", 2);
        assert(model.Trace().value() == (mode == 0 ? 123 : 12));
        const auto binary = model.Binary(mode);
        if (mode == 0)
            assert(binary.is_success() && binary.value().value() == 7);
        else
            domainError(binary, mode == 2, "reader", 2);
        assert(model.Trace().value() == (mode == 0 ? 123 : 12));
        const auto skippedAnd = model.And(false, mode);
        assert(skippedAnd.is_success() && !skippedAnd.value().value());
        assert(model.Trace().value() == 0);
        const auto skippedOr = model.Or(true, mode);
        assert(skippedOr.is_success() && skippedOr.value().value());
        assert(model.Trace().value() == 0);
        const auto evaluatedAnd = model.And(true, mode);
        const auto evaluatedOr = model.Or(false, mode);
        if (mode == 0) {
            assert(evaluatedAnd.value().value());
            assert(!evaluatedOr.value().value());
        } else {
            domainError(evaluatedAnd, mode == 2, "reader", 1);
            domainError(evaluatedOr, mode == 2, "reader", 1);
        }
        const auto choice = model.Choice(true, mode, 3);
        if (mode == 0)
            assert(choice.value().value() == 1);
        else
            domainError(choice, mode == 2, "reader", 1);
        assert(model.Trace().value() == 1);
        const auto alternative = model.Choice(false, 3, mode);
        if (mode == 0)
            assert(alternative.value().value() == 2);
        else
            domainError(alternative, mode == 2, "reader", 2);
        assert(model.Trace().value() == 2);
        const auto absent = model.NullableChoice(false, mode);
        assert(absent.is_success() && !absent.value().value().has_value());
        assert(model.Trace().value() == 0);
        const auto condition = model.Condition(mode);
        if (mode == 0)
            assert(condition.value().value() == 7);
        else
            domainError(condition, mode == 2, "reader", 1);
        const auto cleanupBefore = model.CleanupCount().value();
        const auto caughtBefore = model.CaughtCount().value();
        const auto cleaned = model.WithCleanup(mode);
        if (mode == 0)
            assert(cleaned.value().value() == 1);
        else
            domainError(cleaned, mode == 2, "reader", 1);
        assert(model.CleanupCount().value() == cleanupBefore + 1);
        assert(model.CaughtCount().value() == caughtBefore);
        const auto collection = model.Collection(mode);
        const auto assignment = model.Assign(mode);
        if (mode == 0) {
            assert(collection.value().value() == 2);
            assert(assignment.value().value() == 2);
        } else {
            domainError(collection, mode == 2, "reader", 2);
            domainError(assignment, mode == 2, "reader", 2);
        }
        const auto initialized = model.ForInitializer(mode);
        if (mode == 0)
            assert(initialized.value().value() == 3);
        else
            domainError(initialized, mode == 2, "reader", 1);
    }
    assert(model.Stored(0).value().value() == 2);
    domainError(model.Stored(1), false, "reader", 2);
    assert(model.ForNoContinue(-1).value().value() == 3);
    domainError(model.ForNoContinue(1), true, "iteration", 1);
    assert(model.Enum(Result<std::int64_t,FailureCode>::Success(4)).value().value() == 5);
    const auto enumFailure = model.Enum(Result<std::int64_t,FailureCode>::Failure(FailureCode::Conflict));
    assert(enumFailure.value().is_failure() && enumFailure.value().error() == FailureCode::Conflict);
    const auto enumCancelled = model.Enum(Result<std::int64_t,FailureCode>::Cancelled(FailureCode::Missing));
    assert(enumCancelled.value().is_cancelled() && enumCancelled.value().error() == FailureCode::Missing);
    assert(model.String(Result<std::int64_t>::Success(5)).value() == "5");
    const auto stringFailure = model.String(Result<std::int64_t>::Failure("same failure"));
    assert(stringFailure.is_failure() && stringFailure.error() == "same failure");
    const auto stringCancelled = model.String(Result<std::int64_t>::Cancelled("same cancellation"));
    assert(stringCancelled.is_cancelled() && stringCancelled.error() == "same cancellation");
    const auto beforeExhaustion = model.Trace().value();
    const auto exhausted = model.Exhaustion();
    assert(exhausted.is_failure() && exhausted.error() == "source evaluation budget exceeded");
    assert(model.Trace().value() == beforeExhaustion);
    const auto prefix = model.Trace().value();
    const auto fault = model.Arguments(3);
    assert(fault.is_failure() && fault.error() == "reader execution fault");
    assert(model.Trace().value() == prefix);
    const auto recovered = model.WithCleanup(3);
    assert(recovered.value().value() == -1 && model.CaughtCount().value() == 1);
    assert(model.CrossPayload(IntResult::Success(19)).value().value() == "19");
    domainError(model.CrossPayload(IntResult::Failure(error(77))), false, "caller", 77);
    auto cancelledError = error(78);
    cancelledError.code = FailureCode::Missing;
    domainError(model.CrossPayload(IntResult::Cancelled(cancelledError)), true, "caller", 78);
    assert(model.Void(Result<void,FailureData>::Success()).value().is_success());
    domainError(model.Void(Result<void,FailureData>::Failure(error(79))), false, "caller", 79);
    domainError(model.Void(Result<void,FailureData>::Cancelled(cancelledError)), true, "caller", 78);
    assert(model.Nested(Result<IntResult,FailureData>::Success(IntResult::Success(20))).value().value() == 20);
    domainError(model.Nested(Result<IntResult,FailureData>::Success(IntResult::Failure(error(80)))), false, "caller", 80);
    domainError(model.Nested(Result<IntResult,FailureData>::Cancelled(cancelledError)), true, "caller", 78);
    assert(model.While(-1).value().value() == 3 && model.Trace().value() == 4);
    domainError(model.While(1), false, "condition", 1);
    assert(model.Trace().value() == 2);
    const auto beforeFor = model.CleanupCount().value();
    assert(model.For(-1, -1).value().value() == 1 && model.Trace().value() == 3);
    assert(model.CleanupCount().value() == beforeFor + 3);
    domainError(model.For(-1, 0), true, "iteration", 0);
    assert(model.Trace().value() == 1);
    domainError(model.For(1, -1), false, "condition", 1);
    assert(model.Trace().value() == 2);
    pipelang::model::List<std::int64_t> values;
    values.add(0);
    values.add(1);
    values.add(2);
    assert(model.Each(Result<decltype(values),FailureData>::Success(values)).value().value() == 3);
    domainError(model.Each(Result<decltype(values),FailureData>::Failure(error(81))), false, "caller", 81);
}
template<class Model, class Start>
void verifyAsync(Model& model, Start start) {
    for (std::int64_t mode = 0; mode < 3; ++mode) {
        const auto result = start(model.Run(mode));
        assert(result.completed());
        const auto outcome = result.result();
        if (mode == 0)
            assert(outcome.is_success() && outcome.value() == "value 7");
        else if (mode == 1)
            assert(outcome.is_failure() && outcome.error() == "child failure");
        else
            assert(outcome.is_cancelled() && outcome.error() == "child cancelled");
        assert(model.CleanupCount().value() == mode + 1);
        const auto assigned = start(model.Assign(mode));
        assert(assigned.completed());
        const auto assignment = assigned.result();
        if (mode == 0)
            assert(assignment.is_success() && assignment.value() == 7);
        else if (mode == 1)
            assert(assignment.is_failure() && assignment.error() == "child failure");
        else
            assert(assignment.is_cancelled() && assignment.error() == "child cancelled");
        assert(model.Value().value() == (mode == 0 ? 7 : 3));
    }
    const auto callsBefore = model.Calls().value();
    const auto refused = start(model.AwaitArgument(Result<std::int64_t>::Failure("argument failure")));
    assert(refused.completed() && refused.result().is_failure());
    assert(refused.result().error() == "argument failure");
    assert(model.Calls().value() == callsBefore);
    const auto success = start(model.AwaitArgument(Result<std::int64_t>::Success(0)));
    assert(success.completed() && success.result().is_success() && success.result().value() == 7);
    assert(model.Calls().value() == callsBefore + 1);
}

}
