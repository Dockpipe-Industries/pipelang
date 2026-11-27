// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <bit>
#include <chrono>

using namespace pipelang::native_app_v19;
using pipelang::model::Result;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_floating_session;

class FloatPort final : public HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "NativeProof.FloatingValues.FloatPort"; }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        if (signature != "Echo(float32)" || arguments.size() != 1)
            return Result<HostedServiceValue>::Failure("unexpected floating service call");
        const auto* value = std::get_if<HostedValue>(&arguments.front());
        if (!value || value->kind() != HostedValue::Kind::Float32 ||
            std::bit_cast<std::uint32_t>(value->float32()) != 0x80000000u)
            return Result<HostedServiceValue>::Failure("numeric argument lost its type or sign bit");
        return Result<HostedServiceValue>::Success(HostedValue::fromFloat32(std::bit_cast<float>(0x7fc00017u)));
    }
};

int main()
{
    const auto port = std::make_shared<FloatPort>();
    std::array<HostedConstructorArgument, 2> arguments{HostedValue::fromFloat32(-0.0f), port};
    if (!HostedInstance::bind(proof_floating_session, arguments).is_failure())
        return 1;
    arguments[0] = HostedValue::fromFloat(-0.0);
    const auto instance = HostedInstance::bind(proof_floating_session, arguments);
    if (!instance.is_success())
        return 2;

    TaskScope scope;
    const auto task = instance.value().startValue(scope, {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return 3;
        scope.poll();
    }
    const auto result = task.result();
    if (!result.is_success())
        return 4;
    const auto wide = result.value().field("wide");
    const auto narrow = result.value().field("narrow");
    if (wide.kind() != HostedValue::Kind::Float || std::bit_cast<std::uint64_t>(wide.floating()) != 0x8000000000000000ULL)
        return 5;
    return narrow.kind() == HostedValue::Kind::Float32 && std::bit_cast<std::uint32_t>(narrow.float32()) == 0x7fc00017u
        ? 0 : 6;
}
