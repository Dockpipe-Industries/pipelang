// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../ConstructionPolicy.hpp"
#include <pipelang/di/ScopeExecution.hpp>
#include <optional>
#include <vector>

namespace di_tests::manual {
namespace Contracts = di_fixture::source::DiFixture::Contracts;
namespace Roots = di_fixture::source::DiFixture::Root;
namespace Requests = di_fixture::source::DiFixture::Requests;
namespace Application = di_fixture::source::DiFixture::Application;
using Execution = pipelang::model::ObjectExecution;
using Context = pipelang::di::ScopeExecution;
using Token = pipelang::model::CancellationToken;
template<class T> using Result = pipelang::model::Result<T>;

struct RootCache {
    std::optional<Contracts::IFaultyService> faulty;
    std::optional<Contracts::IIdentitySequence> sequence;
    std::optional<Contracts::IRootService> root;
};
struct ScopeCache {
    std::optional<Contracts::IRequestService> request;
    std::optional<Requests::ScopeEcho> echo;
};
struct Retained {
    std::vector<Contracts::IUseCase> useCases;
    std::vector<Roots::RootDependency> dependencies;
};
struct RootState {
    std::shared_ptr<pipelang::di::ExecutionOwner> owner;
    std::shared_ptr<Context> execution;
    std::shared_ptr<ConstructionPolicy> policy;
    RootCache cache;
    Retained retained;
};
struct ScopeState {
    std::shared_ptr<RootState> root;
    std::shared_ptr<Context> execution;
    Contracts::ScopeStamp stamp;
    ScopeCache cache;
    Retained retained;
};
struct Stage {
    RootCache root;
    ScopeCache scope;
    Retained rootPending;
    Retained scopePending;
};

template<class T, class U>
Result<T> Propagate(const Result<U>& outcome) {
    if (outcome.is_cancelled())
        return Result<T>::Cancelled(outcome.error());
    return Result<T>::Failure(outcome.error());
}
}
