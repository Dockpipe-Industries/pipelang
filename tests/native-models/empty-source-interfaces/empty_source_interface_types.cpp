// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Types.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
template<class Reference>
struct ObjectTag;

template<class Tag>
struct ObjectTag<pipelang::model::ObjectRef<Tag>> {
    using Type = Tag;
};
}

int main() {
    try {
        using Unused = empty_interfaces::source::TestingBase::IUnused;
        using Tag = ObjectTag<Unused>::Type;
        if (Tag::accepts(0) || Tag::accepts(1) || Tag::accepts(std::numeric_limits<std::uint64_t>::max()))
            throw std::runtime_error("standalone empty tag admitted a concrete ID");
        const auto absent = pipelang::model::Optional<Unused>::None();
        const auto empty = pipelang::model::List<Unused>::Empty();
        pipelang::model::validate(absent);
        pipelang::model::validate(empty);
        bool refused = false;
        try {
            pipelang::model::validate(Unused{});
        } catch (const pipelang::model::ExecutionFailure&) {
            refused = true;
        }
        if (!refused)
            throw std::runtime_error("standalone empty interface admitted an invalid reference");
        std::cout << "standalone empty source interface types passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
