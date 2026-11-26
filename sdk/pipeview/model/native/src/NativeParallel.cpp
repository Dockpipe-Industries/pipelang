// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeParallel.hpp"

#include <pipelang/model/native_host_value.hpp>
#include <pipelang/model/parallel.hpp>

#include <utility>
#include <vector>

namespace pipelang::native_app_v19 {
namespace {

class ParallelSnapshot {
public:
    explicit ParallelSnapshot(const HostedValue& values) : values_(values) {}

    std::vector<HostedValue> values() const
    {
        std::vector<HostedValue> result;
        result.reserve(values_.size());
        for (std::size_t index = 0; index < values_.size(); ++index)
            result.push_back(values_.at(index));
        return result;
    }

    friend void validate(const ParallelSnapshot&) noexcept {}

private:
    const HostedValue& values_;
};

}

void runNativeParallel(const HostedValue& input, model::EvaluationBudget& budget,
                       const model::CancellationToken& cancellation, model::ParallelOptions options, ParallelBody body)
{
    model::parallelForEach(ParallelSnapshot(input), budget, cancellation, options, std::move(body));
}

}
