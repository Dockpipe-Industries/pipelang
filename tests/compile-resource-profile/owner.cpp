// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

int readFromOwnedModel() {
    profile_compiled::source::ResourcePolicy::App app;
    return static_cast<int>(app.Read());
}
