// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ResourceBudgetReceipt.hpp"
#include "../model-compiler/ScopedResourceBudget.hpp"
namespace pipelang::tools {
void verifyResourceBudgetUnchanged() {
    const auto path = models::resourceBudgetSourcePath();
    if (!path.empty())
        require(sha256(readFile(path, 65536)) == models::resourceBudgetSourceHash(),
                "resource budget profile changed during compilation");
}
Json resourceBudgetReceipt(const models::ResourceBudget& budget) {
    verifyResourceBudgetUnchanged();
    const auto profile = budget.json();
    Json result{{"profile", profile}, {"profile_sha256", sha256(canonical(profile))}};
    if (budget == models::resourceBudget() && !models::resourceBudgetSourcePath().empty()) {
        result["source"] = models::resourceBudgetSourcePath();
        result["source_sha256"] = models::resourceBudgetSourceHash();
    }
    return result;
}
}
