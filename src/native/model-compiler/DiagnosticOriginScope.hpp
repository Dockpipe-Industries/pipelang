// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <utility>

namespace pipelang::models {
// Private nested diagnostic context, set only from an actual source/AST owner.
// Thread-local storage keeps independent/reentrant compiler invocations isolated.
class DiagnosticOriginScope final {
public:
    explicit DiagnosticOriginScope(const std::string& path) : previous_(currentPath_)
    {
        currentPath_ = path;
    }
    ~DiagnosticOriginScope()
    {
        currentPath_ = std::move(previous_);
    }
    DiagnosticOriginScope(const DiagnosticOriginScope&) = delete;
    DiagnosticOriginScope& operator=(const DiagnosticOriginScope&) = delete;
    static const std::string& current()
    {
        return currentPath_;
    }
private:
    inline static thread_local std::string currentPath_;
    std::string previous_;
};
}
