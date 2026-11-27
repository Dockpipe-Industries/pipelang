// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "InvocationOutcome.hpp"
#include <pipelang/model/context.hpp>
#include <QPointer>
#include <memory>
#include <optional>
#include <vector>

namespace pipelang::qt {
class ContextualObject;

// Registries contain weak QObject references only. Holding a prototype identity
// never retains a model, a projection or an expired operation receiver.
struct PrototypeIdentity {
    QPointer<ContextualObject> prototype;
    std::vector<QPointer<ContextualObject>> operations;
};

struct ContextBinding {
    std::shared_ptr<::pipelang::model::ContextOwner> owner;
    ::pipelang::model::ContextLease lease;
    ::pipelang::model::InvocationPermit permit;
    std::optional<::pipelang::model::OperationTicket> operation;
    std::shared_ptr<::pipelang::model::OperationLane> lane;
    std::shared_ptr<PrototypeIdentity> origin;
    QPointer<QObject> root;
    QString correlation;
    bool registeredOperation = false;
    // Only an explicitly captured permanent root command may hand publication
    // back to that root after current operation admission has been checked.
    bool supervisedRoot = false;
};
}
