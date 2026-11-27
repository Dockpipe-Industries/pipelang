// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_fixed_array_claims;

namespace {

HostedValue seed(std::size_t length)
{
    return HostedValue::fromList("FixedArray<int,16>", std::vector<HostedValue>(length, HostedValue::fromInteger(-1)));
}

HostedValueResult run(const HostedInstance& instance, TaskScope& scope, std::int64_t scenario)
{
    const std::array<HostedArgument, 1> arguments{scenario};
    const auto task = instance.startValue(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (task.completed())
        throw std::runtime_error("fixed array invocation did not suspend");
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("fixed array invocation timed out");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void verifyReport(const HostedValue& snapshot, bool updated)
{
    const auto report = snapshot.field("report");
    const auto contents = report.field("payload").at(1).bytes();
    require(report.field("labels").at(0).text() == (updated ? "retained name" : "initial") &&
                std::string(contents.begin(), contents.end()) == (updated ? "retained bytes" : "bound bytes") &&
                report.field("slotIds").at(0).integer() == (updated ? 42 : 7),
            "owned or numeric fixed arrays changed in a hosted snapshot");
}

}

int main()
{
    try {
        const std::array<HostedConstructorArgument, 1> wrongExtent{seed(15)};
        require(HostedInstance::bind(proof_fixed_array_claims, wrongExtent).is_failure(),
                "fixed array binding accepted the wrong extent");
        const std::array<HostedConstructorArgument, 1> initial{seed(16)};
        const auto created = HostedInstance::bind(proof_fixed_array_claims, initial);
        if (!created.is_success())
            throw std::runtime_error(created.error());

        TaskScope scope;
        const auto& session = created.value();
        for (const auto index : {-1, 16}) {
            const auto failed = run(session, scope, index);
            require(failed.is_failure() && failed.error() == "collection index outside admitted range",
                    "fixed array admitted an invalid index");
            require(run(session, scope, 100).value().at(7).integer() == -1,
                    "out-of-bounds claim left a partial write");
        }
        const auto change = run(session, scope, 15).value();
        require(change.kind() == HostedValue::Kind::List && change.typeName() == "FixedArray<int,16>" &&
                    change.size() == 16 && change.at(0).integer() == -1 && change.at(7).integer() == 42 &&
                    change.at(15).integer() == 42,
                "hosted fixed array result lost its checked values");
        const auto oversized = run(session, scope, 101);
        require(oversized.is_failure() && oversized.error() == "collection owned byte limit exceeded",
                "fixed array construction exceeded its owned byte limit");
        require(run(session, scope, 102).value().at(15).integer() == 42,
                "failed replacement changed a persisted claim");
        require(run(session, scope, 103).value().at(15).integer() == 42,
                "fixed array iteration lost its snapshot");
        require(run(session, scope, 104).value().at(15).integer() == 42,
                "bounded claim matrix changed the hosted receiver");
        const auto beforeReport = session.snapshot();
        require(beforeReport.is_success(), "could not snapshot the original fixed arrays");
        verifyReport(beforeReport.value(), false);
        require(run(session, scope, 105).is_success(), "could not publish owned fixed arrays");
        const auto afterReport = session.snapshot();
        require(afterReport.is_success(), "could not snapshot committed fixed arrays");
        verifyReport(afterReport.value(), true);
        const auto reverted = run(session, scope, 106);
        require(reverted.is_failure() && reverted.error() == "fixture.fixed.rollback",
                "fixed-array rollback lost its primary error");
        require(run(session, scope, 107).is_success(), "owned fixed arrays did not survive receiver rollback");
        verifyReport(beforeReport.value(), false);
        verifyReport(afterReport.value(), true);
        verifyReport(session.snapshot().value(), true);
        std::cout << "Fixed arrays preserved hosted values, snapshots, bounds, ownership limits and rollback.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
