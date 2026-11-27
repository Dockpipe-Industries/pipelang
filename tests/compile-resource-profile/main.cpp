// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

int main() {
#ifdef PROFILE_HEADER
    profile_header::source::ResourcePolicy::App app;
#else
    profile_compiled::source::ResourcePolicy::App app;
#endif
    return app.Read() == 23 ? 0 : 1;
}
