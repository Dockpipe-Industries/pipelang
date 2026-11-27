// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::model;
using namespace reference::source::NativeProof::HostedClock;

namespace {

class ClockAdapter final : public Clock {
public:
    Result<std::int64_t> NowMilliseconds() override { return Result<std::int64_t>::Success(40); }
    bool IsReady() const override { return true; }

    Result<std::string> Read(std::string label, std::int64_t step, bool enabled) override
    {
        if (label != "étape" || !enabled)
            return Result<std::string>::Failure("unexpected reference arguments");
        if (step == 1)
            return Result<std::string>::Failure("clock refused");
        if (step == 2)
            return Result<std::string>::Cancelled("clock cancelled");
        if (step == 4)
            throw std::runtime_error("callback raised");
        return Result<std::string>::Success("retained text");
    }

    Result<Observation> Observe(Request request) override
    {
        if (request.path != "fixture.pipe" || request.labels.count() != 1 || request.labels.at(0) != "étape")
            return Result<Observation>::Failure("unexpected reference observation request");
        if (request.scenario == 5)
            return Result<Observation>::Failure("observation refused");
        if (request.scenario == 6)
            return Result<Observation>::Cancelled("observation cancelled");
        return Result<Observation>::Success(Observation{true, "owned observation", std::move(request.labels),
                                                        ObservationKind::Found});
    }

    Result<void> Acknowledge() override { return Result<void>::Success(); }

    Result<Observation> WaitForObservation(Request request, CancellationToken token) override
    {
        if (request.scenario == 8)
            return Result<Observation>::Failure("async observation refused");
        if (request.scenario == 9)
            throw std::runtime_error("async callback raised");
        if (token.is_cancelled())
            return Result<Observation>::Cancelled("async observation cancelled");
        return Observe(std::move(request));
    }
};

Result<std::int64_t> run(Session& session, std::int64_t scenario)
{
    const auto task = session.Run(scenario, CancellationToken{});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference host call exceeded its proof deadline");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    try {
        Session session(std::make_shared<ClockAdapter>(), 2);
        if (run(session, 0).value() != 42 || run(session, 4).value() != 42)
            return 1;
        const auto failed = run(session, 1);
        if (!failed.is_failure() || failed.error() != "clock refused")
            return 1;
        const auto cancelled = run(session, 2);
        if (!cancelled.is_cancelled() || cancelled.error() != "clock cancelled")
            return 1;
        const auto observationFailed = run(session, 5);
        if (!observationFailed.is_failure() || observationFailed.error() != "observation refused")
            return 1;
        const auto observationCancelled = run(session, 6);
        if (!observationCancelled.is_cancelled() || observationCancelled.error() != "observation cancelled")
            return 1;
        const auto asyncFailed = run(session, 8);
        if (!asyncFailed.is_failure() || asyncFailed.error() != "async observation refused")
            return 1;
        const auto asyncThrown = run(session, 9);
        if (!asyncThrown.is_failure() || asyncThrown.error() != "async callback raised")
            return 1;
        std::cout << "The C++ reference agreed on hosted clock success, failure, cancellation and exception recovery.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
