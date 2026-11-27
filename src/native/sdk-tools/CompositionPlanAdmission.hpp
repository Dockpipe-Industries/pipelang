// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "manifest.hpp"

namespace pipelang::tools {

// Validate metadata received across the pinned compiler process boundary.
void admitCompositionPlans(const Json& plans);

}
