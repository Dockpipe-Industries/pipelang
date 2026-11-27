// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::model;
using namespace pipelang::native_app_v19;

extern "C" const HostedModel proof_hosted_session;
extern "C" const HostedModel proof_hosted_report;
extern "C" const HostedModel proof_hosted_clock;

namespace {

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

HostedInstance make(std::int64_t initial)
{
    const std::array arguments{initial};
    const auto created = HostedInstance::create(proof_hosted_session, arguments);
    if (!created.is_success())
        throw std::runtime_error(created.error());
    return created.value();
}

template<class T>
T finish(TaskScope& scope, const Task<T>& task)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        require(std::chrono::steady_clock::now() < deadline, "hosted invocation exceeded its proof deadline");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

void verifyReport(TaskScope& scope)
{
    HostedValue report;
    {
        const std::array constructor{std::int64_t{42}};
        const auto created = HostedInstance::create(proof_hosted_report, constructor);
        require(created.is_success(), "host could not construct the report model");
        const auto instance = created.value();
        std::array<HostedArgument, 1> arguments{std::int64_t{0}};
        require(finish(scope, instance.start(scope, arguments)).is_failure(), "integer API accepted a record result");
        report = finish(scope, instance.startValue(scope, arguments)).value();

        arguments[0] = std::int64_t{1};
        const auto failed = finish(scope, instance.startValue(scope, arguments));
        require(failed.is_failure() && failed.error() == "report failed", "record result lost its typed failure");
        arguments[0] = std::int64_t{2};
        const auto cancelled = finish(scope, instance.startValue(scope, arguments));
        require(cancelled.is_cancelled() && cancelled.error() == "report cancelled", "record result lost cancellation");
        arguments[0] = std::int64_t{3};
        const auto empty = finish(scope, instance.startValue(scope, arguments)).value();
        require(empty.field("success").boolean() && empty.field("cases").size() == 0, "empty report list has the wrong layout");
    }

    const auto rows = report.field("cases");
    require(report.typeName() == "NativeProof.HostedReport.HarnessReport" && rows.size() == 2,
            "report layout or list storage was lost after invocation");
    const auto tags = report.field("tags");
    require(tags.kind() == HostedValue::Kind::HashSet && tags.size() == 2 &&
                tags.at(0).text() == "étape" && tags.at(1).text() == "verified",
            "hosted HashSet lost its keys or insertion order after invocation");
    require(!report.field("success").boolean() && report.field("observed").text() == "étape échouée",
            "report lost its retained error text");
    const auto passed = rows.at(0);
    const auto failed = rows.at(1);
    require(passed.field("outcome").enumerator() == "Passed" && !passed.field("failure").hasValue(),
            "report did not preserve its enum or absent payload");
    const auto success = passed.field("verification");
    require(success.outcome() == Outcome::Success && success.value().kind() == HostedValue::Kind::Void,
            "nested void success was not preserved");
    const auto failure = failed.field("failure").value();
    const auto verification = failed.field("verification");
    require(failed.field("outcome").ordinal() == 1 && failure.field("code").integer() == 42 &&
                failure.field("message").text() == "étape échouée" && verification.outcome() == Outcome::Failure &&
                verification.error().field("code").integer() == 42,
            "report lost its retained typed record");

    bool refused = false;
    try {
        (void)rows.at(2);
    } catch (const ExecutionFailure&) {
        refused = true;
    }
    require(refused, "hosted list permitted an out-of-bounds read");
    std::cout << "Hosted PipeLang report values survived invocation and receiver destruction.\n";
}

class ClockHost final : public HostedService {
public:
    explicit ClockHost(std::string_view name) : name_(name), caller_(std::this_thread::get_id()) {}

    std::string_view interfaceName() const noexcept override { return name_; }

    Result<HostedServiceValue> invoke(std::string_view signature,
                                     std::span<const HostedServiceValue> arguments) override
    {
        if (signature == "NowMilliseconds()" && arguments.empty())
            return Result<HostedServiceValue>::Success(std::int64_t{40});
        if (signature == "IsReady()" && arguments.empty())
            return Result<HostedServiceValue>::Success(true);
        if (signature == "Acknowledge()" && arguments.empty())
            return Result<HostedServiceValue>::Success(HostedValue{});
        if (signature == "Observe(NativeProof.HostedClock.Request)" && arguments.size() == 1)
            return observe(std::get<HostedValue>(arguments.front()));
        if (signature == "WaitForObservation(NativeProof.HostedClock.Request,CancellationToken)" && arguments.size() == 2)
            return waitForObservation(std::get<HostedValue>(arguments[0]), std::get<CancellationToken>(arguments[1]));
        if (signature != "Read(string,int,bool)" || arguments.size() != 3 ||
            std::get<std::string>(arguments[0]) != "étape" || !std::get<bool>(arguments[2]))
            return Result<HostedServiceValue>::Failure("unexpected native ABI arguments");

        const auto scenario = std::get<std::int64_t>(arguments[1]);
        if (scenario == 1)
            return Result<HostedServiceValue>::Failure("clock refused");
        if (scenario == 2)
            return Result<HostedServiceValue>::Cancelled("clock cancelled");
        if (scenario == 3)
            return Result<HostedServiceValue>::Success(true);
        if (scenario == 4)
            throw std::runtime_error("callback raised");
        return Result<HostedServiceValue>::Success(std::string("retained text"));
    }

