// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ManualState.hpp"

namespace di_tests::manual {

// Test-only guarded facade. No raw source reference is exposed to the benchmark.
// Strong root/scope state retains borrowed inputs and the same lifetime context.
template<class Reference>
class ManualHandle {
public:
    ManualHandle(std::shared_ptr<RootState> root, std::shared_ptr<ScopeState> scope, Reference reference)
        : root_(std::move(root)), scope_(std::move(scope)), reference_(std::move(reference)) {}

    Result<std::int64_t> Identity(Token cancellation = {}) const {
        return invoke<std::int64_t>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::Identity(execution, reference_);
        });
    }
    Result<std::int64_t> Count(Token cancellation = {}) const {
        return invoke<std::int64_t>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::Count(execution, reference_);
        });
    }
    Result<std::int64_t> DependencyIdentity(Token cancellation = {}) const {
        return invoke<std::int64_t>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::DependencyIdentity(execution, reference_);
        });
    }
    Result<std::int64_t> RequestIdentity(Token cancellation = {}) const {
        return invoke<std::int64_t>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::RequestIdentity(execution, reference_);
        });
    }
    Result<std::int64_t> ScopeIdentity(Token cancellation = {}) const {
        return invoke<std::int64_t>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::ScopeIdentity(execution, reference_);
        });
    }
    Result<std::int64_t> RootIdentity(Token cancellation = {}) const {
        return invoke<std::int64_t>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::RootIdentity(execution, reference_);
        });
    }
    Result<std::string> Tenant(Token cancellation = {}) const {
        return invoke<std::string>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::Tenant(execution, reference_);
        });
    }
    Result<std::int64_t> OperationCount(Token cancellation = {}) const {
        return invoke<std::int64_t>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::OperationCount(execution, reference_);
        });
    }
    Result<std::int64_t> ReadPolicy(Token cancellation = {}) const {
        const auto outcome = invoke<Result<std::int64_t>>(cancellation, [&](Execution& execution) {
            return di_fixture::construction::SourceMethods<Reference>::ReadPolicy(execution, reference_);
        });
        if (!outcome.is_success())
            return Propagate<std::int64_t>(outcome);
        return outcome.value();
    }

private:
    template<class T, class Function>
    Result<T> invoke(Token cancellation, Function&& function) const {
        if (!root_)
            return Result<T>::Failure("manual handle is moved from");
        const auto context = scope_ ? scope_->execution : root_->execution;
        return context->invoke<T>(cancellation, std::forward<Function>(function));
    }
    std::shared_ptr<RootState> root_;
    std::shared_ptr<ScopeState> scope_;
    Reference reference_;
};
}
