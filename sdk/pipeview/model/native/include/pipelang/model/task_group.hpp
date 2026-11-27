// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <cstddef>

namespace pipelang::model {
class TaskScope;

// An owner-thread lifetime tag, not an executor or a service owner. It retains
// only other tags; neither tasks nor tags retain their TaskScope.
class TaskGroup {
public:
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
private:
    friend class TaskScope;
    TaskGroup(std::shared_ptr<const void> identity, std::shared_ptr<TaskGroup> parent)
        : identity_(std::move(identity)), parent_(std::move(parent)) {}
    std::shared_ptr<const void> identity_;
    std::shared_ptr<TaskGroup> parent_;
    bool closing_ = false;
};
}
