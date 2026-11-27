// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>

namespace pipelang::model {
class CancellationToken;
class EvaluationBudget;
struct ParallelOptions;
}

namespace pipelang::native_app_v19 {

class HostedValue;
using ParallelBody = std::function<void(const HostedValue&, model::EvaluationBudget&)>;

void runNativeParallel(const HostedValue& input, model::EvaluationBudget& budget,
                       const model::CancellationToken& cancellation, model::ParallelOptions options, ParallelBody body);

}
