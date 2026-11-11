// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ScopeData.hpp"

namespace lifecycle_test::manual {

template<class Ref>
class Handle {
public:
    Handle(std::shared_ptr<RootData> root, std::shared_ptr<ScopeData> scope, Ref reference)
        : root_(std::move(root)), scope_(std::move(scope)), reference_(std::move(reference)) {
    }

    Result<std::int64_t> Identity(Token cancellation = {}) const {
        if (!root_)
            return Result<std::int64_t>::Failure("manual moved-from handle");
        auto context = scope_ ? scope_->execution : root_->execution;
        return context->invoke<std::int64_t>(cancellation, [&](Execution& execution) {
            return lifecycle_fixture::construction::SourceMethods<Ref>::Identity(execution, reference_);
        });
    }

    Result<pipelang::model::Task<Result<std::int64_t>>> LocalWork(Token cancellation = {}) const {
        if (!root_)
            return Result<pipelang::model::Task<Result<std::int64_t>>>::Failure("manual moved-from handle");
        auto context = scope_ ? scope_->execution : root_->execution;
        return context->start<std::int64_t>(cancellation,
            [receiver = reference_](Execution& execution, Token taskToken) {
                return lifecycle_fixture::construction::SourceMethods<Ref>::LocalWork(
                    execution, receiver, std::move(taskToken));
            });
    }

    Result<pipelang::model::Task<Result<std::int64_t>>> Work(
        Token authoredToken, Token boundaryToken = {}) const {
        if (!root_)
            return Result<pipelang::model::Task<Result<std::int64_t>>>::Failure("manual moved-from handle");
        try {
            auto context = scope_ ? scope_->execution : root_->execution;
            auto cancellation = Token::combine(boundaryToken, authoredToken);
            return context->start<std::int64_t>(std::move(cancellation),
                [receiver = reference_, authoredToken](Execution& execution, Token taskToken) {
                    return lifecycle_fixture::construction::SourceMethods<Ref>::Work(
                        execution, receiver, authoredToken, std::move(taskToken));
                });
        } catch (const std::exception& error) {
            return Result<pipelang::model::Task<Result<std::int64_t>>>::Failure(
                pipelang::model::safeErrorMessage(error));
        }
    }

    Result<void> ReleaseLocal(Token cancellation = {}) const {
        if (!root_)
            return Result<void>::Failure("manual moved-from handle");
        auto context = scope_ ? scope_->execution : root_->execution;
        return context->invoke<void>(cancellation, [&](Execution& execution) {
            lifecycle_fixture::construction::SourceMethods<Ref>::ReleaseLocal(execution, reference_);
        });
    }

private:
    std::shared_ptr<RootData> root_;
    std::shared_ptr<ScopeData> scope_;
    Ref reference_;
};
}
