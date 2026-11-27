// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::Result;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_array_host_values;

namespace {

class Port final : public HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "NativeProof.ArrayHostValues.Port"; }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        using Outcome = Result<HostedServiceValue>;
        if (arguments.size() != 1)
            return Outcome::Failure("unexpected host argument count");
        if (signature == "Read(int)") {
            const auto mode = std::get<std::int64_t>(arguments.front());
            std::vector<HostedValue> items;
            items.reserve(257);
            for (int index = 0; index < 257; ++index)
                items.push_back(HostedValue::fromText("entry-" + std::to_string(index)));
            return Outcome::Success(HostedValue::fromList(mode == 0 ? "Array<string>" : "List<string>",
                                                          std::move(items)));
        }
        if (signature == "Check(Array<string>)") {
            const auto& values = std::get<HostedValue>(arguments.front());
            if (values.kind() == HostedValue::Kind::List && values.typeName() == "Array<string>" &&
                values.size() == 257 && values.at(127).text() == "entry-127" &&
                values.at(128).text() == "changed" && values.at(129).text() == "entry-129")
                return Outcome::Success(HostedValue{});
        }
        return Outcome::Failure("unexpected host array");
    }
};

HostedValueResult run(const HostedInstance& instance, TaskScope& scope, std::int64_t mode)
{
    const std::array<HostedArgument, 1> arguments{mode};
    const auto task = instance.startValue(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("host array test timed out");
        scope.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

}

int main()
{
    try {
        const std::array<HostedConstructorArgument, 1> bindings{std::make_shared<Port>()};
        const auto instance = HostedInstance::bind(proof_array_host_values, bindings);
        if (!instance.is_success())
            throw std::runtime_error(instance.error());
        TaskScope scope;
        const auto result = run(instance.value(), scope, 0);
        if (!result.is_success())
            throw std::runtime_error(result.error());
        const auto values = result.value();
        if (values.size() != 257 || values.at(128).text() != "changed")
            throw std::runtime_error("native export lost the updated array");
        const auto badType = run(instance.value(), scope, 1);
        if (!badType.is_failure() || badType.error() != "native hosted input does not match its checked nominal type")
            throw std::runtime_error("native import accepted the wrong nominal array type");
        std::cout << "The native service bridge preserved array copies and checked the nominal type.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
