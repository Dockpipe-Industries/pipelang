// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CleanupStatus.hpp"
#include "ScopeState.hpp"
#include <pipelang/model/task.hpp>
#include <memory>

namespace pipelang::di::detail {
// Tasks retain this owner-free state, never an ExecutionOwner or public facade.
struct ScopeContext {
    explicit ScopeContext(std::uint64_t identity, ScopeLimits limits)
        : id(identity), lifecycle(limits),
          closeResult(std::make_shared<model::TaskState<model::Result<void>>>()) {}

    const std::uint64_t id;
    ScopeState lifecycle;
    CleanupStatus cleanup;
    std::weak_ptr<ScopeContext> parent;
    std::vector<std::shared_ptr<ScopeContext>> children;
    std::optional<ScopeTicket> parentTicket;
    std::optional<ScopeTicket> rootTicket;
    bool parentReleased = false;
    bool diagnosticsRolledUp = false;
    bool closeRequested = false;
    std::shared_ptr<model::TaskGroup> tasks;
    std::shared_ptr<model::TaskState<model::Result<void>>> closeResult;
};
}
