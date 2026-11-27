// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;

extern "C" const native::HostedModel proof_member_bindings;

namespace {
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

native::HostedValue checked(const native::HostedValueResult& result)
{
    require(result.is_success(), result.is_success() ? "" : result.error());
    return result.value();
}

class BindingHost final : public native::HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "BindingHost"; }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Result = model::Result<native::HostedServiceValue>;
        if (signature == "Read()" && arguments.empty())
            return Result::Success(std::int64_t{37});
        if (signature == "Step(int)" && arguments.size() == 1)
            return Result::Success(std::get<std::int64_t>(arguments.front()) + 2);
        return Result::Failure("unexpected binding host call: " + std::string(signature));
    }
};
}

int main()
{
    try {
        const std::array<native::HostedConstructorArgument, 3> arguments{
            std::make_shared<BindingHost>(), std::int64_t{91}, std::int64_t{41}};
        auto binding = native::HostedInstance::bind(proof_member_bindings, arguments);
        require(binding.is_success(), binding.is_success() ? "" : binding.error());
        auto instance = std::move(binding).value();
        model::TaskScope scope;
        const auto call = [&](std::string_view name, std::span<const native::HostedCallArgument> values = {}) {
            const auto task = instance.startMethod(scope, name, values);
            require(task.completed(), "synchronous binding call suspended");
            return checked(task.result());
        };
        const auto integer = [&](std::string_view name, std::int64_t argument, std::int64_t expected) {
            const std::array<native::HostedCallArgument, 1> values{argument};
            require(call(name, values).integer() == expected, name);
        };

        const auto initial = checked(instance.snapshot());
        require(initial.field("value").integer() == 91, "explicit root field write lost the constructor parameter");
        require(initial.field("initialized").integer() == 11, "root initializer captured a shadowing parameter");
        require(initial.field("fromArgument").integer() == 41, "root initializer lost its constructor input");
        require(call("ItemInitial").integer() == 7, "child initializer captured a shadowing parameter");
        require(call("ItemInput").integer() == 42, "child initializer lost its constructor input");
        require(call("InheritedInitial").integer() == 92, "derived initializer lost the initialized base field");
        integer("ItemParameter", 3, 94);
        integer("InheritedParameter", 3, 95);
        integer("Parameter", 3, 94);
        integer("ConstantParameter", 19, 38);
        const std::array<native::HostedCallArgument, 1> point{
            native::HostedValue::fromRecord("Point", {{"count", native::HostedValue::fromInteger(23)}})};
        require(call("NamedTypeParameter", point).integer() == 23, "type name captured a lexical parameter");
        require(call("ReadHost").integer() == 37, "host field was not selected explicitly");
        const std::array<native::HostedCallArgument, 1> update{std::int64_t{55}};
        call("Set", update);
        const auto changed = checked(instance.snapshot());
        require(changed.field("value").integer() == 55 && changed.field("point").field("count").integer() == 55,
                "explicit field or record assignment mutated the wrong state");
        require(call("First").integer() == 55, "explicit list mutation was lost");
        integer("ExplicitCall", 8, 9);
        integer("GenericCall", 17, 17);
        const std::array<native::HostedCallArgument, 1> inputs{native::HostedValue::fromList("List<int>", {
            native::HostedValue::fromInteger(2), native::HostedValue::fromInteger(3)})};
        require(call("Workers", inputs).integer() == 10, "parallel helper captured an instance field");
        const std::array<native::HostedCallArgument, 1> forward{std::int64_t{17}};
        const auto task = instance.startMethod(scope, "Forward", forward);
        require(!task.completed(), "host call did not suspend the member invocation");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.completed()) {
            scope.poll();
            require(std::chrono::steady_clock::now() < deadline, "host member call did not resume");
            std::this_thread::yield();
        }
        require(checked(task.result()).integer() == 19, "async member call changed its input or result");
        require(checked(instance.snapshot()).field("value").integer() == 55,
                "async parameter wrote an instance field");
        std::cout << "Native member binding checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
