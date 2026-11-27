// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../ResourcePortAdapter.hpp"
#include <pipelang/di/ScopeExecution.hpp>
#include <optional>

namespace lifecycle_test::manual {
namespace Contracts = lifecycle_fixture::source::DiLifecycle::Contracts;
namespace Resources = lifecycle_fixture::source::DiLifecycle::Resources;
namespace Factories = lifecycle_fixture::source::DiLifecycle::Factories;
using Execution = pipelang::model::ObjectExecution;
using Context = pipelang::di::ScopeExecution;
using Attempt = pipelang::di::ConstructionAttempt;
using Token = pipelang::model::CancellationToken;
template<class T> using Result = pipelang::model::Result<T>;

template<class T, class U>
Result<T> Propagate(const Result<U>& outcome) {
    if (outcome.is_cancelled())
        return Result<T>::Cancelled(outcome.error());
    return Result<T>::Failure(outcome.error());
}

template<class T>
T UnwrapFactory(const Result<T>& outcome) {
    if (outcome.is_cancelled())
        throw pipelang::model::OperationCancelled(outcome.error());
    if (!outcome.is_success())
        throw pipelang::model::ExecutionFailure(outcome.error());
    return outcome.value();
}

template<class Ref>
Result<void> Cleanup(Ref reference, Execution& execution, Token cancellation) {
    const Contracts::IResourceCleanup cleanup(reference);
    return lifecycle_fixture::construction::SourceMethods<Contracts::IResourceCleanup>::Close(
        execution, cleanup, std::move(cancellation));
}
}
