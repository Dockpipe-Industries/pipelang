// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <functional>
#include <stdexcept>
#include <string>

namespace pipelang::workspace {
class WorkspaceError final : public std::runtime_error {
public:
    explicit WorkspaceError(const std::string& code) : std::runtime_error("workspace." + code) {}
};
using Cancellation = std::function<bool()>;
inline void checkCancellation(const Cancellation& cancelled) {
    if (cancelled && cancelled())
        throw WorkspaceError("cancelled");
}
}
