// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResourceTrace.hpp"
#include "CompositionFactory.hpp"
#include <memory>

namespace lifecycle_test {

class ResourcePortAdapter final
    : public lifecycle_fixture::source::DiLifecycle::Contracts::ResourcePort {
public:
    explicit ResourcePortAdapter(std::shared_ptr<ResourceTrace> trace)
        : trace_(std::move(trace)) {
    }

    pipelang::model::Result<std::int64_t> Open(std::string kind, std::int64_t context) override {
        return trace_->Open(std::move(kind), context);
    }

    pipelang::model::Result<void> Close(
        std::int64_t resource, pipelang::model::CancellationToken cancellation) override {
        if (cancellation.is_cancelled())
            return pipelang::model::Result<void>::Cancelled("fixture.close_token_cancelled");
        return trace_->Close(resource);
    }

    pipelang::model::Result<void> Note(
        std::string operation, std::int64_t resource, std::int64_t dependency) override {
        return trace_->Note(std::move(operation), resource, dependency);
    }

    pipelang::model::Result<std::int64_t> Wait(
        std::int64_t resource, pipelang::model::CancellationToken cancellation) override {
        return trace_->Wait(resource, cancellation);
    }

private:
    std::shared_ptr<ResourceTrace> trace_;
};
}
