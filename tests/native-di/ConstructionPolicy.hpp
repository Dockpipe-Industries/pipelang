// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CompositionFactory.hpp"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace di_tests {

// The decision is a read-only external input during one Source invocation.
// Tracing is test instrumentation and never changes returned decisions or IDs.
class ConstructionPolicy final : public di_fixture::source::DiFixture::Contracts::ConstructionPolicy {
public:
    enum class Decision { Success, Failure, Cancelled, ThrowCancellation };
    struct ReadEvent {
        std::string kind;
        std::int64_t context;
    };

    pipelang::model::Result<std::int64_t> Read(std::string kind, std::int64_t context) const override {
        if (trace)
            reads.push_back({kind, context});
        if (onRead)
            onRead(kind, context);
        if (kind == selectedKind) {
            switch (decision) {
            case Decision::Failure:
                return pipelang::model::Result<std::int64_t>::Failure("fixture.domain_failure");
            case Decision::Cancelled:
                return pipelang::model::Result<std::int64_t>::Cancelled("fixture.domain_cancelled");
            case Decision::ThrowCancellation:
                throw pipelang::model::OperationCancelled("fixture.boundary_cancelled");
            case Decision::Success:
                break;
            }
        }
        return pipelang::model::Result<std::int64_t>::Success(0);
    }

    std::size_t ReadsOf(const std::string& kind) const {
        std::size_t count = 0;
        for (const auto& read : reads) {
            if (read.kind == kind)
                ++count;
        }
        return count;
    }

    bool trace = true;
    Decision decision = Decision::Success;
    std::string selectedKind;
    std::function<void(const std::string&, std::int64_t)> onRead;
    mutable std::vector<ReadEvent> reads;
};

}
