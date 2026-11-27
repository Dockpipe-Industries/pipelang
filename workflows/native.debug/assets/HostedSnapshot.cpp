// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_SNAPSHOT_CPP_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef PIPELANG_SNAPSHOT_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_hosted_snapshot;
#endif

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

#ifdef PIPELANG_SNAPSHOT_CPP_REFERENCE
using Session = reference::source::NativeProof::HostedSnapshot::Session;
using Snapshot = Session::State;
#else
using Snapshot = HostedValue;

class Session {
public:
    auto Lifetime()
    {
        const auto result = binding_.startContextOwner(scope_, "Lifetime").result();
        require(result.is_success(), result.is_success() ? "" : result.error());
        return result.value();
    }

    auto Run(std::int64_t scenario)
    {
        const std::array<HostedArgument, 1> args{scenario};
        return binding_.start(scope_, args).result();
    }

    Snapshot snapshot() const
    {
        const auto snapshot = binding_.snapshot();
        require(snapshot.is_success(), snapshot.is_success() ? "" : snapshot.error());
        return snapshot.value();
    }

private:
    static HostedInstance bind()
    {
        auto bound = HostedInstance::bind(proof_hosted_snapshot, {});
        require(bound.is_success(), bound.is_success() ? "" : bound.error());
        return std::move(bound).value();
    }

    HostedInstance binding_ = bind();
    pipelang::model::TaskScope scope_;
};
#endif

void verify(const Snapshot& snapshot, bool changed)
{
#ifdef PIPELANG_SNAPSHOT_CPP_REFERENCE
    require(snapshot.count == (changed ? 42 : 0), "scalar state changed");
    require(snapshot.phase == (changed ? "published" : "initial"), "text state changed");
    require(snapshot.report.label == (changed ? "changed" : "initial"), "record text changed");
    require(snapshot.report.values.count() == (changed ? 1 : 0), "list state changed");
    require(snapshot.report.keys.count() == (changed ? 1 : 0), "set state changed");
    if (changed)
        require(snapshot.report.values.at(0) == 42 && snapshot.report.keys.contains("new"), "owned contents changed");
#else
    require(snapshot.typeName() == "NativeProof.HostedSnapshot.Session.State", "snapshot identity changed");
    require(snapshot.field("count").integer() == (changed ? 42 : 0), "scalar state changed");
    require(snapshot.field("phase").text() == (changed ? "published" : "initial"), "text state changed");
    const auto report = snapshot.field("report");
    require(report.field("label").text() == (changed ? "changed" : "initial"), "record text changed");
    const auto values = report.field("values");
    const auto keys = report.field("keys");
    require(values.size() == (changed ? 1 : 0), "list state changed");
    require(keys.size() == (changed ? 1 : 0), "set state changed");
    if (changed)
        require(values.at(0).integer() == 42 && keys.at(0).text() == "new", "owned contents changed");
    try {
        (void)snapshot.field("secret");
        throw std::runtime_error("snapshot exported a private field");
    } catch (const pipelang::model::ExecutionFailure&) {
    }
#endif
}

int main()
{
    try {
        std::optional<Snapshot> retained;
        {
            Session session;
            const auto before = session.snapshot();
            verify(before, false);
            const auto owner = session.Lifetime();
            std::optional<Snapshot> observed;
            auto observer = owner->observeRevocation([&] { observed = session.snapshot(); });
            const auto failed = session.Run(-1);
            require(failed.is_failure() && failed.error() == "rollback snapshot", "failure changed");
            require(observed.has_value(), "revocation did not observe a snapshot");
            verify(*observed, false);
            verify(session.snapshot(), false);

            (void)owner->replace();
            observed.reset();
            observer = owner->observeRevocation([&] { observed = session.snapshot(); });
            const auto completed = session.Run(0);
            require(completed.is_success() && completed.value() == 42, "successful mutation failed");
            require(observed.has_value(), "successful mutation did not observe prior state");
            verify(*observed, false);
            verify(before, false);
            retained = session.snapshot();
            verify(*retained, true);
        }
        verify(*retained, true);
        std::cout << "Public snapshots preserve committed scalar, record, list and set state across reentry and teardown.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
