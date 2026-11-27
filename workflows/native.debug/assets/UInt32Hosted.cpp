// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>

using namespace pipelang::native_app_v19;
using pipelang::model::Result;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_uint32_session;

class WordPort final : public HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "NativeProof.UInt32Values.WordPort"; }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        if (signature != "Echo(uint32)" || arguments.size() != 1)
            return Result<HostedServiceValue>::Failure("unexpected word service call");
        const auto* word = std::get_if<HostedValue>(&arguments.front());
        if (!word || word->kind() != HostedValue::Kind::UInt32 ||
            word->uint32() != std::numeric_limits<std::uint32_t>::max())
            return Result<HostedServiceValue>::Failure("native word lost its type or value");
        return Result<HostedServiceValue>::Success(HostedValue::fromUInt32(0x80000000u));
    }
};

HostedValueResult run(const HostedInstance& instance, TaskScope& scope, std::int64_t scenario)
{
    const std::array<HostedArgument, 1> arguments{scenario};
    const auto task = instance.startValue(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("hosted uint32 invocation timed out");
        scope.poll();
    }
    return task.result();
}

int main()
{
    const auto port = std::make_shared<WordPort>();
    std::array<HostedConstructorArgument, 2> arguments{HostedValue::fromInteger(1), port};
    if (!HostedInstance::bind(proof_uint32_session, arguments).is_failure())
        return 1;
    arguments[0] = HostedValue::fromUInt32(std::numeric_limits<std::uint32_t>::max());
    const auto instance = HostedInstance::bind(proof_uint32_session, arguments);
    if (!instance.is_success())
        return 2;

    TaskScope scope;
    const auto failure = run(instance.value(), scope, 1);
    if (!failure.is_failure() || failure.error() != "uint32 addition overflow")
        return 3;

    const auto result = run(instance.value(), scope, 0);
    if (!result.is_success())
        return 4;
    const auto word = result.value().field("value");
    return word.kind() == HostedValue::Kind::UInt32 && word.uint32() == 0x80000000u ? 0 : 5;
}
