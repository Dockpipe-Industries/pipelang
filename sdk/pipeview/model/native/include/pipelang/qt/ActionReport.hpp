// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "InvocationOutcome.hpp"

namespace pipelang::qt {
class ActionReport {
    Q_GADGET
    Q_PROPERTY(pipelang::qt::InvocationOutcome operation READ operation)
    Q_PROPERTY(pipelang::qt::InvocationOutcome projection READ projection)
public:
    ActionReport() = default;
    ActionReport(InvocationOutcome operation, InvocationOutcome projection)
        : operation_(std::move(operation)), projection_(std::move(projection)) {}
    InvocationOutcome operation() const { return operation_; }
    InvocationOutcome projection() const { return projection_; }
private:
    InvocationOutcome operation_;
    InvocationOutcome projection_;
};
}
Q_DECLARE_METATYPE(pipelang::qt::ActionReport)
