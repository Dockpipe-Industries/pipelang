// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResourceEvent.hpp"
#include <vector>

namespace lifecycle_test {

// Independent oracle for the source fixture's declared dependency/lifetime graph.
// Resource1 is pre-opened by the external owner; source acquisition starts at2.
class ExpectedResourceTrace {
public:
    static std::vector<ResourceEvent> NormalTwoScopes() {
        std::vector<ResourceEvent> events{
            {"open", 1, "borrowed", 0},
            {"open", 2, "root-dependency", 0},
            {"open", 3, "root", 0},
            {"open", 4, "scoped", 11},
            {"open", 5, "transient", 11},
            {"open", 6, "scoped", 22},
            {"open", 7, "transient", 22}
        };
        AppendCleanup(events, 5, "transient", 11, 4, false);
        AppendCleanup(events, 4, "scoped", 11, 3, true);
        AppendCleanup(events, 7, "transient", 22, 6, false);
        AppendCleanup(events, 6, "scoped", 22, 3, true);
        AppendCleanup(events, 3, "root", 0, 2, false);
        AppendCleanup(events, 2, "root-dependency", 0, 2, false);
        return events;
    }

private:
    static void AppendCleanup(std::vector<ResourceEvent>& events, std::int64_t resource,
        const std::string& kind, std::int64_t context, std::int64_t dependency, bool scoped) {
        if (scoped)
            events.push_back({"cleanup-enter", resource, kind, context, dependency});
        events.push_back({"cleanup-live", resource, kind, context, dependency});
        events.push_back({"close-attempt", resource, kind, context});
        events.push_back({"close-effect", resource, kind, context});
    }
};
}