    const HostedValue& request() const { return request_; }
    bool waitingForCancellation() const { return waitingForCancellation_.load(); }

private:
    Result<HostedServiceValue> waitForObservation(HostedValue request, CancellationToken cancellation)
    {
        if (std::this_thread::get_id() == caller_)
            return Result<HostedServiceValue>::Failure("async observation ran on the caller thread");
        const auto scenario = request.field("scenario").integer();
        if (scenario == 8)
            return Result<HostedServiceValue>::Failure("async observation refused");
        if (scenario == 9)
            throw std::runtime_error("async callback raised");
        if (scenario == 10) {
            waitingForCancellation_.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!cancellation.is_cancelled()) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return Result<HostedServiceValue>::Failure("async cancellation did not reach the worker");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return Result<HostedServiceValue>::Cancelled("async observation cancelled");
        }
        return observe(std::move(request));
    }

    Result<HostedServiceValue> observe(HostedValue request)
    {
        const auto labels = request.field("labels");
        if (request.typeName() != "NativeProof.HostedClock.Request" || request.field("path").text() != "fixture.pipe" ||
            labels.size() != 1 || labels.at(0).text() != "étape")
            return Result<HostedServiceValue>::Failure("unexpected native observation request");

        const auto scenario = request.field("scenario").integer();
        request_ = std::move(request);
        if (scenario == 5)
            return Result<HostedServiceValue>::Failure("observation refused");
        if (scenario == 6)
            return Result<HostedServiceValue>::Cancelled("observation cancelled");

        const auto type = scenario == 7 ? "Other.Observation" : "NativeProof.HostedClock.Observation";
        return Result<HostedServiceValue>::Success(HostedValue::fromRecord(type, {
            {"exists", HostedValue::fromBoolean(true)},
            {"content", HostedValue::fromText("owned observation")},
            {"labels", labels},
            {"kind", HostedValue::fromEnum("NativeProof.HostedClock.ObservationKind", 1, "Found")}
        }));
    }

    std::string name_;
    std::thread::id caller_;
    std::atomic<bool> waitingForCancellation_ = false;
    HostedValue request_;
};

void verifyClock(TaskScope& scope)
{
    using Service = std::shared_ptr<HostedService>;
    constexpr std::string_view interfaceName = "NativeProof.HostedClock.Clock";
    const std::array<HostedConstructorArgument, 2> wrong{Service{std::make_shared<ClockHost>("Other.Clock")}, std::int64_t{2}};
    require(HostedInstance::bind(proof_hosted_clock, wrong).is_failure(), "host constructor accepted the wrong service");

    const auto service = std::make_shared<ClockHost>(interfaceName);
    const std::array<HostedConstructorArgument, 2> constructor{Service{service}, std::int64_t{2}};
    const auto created = HostedInstance::bind(proof_hosted_clock, constructor);
    require(created.is_success(), "host did not bind the clock service");
    const auto& instance = created.value();
    std::array<HostedArgument, 2> arguments{std::int64_t{0}, CancellationToken{}};
    require(finish(scope, instance.start(scope, arguments)).value() == 42, "source did not call the clock after suspension");
    require(service->request().field("labels").at(0).text() == "étape",
            "the host lost the retained request when the native invocation ended");

    arguments[0] = std::int64_t{1};
    const auto refused = finish(scope, instance.start(scope, arguments));
    require(refused.is_failure() && refused.error() == "clock refused", "native host call lost an explicit failure");
    arguments[0] = std::int64_t{2};
    const auto cancelled = finish(scope, instance.start(scope, arguments));
    require(cancelled.is_cancelled() && cancelled.error() == "clock cancelled", "native host call lost explicit cancellation");
    arguments[0] = std::int64_t{3};
    const auto mismatch = finish(scope, instance.start(scope, arguments));
    require(mismatch.is_failure() && mismatch.error() == "native host service result does not match the checked return type",
            "native host call accepted a result of the wrong type");
    arguments[0] = std::int64_t{4};
    require(finish(scope, instance.start(scope, arguments)).value() == 42,
            "native host callback exception did not enter the PipeLang catch block");

    arguments[0] = std::int64_t{5};
    const auto observationFailed = finish(scope, instance.start(scope, arguments));
    require(observationFailed.is_failure() && observationFailed.error() == "observation refused",
            "the structured host call lost an explicit failure");
    arguments[0] = std::int64_t{6};
    const auto observationCancelled = finish(scope, instance.start(scope, arguments));
    require(observationCancelled.is_cancelled() && observationCancelled.error() == "observation cancelled",
            "the structured host call lost explicit cancellation");
    arguments[0] = std::int64_t{7};
    const auto wrongRecord = finish(scope, instance.start(scope, arguments));
    require(wrongRecord.is_failure() &&
                wrongRecord.error() == "native hosted input does not match its checked nominal type",
            "the structured host call accepted a record with the wrong nominal type");

    arguments[0] = std::int64_t{8};
    const auto asyncFailed = finish(scope, instance.start(scope, arguments));
    require(asyncFailed.is_failure() && asyncFailed.error() == "async observation refused",
            "the async host call lost an explicit failure");
    arguments[0] = std::int64_t{9};
    const auto asyncThrown = finish(scope, instance.start(scope, arguments));
    require(asyncThrown.is_failure() && asyncThrown.error() == "async callback raised",
            "the async host worker did not convert its exception to a typed failure");
    const CancellationSource cancellation;
    arguments[0] = std::int64_t{10};
    arguments[1] = cancellation.token();
    const auto waiting = instance.start(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!service->waitingForCancellation() && !waiting.completed()) {
        require(std::chrono::steady_clock::now() < deadline, "the async host worker did not start");
        scope.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!waiting.completed(), "the caller did not suspend for the async host worker");
    cancellation.cancel();
    const auto asyncCancelled = finish(scope, waiting);
    require(asyncCancelled.is_cancelled() && asyncCancelled.error() == "async observation cancelled",
            "cancellation did not reach the running host call");

    arguments[0] = std::int64_t{0};
    arguments[1] = CancellationToken{};
    std::weak_ptr<HostedService> retained;
    const auto running = [&] {
        const auto temporary = std::make_shared<ClockHost>(interfaceName);
        retained = temporary;
        const std::array<HostedConstructorArgument, 2> bindings{Service{temporary}, std::int64_t{2}};
        const auto owner = HostedInstance::bind(proof_hosted_clock, bindings);
        return owner.value().start(scope, arguments);
    }();
    require(!running.completed() && !retained.expired(), "a suspended task did not retain its service");
    require(finish(scope, running).value() == 42, "a retained service could not be called after its owner left scope");
    std::cout << "Hosted PipeLang service arguments, outcomes and lifetime passed.\n";
}

}

