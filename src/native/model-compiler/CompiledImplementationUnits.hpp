// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "compiler.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pipelang::models {

// The emitter supplies complete definitions at checked AST boundaries. This
// owner groups them without parsing, changing, or splitting their contents.
class CompiledImplementationUnits {
public:
    static constexpr std::size_t preferredBodyBytes = 64 * 1024;
    static constexpr std::size_t preferredDefinitions = 16;

    explicit CompiledImplementationUnits(std::string nameSpace)
        : nameSpace_(std::move(nameSpace)) {
    }

    void addOwner(const std::string& owner) {
        if (owners_.contains(owner))
            throw Diagnostic("duplicate compiled implementation owner", 0);
        const auto index = addUnit();
        owners_.emplace(owner, index);
    }

    void append(const std::string& owner, const std::string& definition) {
        auto& index = owners_.at(owner);
        const auto& current = units_[index];
        // An oversized complete definition gets a unit of its own. The byte
        // target is a grouping preference, not a new Source acceptance limit.
        const bool exceedsBytes = definition.size() > preferredBodyBytes ||
            current.body.size() > preferredBodyBytes - definition.size();
        if (current.definitions != 0 &&
            (current.definitions >= preferredDefinitions || exceedsBytes)) {
            index = addUnit();
        }
        auto& destination = units_[index];
        if (budgetExceeded(destination.body.size(), definition.size(), resourceBudget().outputBytes))
            throw Diagnostic("compiled implementation output_bytes resource budget exceeded; configure output_bytes", 0);
        destination.body += definition;
        ++destination.definitions;
    }

    std::map<std::string, std::string> files() const {
        std::map<std::string, std::string> result;
        for (std::size_t index = 0; index < units_.size(); ++index) {
            result.emplace("Implementation_" + std::to_string(index) + ".cpp",
                "// Generated project implementation.\n"
                "#include \"Objects.hpp\"\nnamespace " + nameSpace_ + " {\n" +
                units_[index].body + "}\n");
        }
        return result;
    }

private:
    struct Unit {
        std::string body;
        std::size_t definitions = 0;
    };

    std::size_t addUnit() {
        if (units_.size() >= resourceBudget().implementationUnits)
            throw Diagnostic("compiled project implementation_units resource budget exceeded; configure implementation_units", 0);
        const auto index = units_.size();
        units_.emplace_back();
        return index;
    }

    std::string nameSpace_;
    std::map<std::string, std::size_t> owners_;
    std::vector<Unit> units_;
};

}
