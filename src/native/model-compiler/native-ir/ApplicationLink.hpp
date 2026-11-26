// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Target.hpp"
#include "../../support/support.hpp"

namespace pipelang::models::native_ir {

struct MacApplicationTools {
    tools::fs::path sysroot;
    std::string sdkVersion;
    tools::fs::path dsymutil;
};

class ApplicationLink {
public:
    ApplicationLink(const Target& target, const tools::fs::path& sdk, std::string digest,
                    const tools::fs::path& linker, const tools::fs::path& output, const tools::fs::path& symbols,
                    MacApplicationTools mac);
    void publish(std::string_view object, const std::function<void()>& verifySources) const;

private:
    bool verifySdk() const;

    Target target_;
    tools::fs::path sdk_;
    std::string digest_;
    tools::fs::path linker_;
    tools::fs::path output_;
    tools::fs::path symbols_;
    MacApplicationTools mac_;
    bool hasLinuxMath_ = false;
};

}
