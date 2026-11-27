// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace pipelang::di {
class ScopeState;

// A plain accounting ticket. Copies never complete work on destruction.
class ScopeTicket {
public:
    ScopeTicket() = default;
    bool operator==(const ScopeTicket&) const = default;
private:
    friend class ScopeState;
    enum class Kind { Construction, Operation, Child };
    ScopeTicket(std::uint64_t scope, std::uint64_t serial, Kind kind)
        : scope_(scope), serial_(serial), kind_(kind) {}
    std::uint64_t scope_ = 0;
    std::uint64_t serial_ = 0;
    Kind kind_ = Kind::Operation;
};
}
