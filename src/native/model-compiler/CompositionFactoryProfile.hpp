// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompositionGraph.hpp"
#include "syntax.hpp"
#include <map>
#include <set>

namespace pipelang::models {

struct CompositionMethod {
    const Declaration* owner;
    const Method* method;
};

// Admission for the explicitly selected synchronous construction profile.
// The ordinary checked program and graph remain valid independently of this gate.
class CompositionFactoryProfile {
public:
    CompositionFactoryProfile(const Program& program, const std::vector<CompositionGraph>& graphs);
    void admit();
    const Declaration* declaration(const Type& type) const;
    const Method& constructor(const std::string& identity) const;
    CompositionMethod checkedMethod(const std::string& identity) const;
    const std::vector<CompositionMethod>& methods(const Type& type);
    bool ownedValue(const Type& type) const;

private:
    void work(const CompositionOrigin& origin);
    void inspectMethod(const Method& method, const CompositionOrigin& origin);
    void inspectExpression(const Expr& expression, const CompositionOrigin& origin);
    void inspectStatements(const std::vector<Statement>& statements, const CompositionOrigin& origin);
    void inspectHost(const Expr& expression, const CompositionOrigin& origin);
    void queue(const std::string& identity, const CompositionOrigin& origin);
    void inspectFactory(const std::string& identity, const CompositionOrigin& origin);
    void admitFactoryCleanupSubtypes(const CompositionGraph& graph, const CompositionService& service);
    const Program& program_;
    const std::vector<CompositionGraph>& graphs_;
    std::map<std::string, const Declaration*> declarations_;
    std::map<std::string, CompositionMethod> methods_;
    std::map<std::string, std::vector<CompositionMethod>> exportedMethods_;
    std::vector<std::pair<const Method*, CompositionOrigin>> pending_;
    std::set<std::string> admittedMethods_;
    std::size_t work_ = 0;
};

}