int main()
{
    try {
        require(HostedInstance::create(proof_hosted_session, {}).is_failure(), "constructor accepted wrong arity");
        TaskScope scope;
        const CancellationSource body;
        const CancellationSource cleanup;
        const auto session = make(40);
        auto copy = session;
        std::array<HostedArgument, 3> arguments{std::int64_t{0}, body.token(), cleanup.token()};

        const auto started = session.start(scope, arguments);
        require(!started.completed(), "hosted model did not suspend");
        arguments[0] = std::int64_t{1};
        const auto collision = finish(scope, copy.start(scope, arguments));
        require(collision.is_failure() && collision.error() == "native hosted model already has an active invocation",
                "a second invocation entered the retained receiver");
        require(finish(scope, started).value() == 42, "invocation lost constructed state or captured arguments");

        const auto thrown = finish(scope, session.start(scope, arguments));
        require(thrown.is_failure() && thrown.error() == "receiver rollback", "hosted exception did not return its error");
        arguments[0] = std::int64_t{0};
        require(finish(scope, session.start(scope, arguments)).value() == 45, "fault rolled back the wrong receiver segment");

        arguments[0] = std::int64_t{2};
        const auto returned = finish(scope, session.start(scope, arguments));
        require(returned.is_failure() && returned.error() == "returned failure", "hosted returned failure changed its outcome");
        arguments[0] = std::int64_t{0};
        require(finish(scope, session.start(scope, arguments)).value() == 49, "returned failure lost the committed receiver");

        body.cancel();
        const auto cancelled = finish(scope, session.start(scope, arguments));
        require(cancelled.is_cancelled() && cancelled.error() == "body token already cancelled", "explicit body token was lost");
        arguments[1] = std::int64_t{0};
        require(finish(scope, session.start(scope, arguments)).is_failure(), "hosted invocation accepted the wrong argument type");
        arguments[1] = CancellationToken{};
        cleanup.cancel();
        const auto refused = finish(scope, session.start(scope, arguments));
        require(refused.is_failure() && refused.error() == "cleanup token already cancelled", "cleanup token was merged with task cancellation");
        arguments[2] = CancellationToken{};
        const auto fromTemporary = make(80).start(scope, arguments);
        require(finish(scope, fromTemporary).value() == 82, "task did not retain a temporary receiver");

        const CancellationSource caller;
        caller.cancel();
        require(finish(scope, session.start(scope, arguments, caller.token())).is_cancelled(), "caller cancellation was ignored");
        require(finish(scope, session.start(scope, arguments)).value() == 51, "cancelled admission changed the receiver");
        verifyReport(scope);
        verifyClock(scope);
        std::cout << "Hosted PipeLang receiver, task lifetime, cancellation and segment rollback passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
