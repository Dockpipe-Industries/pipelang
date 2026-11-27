// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResourceJournal.hpp"

namespace pipelang::di {
class ExecutionOwner;
class ScopeExecution;

// Stack-bound authorization for a single generated construction transaction.
class ConstructionAttempt {
public:
    ConstructionAttempt(const ConstructionAttempt&) = delete;
    ConstructionAttempt& operator=(const ConstructionAttempt&) = delete;

    template<class Ref, class Cleanup>
    ResourceJournal::Reservation<Ref> reserve(const std::shared_ptr<ScopeExecution>& scope,
                                             std::string key, Cleanup cleanup);
    std::uint64_t allocationMark() const { return graph_.allocationMark(); }
    template<class Ref>
    void adopt(const ResourceJournal::Reservation<Ref>& reservation, const Ref& reference,
               std::uint64_t mark) {
        journal_.adopt(reservation, reference, graph_, mark, identity_);
    }
    template<class Ref>
    void requireFresh(const Ref& reference, std::uint64_t mark) const {
        journal_.requireFresh(reference, graph_, mark);
    }
private:
    friend class ScopeExecution;
    ConstructionAttempt(ExecutionOwner& owner, ResourceJournal& journal,
                        model::GraphTransaction& graph, std::uint64_t identity)
        : owner_(owner), journal_(journal), graph_(graph), identity_(identity) {}
    ExecutionOwner& owner_;
    ResourceJournal& journal_;
    model::GraphTransaction& graph_;
    const std::uint64_t identity_;
};
}
