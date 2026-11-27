// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "manifest.hpp"
#include "../model-compiler/ResourceBudget.hpp"
namespace pipelang::tools {
Json resourceBudgetReceipt(const models::ResourceBudget& budget = models::resourceBudget());
void verifyResourceBudgetUnchanged();
}
