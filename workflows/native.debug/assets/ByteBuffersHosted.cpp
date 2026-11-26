// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::CancellationToken;
using pipelang::model::Result;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_byte_session;

namespace {

constexpr std::array<std::uint8_t, 3> initialBytes{0, 255, 128};
constexpr std::array<std::uint8_t, 4> returnedBytes{128, 0, 255, 254};

class BytePort final : public HostedService {
public:
    BytePort() : caller_(std::this_thread::get_id()) {}

    std::string_view interfaceName() const noexcept override { return "NativeProof.ByteBuffers.BytePort"; }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        using Outcome = Result<HostedServiceValue>;
        if (signature != "Echo(NativeProof.ByteBuffers.Packet,CancellationToken)" || arguments.size() != 2)
            return Outcome::Failure("unexpected byte service call");
        if (std::this_thread::get_id() == caller_)
            return Outcome::Failure("awaited bytes did not cross a host worker");

        const auto* input = std::get_if<HostedValue>(&arguments.front());
        if (!input)
            return Outcome::Failure("byte input lost its record type");
        const auto body = input->field("body");
        if (body.kind() != HostedValue::Kind::ByteBuffer ||
            (!std::ranges::equal(body.bytes(), initialBytes) && !std::ranges::equal(body.bytes(), returnedBytes)))
            return Outcome::Failure("byte input changed");
        return Outcome::Success(HostedValue::fromRecord("NativeProof.ByteBuffers.Packet",
                                                       {{"body", HostedValue::fromBytes(returnedBytes)}}));
    }

private:
    std::thread::id caller_;
};

HostedValueResult run(const HostedInstance& instance, TaskScope& scope, std::int64_t scenario)
{
    const std::array<HostedArgument, 2> arguments{scenario, CancellationToken{}};
    const auto task = instance.startValue(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("hosted byte invocation timed out");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    HostedValue detached;
    {
        const auto port = std::make_shared<BytePort>();
        std::array<HostedConstructorArgument, 2> arguments{HostedValue::fromText("text"), port};
        if (!HostedInstance::bind(proof_byte_session, arguments).is_failure())
            return 1;
        arguments[0] = HostedValue::fromBytes(initialBytes);
        const auto instance = HostedInstance::bind(proof_byte_session, arguments);
        if (!instance.is_success())
            return 2;
        TaskScope scope;
        const auto invalid = run(instance.value(), scope, 1);
        if (!invalid.is_failure() || invalid.error() != "byte buffer value must be between 0 and 255")
            return 3;
        const auto result = run(instance.value(), scope, 0);
        if (!result.is_success() || !run(instance.value(), scope, 0).is_success())
            return 4;
        detached = result.value().field("body");
    }
    return detached.kind() == HostedValue::Kind::ByteBuffer && std::ranges::equal(detached.bytes(), returnedBytes)
        ? 0 : 5;
}
