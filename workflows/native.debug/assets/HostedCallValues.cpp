// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::Outcome;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_hosted_call_values;

namespace {

HostedValue request(std::string type)
{
    const auto paths = HostedValue::fromList("List<string>", {
        HostedValue::fromText("source.pipe"), HostedValue::fromText("résultat.json")
    });
    return HostedValue::fromRecord(std::move(type), {
        {"label", HostedValue::fromText("qualification")}, {"paths", paths}
    });
}

HostedValueResult finish(TaskScope& scope, const pipelang::model::Task<HostedValueResult>& task)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("hosted request timed out");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

HostedValueResult run(const HostedInstance& model, TaskScope& scope,
                      const std::array<HostedCallArgument, 4>& arguments)
{
    return finish(scope, model.startWithValues(scope, arguments));
}

void expectFailure(const HostedValueResult& result, std::string_view expected)
{
    if (!result.is_failure() || result.error() != expected)
        throw std::runtime_error("wrong hosted argument error");
}

void expectCount(const HostedValueResult& result, std::int64_t expected = 2)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    if (result.value().integer() != expected)
        throw std::runtime_error("hosted request returned the wrong count");
}

HostedValue check(const HostedInstance& model, TaskScope& scope, std::int64_t scenario)
{
    const std::array<HostedCallArgument, 1> arguments{scenario};
    const auto result = model.startMethod(scope, "Check", arguments).result();
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return result.value();
}

}

int main()
{
    try {
        const auto model = HostedInstance::bind(proof_hosted_call_values, {});
        if (!model.is_success())
            throw std::runtime_error(model.error());
        auto arguments = std::array<HostedCallArgument, 4>{
            request("NativeProof.HostedCallValues.Request"), HostedValue::fromBoolean(true),
            std::int64_t{2}, pipelang::model::CancellationToken{}
        };

        TaskScope scope;
        expectCount(run(model.value(), scope, arguments));
        arguments[1] = HostedValue::fromBoolean(false);
        expectCount(run(model.value(), scope, arguments));

        arguments[0] = request("NativeProof.HostedCallValues.Wrong");
        expectFailure(run(model.value(), scope, arguments), "native hosted input does not match its checked nominal type");
        arguments[0] = request("NativeProof.HostedCallValues.Request");
        arguments[1] = std::int64_t{1};
        expectFailure(run(model.value(), scope, arguments), "native hosted invocation argument type does not match");
        arguments[1] = HostedValue::fromBoolean(false);
        expectCount(run(model.value(), scope, arguments));

        const auto pending = model.value().startWithValues(scope, arguments);
        const auto competing = model.value().startMethod(scope, "Inspect", {});
        expectFailure(competing.result(), "native hosted model already has an active invocation");
        expectCount(finish(scope, pending));
        expectCount(model.value().startMethod(scope, "Inspect", {}).result());
        expectFailure(model.value().startMethod(scope, "Missing", {}).result(), "native hosted method was not found");

        const auto failure = check(model.value(), scope, 1);
        const auto success = check(model.value(), scope, 0);
        const auto cancelled = check(model.value(), scope, 2);
        if (success.outcome() != Outcome::Success || success.value().integer() != 2 ||
            failure.outcome() != Outcome::Failure || failure.error().field("code").text() != "request rejected" ||
            cancelled.outcome() != Outcome::Cancelled || cancelled.error().field("code").text() != "request retired")
            throw std::runtime_error("typed hosted result lost its outcome or value");
        const std::array<HostedCallArgument, 1> thrown{std::int64_t{3}};
        expectFailure(model.value().startMethod(scope, "Check", thrown).result(), "request faulted");

        const auto cleared = model.value().startMethod(scope, "Clear", {}).result();
        if (!cleared.is_success())
            throw std::runtime_error(cleared.error());
        expectCount(model.value().startMethod(scope, "Inspect", {}).result(), 0);

        std::cout << "Native hosted methods shared receiver ownership and preserved typed inputs across suspension.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
