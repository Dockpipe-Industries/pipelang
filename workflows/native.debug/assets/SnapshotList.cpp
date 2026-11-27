// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_SNAPSHOT_CPP_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifndef PIPELANG_SNAPSHOT_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_snapshot_list;
#endif

int main()
{
    try {
#ifdef PIPELANG_SNAPSHOT_CPP_REFERENCE
        auto initial = pipelang::model::List<std::int64_t>::Empty();
        for (std::int64_t index = 0; index < 4095; ++index)
            initial.add(index);
        reference::source::NativeProof::SnapshotList::Session session(std::move(initial));
        const auto applied = session.Append();
#else
        std::vector<HostedValue> initial;
        initial.reserve(4095);
        for (std::int64_t index = 0; index < 4095; ++index)
            initial.push_back(HostedValue::fromInteger(index));
        const std::array<HostedConstructorArgument, 1> arguments{HostedValue::fromList("List<int>", std::move(initial))};
        const auto bound = HostedInstance::bind(proof_snapshot_list, arguments);
        if (!bound.is_success())
            throw std::runtime_error(bound.error());
        const auto& session = bound.value();
        pipelang::model::TaskScope scope;
        const auto applied = session.start(scope, {}).result();
#endif
        if (!applied.is_success() || applied.value() != 4096)
            throw std::runtime_error("benchmark failed to publish the reference list");

        const auto started = std::chrono::steady_clock::now();
        std::int64_t sum = 0;
        for (std::size_t index = 0; index < 800; ++index) {
#ifdef PIPELANG_SNAPSHOT_CPP_REFERENCE
            const auto values = session.snapshot().values;
            if (values.count() != 4096)
                throw std::runtime_error("snapshot list count changed");
            sum += values.at(4095);
#else
            const auto snapshot = session.snapshot();
            if (!snapshot.is_success())
                throw std::runtime_error(snapshot.error());
            const auto values = snapshot.value().field("values");
            if (values.size() != 4096)
                throw std::runtime_error("snapshot list count changed");
            sum += values.at(4095).integer();
#endif
        }
        const auto finished = std::chrono::steady_clock::now();
        if (sum != 33600)
            throw std::runtime_error("snapshot values changed");
        std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count() << '\n';
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
