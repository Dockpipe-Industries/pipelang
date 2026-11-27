// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeReceiverJournal.hpp"
#include "native_app_execution.hpp"

#include <pipelang/model/cleanup.hpp>
#include <pipelang/model/synchronization.hpp>

#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace pipelang::native_app_v19 {

class HostedService;

struct NativeHostedConstruction {
    struct CleanupRegion {
        CleanupRegion(model::EvaluationBudget& budget, model::AsyncFinallyFailure primary)
            : failure(std::move(primary)), evaluation(budget) {}

        model::AsyncFinallyFailure failure;
        model::EvaluationBudget::CleanupRegion evaluation;
    };

    NativeReceiverStorage& storage;
    std::span<const HostServiceBinding> services;
    HostedInput& values;
    NativeLists& lists;
    model::EvaluationBudget budget;
    std::exception_ptr failure{};
    // A routed fault is taken by the next catch or saved by its finally.
    std::exception_ptr pendingFailure{};
    std::vector<std::unique_ptr<CleanupRegion>> cleanup{};
    ReceiverTransactions transactions{storage};
    // The bound constructor is the first frame in its source ancestry.
    std::size_t sourceDepth = 1;
    std::vector<std::pair<ReceiverSynchronizationKind, std::uint64_t>> synchronization{};
    std::vector<model::Lock::Guard> lockGuards{};
    ReceiverTextBorrows textBorrows{};
};

template<class Function>
std::int32_t withHostedConstruction(void* context, const Function& function) noexcept
{
    auto& construction = *static_cast<NativeHostedConstruction*>(context);
    try {
        function(construction);
        return 0;
    } catch (...) {
        construction.failure = std::current_exception();
        return 3;
    }
}

}
