// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using pipelang::model::OperationCancelled;
using pipelang::model::Result;

#ifdef PIPELANG_REFERENCE
using PolicyInterface = reference::source::DiFixture::Contracts::ConstructionPolicy;
#else
using namespace pipelang::native_app_v19;
using PolicyInterface = HostedService;
extern "C" const HostedModel test_construction;
#endif

class Policy final : public PolicyInterface {
public:
    explicit Policy(std::string_view rejectedKind) : rejectedKind_(rejectedKind) {}

#ifdef PIPELANG_REFERENCE
    Result<std::int64_t> Read(std::string kind, std::int64_t context) const override
    {
        return read(kind, context);
    }
#else
    std::string_view interfaceName() const noexcept override
    {
        return "DiFixture.Contracts.ConstructionPolicy";
    }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        if (signature != "Read(string,int)" || arguments.size() != 2)
            throw std::runtime_error("unexpected constructor host call");
        const auto decision = read(std::get<std::string>(arguments[0]), std::get<std::int64_t>(arguments[1]));
        if (decision.is_failure())
            return Result<HostedServiceValue>::Failure(decision.error());
        return Result<HostedServiceValue>::Success(decision.value());
    }
#endif

    const std::vector<std::string>& calls() const { return calls_; }

private:
    Result<std::int64_t> read(std::string_view kind, std::int64_t context) const
    {
        if (context != 0)
            throw std::runtime_error("unexpected constructor policy context");
        calls_.emplace_back(kind);
        if (rejectedKind_ == "cancel" && kind == "root")
            throw OperationCancelled("fixture.cancelled");
        if (kind == rejectedKind_)
            return Result<std::int64_t>::Failure("fixture.rejeté");
        return Result<std::int64_t>::Success(0);
    }

    std::string_view rejectedKind_;
    mutable std::vector<std::string> calls_;
};

Result<std::int64_t> run(const std::shared_ptr<Policy>& policy, std::int64_t scenario)
{
#ifdef PIPELANG_REFERENCE
    try {
        reference::source::DiFixture::NativeConstruction::Witness witness(policy, scenario);
        return Result<std::int64_t>::Success(witness.Run());
    } catch (const OperationCancelled& cancellation) {
        return Result<std::int64_t>::Cancelled(cancellation.what());
    } catch (const pipelang::model::ExecutionFailure& failure) {
        return Result<std::int64_t>::Failure(failure.what());
    }
#else
    const std::array<HostedConstructorArgument, 2> arguments{policy, scenario};
    const auto witness = HostedInstance::bind(test_construction, arguments);
    if (witness.is_failure())
        return Result<std::int64_t>::Failure(witness.error());
    if (witness.is_cancelled())
        return Result<std::int64_t>::Cancelled(witness.error());

    pipelang::model::TaskScope scope;
    return witness.value().start(scope, {}).result();
#endif
}

void verify(std::string_view rejectedKind, std::int64_t scenario, const Result<std::int64_t>& expected,
            const std::vector<std::string>& calls)
{
    const auto policy = std::make_shared<Policy>(rejectedKind);
    const auto result = run(policy, scenario);
    if (result != expected || policy->calls() != calls)
        throw std::runtime_error("constructor did not preserve the expected result and policy call order");
}

int main()
{
    try {
        const std::vector<std::string> normalCalls{"root-dependency", "root"};
        verify("", 0, Result<std::int64_t>::Success(201), normalCalls);
        verify("", 1, Result<std::int64_t>::Success(4241), normalCalls);
        verify("root-dependency", 0, Result<std::int64_t>::Failure("fixture.rejeté"), {"root-dependency"});
        verify("root", 1, Result<std::int64_t>::Failure("fixture.rejeté"), normalCalls);
        verify("invalid-value", 2, Result<std::int64_t>::Failure("Result.Value requires Success"), {"invalid-value"});
        verify("cancel", 0, Result<std::int64_t>::Cancelled("fixture.cancelled"), normalCalls);
        verify("", 0, Result<std::int64_t>::Success(201), normalCalls);
        std::cout << "Hosted construction preserved dispatch, policy call order, failures and retries.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
