// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::CancellationToken;
using pipelang::model::Result;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_sequences;

namespace {

class Port final : public HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "NativeProof.Sequences.TransferPort"; }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        using Outcome = Result<HostedServiceValue>;
        if (signature != "Send(NativeProof.Sequences.Transfer,CancellationToken)" || arguments.size() != 2)
            return Outcome::Failure("unexpected sequence service call");
        const auto* request = std::get_if<HostedValue>(&arguments.front());
        if (!request)
            return Outcome::Failure("sequence request was not owned");
        const auto queue = request->field("queue");
        const auto stack = request->field("stack");
        if (queue.kind() != HostedValue::Kind::Queue || queue.size() != 2 ||
            queue.at(0).integer() != 42 || queue.at(1).integer() != 43 ||
            stack.kind() != HostedValue::Kind::Stack || stack.size() != 2 ||
            stack.at(0).text() != "top" || stack.at(1).text() != "bottom")
            return Outcome::Failure("host request lost sequence order");

        return Outcome::Success(HostedValue::fromRecord("NativeProof.Sequences.Transfer", {
            {"queue", HostedValue::fromQueue("Queue<int>", {HostedValue::fromInteger(17), HostedValue::fromInteger(19)})},
            {"stack", HostedValue::fromStack("Stack<string>", {HostedValue::fromText("new top"),
                                                               HostedValue::fromText("new bottom")})}
        }));
    }
};

}

int main()
{
    HostedValue detached;
    {
        const std::array<HostedConstructorArgument, 1> services{std::make_shared<Port>()};
        const auto hosted = HostedInstance::bind(proof_sequences, services);
        if (!hosted.is_success())
            return 1;
        TaskScope scope;
        const std::array<HostedArgument, 2> arguments{std::int64_t{0}, CancellationToken{}};
        const auto task = hosted.value().startValue(scope, arguments);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.completed()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return 2;
            scope.poll();
            if (!task.completed())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto result = task.result();
        if (!result.is_success()) {
            std::cerr << result.error() << '\n';
            return 3;
        }
        detached = result.value();
    }
    const auto queue = detached.field("queue");
    const auto stack = detached.field("stack");
    if (queue.at(1).integer() != 19 || stack.at(1).text() != "new bottom")
        return 4;
    std::cout << "Hosted queue and stack preserved nominal types and order across suspension.\n";
}
