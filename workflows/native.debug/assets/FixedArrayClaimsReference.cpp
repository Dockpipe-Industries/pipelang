// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::FixedArrayClaims::Session;
using Claims = pipelang::model::FixedArray<std::int64_t, 16>;

namespace {

pipelang::model::Result<Claims> run(Session& session, std::int64_t input)
{
    const auto task = session.Run(input);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("fixed array reference timed out");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

bool matchesReport(const Session::State& snapshot, bool updated)
{
    return snapshot.report.labels.at(0) == (updated ? "retained name" : "initial") &&
        snapshot.report.payload.at(1).decodeUtf8() == (updated ? "retained bytes" : "bound bytes") &&
        snapshot.report.slotIds.at(0) == (updated ? 42 : 7);
}

}

int main()
{
    for (const auto index : {-1, 16}) {
        Session invalid(Claims::Create(-1));
        const auto failed = run(invalid, index);
        if (!failed.is_failure() || failed.error() != "collection index outside admitted range")
            return 1;
        if (run(invalid, 100).value().at(7) != -1)
            return 1;
    }
    Session session(Claims::Create(-1));
    const auto changed = run(session, 15).value();
    if (changed.count() != 16 || changed.at(0) != -1 || changed.at(7) != 42 || changed.at(15) != 42)
        return 1;
    const auto oversized = run(session, 101);
    if (!oversized.is_failure() || oversized.error() != "collection owned byte limit exceeded")
        return 1;
    if (run(session, 102).value().at(15) != 42 || run(session, 103).value().at(15) != 42)
        return 1;
    if (run(session, 104).value().at(15) != 42)
        return 1;
    const auto beforeReport = session.snapshot();
    if (!matchesReport(beforeReport, false) || !run(session, 105).is_success())
        return 1;
    const auto afterReport = session.snapshot();
    if (!matchesReport(afterReport, true))
        return 1;
    const auto reverted = run(session, 106);
    if (!reverted.is_failure() || reverted.error() != "fixture.fixed.rollback" || !run(session, 107).is_success())
        return 1;
    if (!matchesReport(beforeReport, false) || !matchesReport(afterReport, true) || !matchesReport(session.snapshot(), true))
        return 1;
    std::cout << "The C++ reference agreed on fixed array values, snapshots, bounds, ownership limits and rollback.\n";
}
