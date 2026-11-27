// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;
extern "C" const native::HostedModel proof_byte_iteration;

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
}

int main()
{
    try {
        const auto binding = native::HostedInstance::bind(proof_byte_iteration, {});
        require(binding.is_success(), binding.is_success() ? "" : binding.error());
        const auto& instance = binding.value();
        model::TaskScope scope;
        const auto invoke = [&](std::string_view name, std::span<const native::HostedCallArgument> arguments = {}) {
            const auto task = instance.startMethod(scope, name, arguments);
            require(task.completed(), "byte iteration suspended");
            return task.result();
        };
        const auto singleArgument = [](std::span<const std::uint8_t> bytes) {
            return std::array<native::HostedCallArgument, 1>{native::HostedValue::fromBytes(bytes)};
        };

        const auto empty = singleArgument({});
        require(checked(invoke("Sum", empty)).integer() == 0, "empty sum changed");
        require(checked(invoke("Typed", empty)).integer() == 0, "empty overload result changed");

        std::array<std::uint8_t, 256> allBytes{};
        for (std::uint32_t value = 0; value < allBytes.size(); ++value) {
            allBytes[value] = static_cast<std::uint8_t>(value);
            const auto input = singleArgument(std::span(allBytes).subspan(value, 1));
            require(checked(invoke("Sum", input)).integer() == value, "byte lost its unsigned value");
            require(checked(invoke("Typed", input)).integer() == value + 1, "byte selected the wrong overload");
            require(checked(invoke("Complement", input)).uint32() == ~value, "byte complement changed");
        }
        require(checked(invoke("Sum", singleArgument(allBytes))).integer() == 32640, "full byte sum changed");

        constexpr std::array<std::uint8_t, 5> bytes{0, 1, 128, 255, 9};
        const auto input = singleArgument(bytes);
        require(checked(invoke("Control", input)).integer() == 129, "iteration control changed");
        require(checked(invoke("Snapshot", input)).integer() == 393, "iteration observed newly appended bytes");
        require(std::ranges::equal(std::get<native::HostedValue>(input[0]).bytes(), bytes),
                "receiver mutation changed the host argument");
        const auto stored = checked(instance.snapshot()).field("stored");
        constexpr std::array<std::uint8_t, 10> expected{0, 1, 128, 255, 9, 9, 9, 9, 9, 9};
        require(std::ranges::equal(stored.bytes(), expected), "receiver bytes changed");

        require(checked(invoke("ReceiverOnce")).integer() == 438, "single receiver iteration result changed");
        require(checked(instance.snapshot()).field("reads").integer() == 1, "iteration evaluated its receiver twice");
        constexpr std::array<std::uint8_t, 2> first{1, 2};
        constexpr std::array<std::uint8_t, 2> second{3, 4};
        const std::array<native::HostedCallArgument, 2> paired{
            native::HostedValue::fromBytes(first), native::HostedValue::fromBytes(second)};
        require(checked(invoke("Nested", paired)).integer() == 20, "nested iteration changed");

        const auto oversized = singleArgument(std::vector<std::uint8_t>(65536, 1));
        const auto exhausted = invoke("Sum", oversized);
        require(exhausted.is_failure() && exhausted.error().find("evaluation") != std::string::npos,
                "large iteration failed to report evaluation exhaustion");
        require(std::ranges::equal(checked(instance.snapshot()).field("stored").bytes(), stored.bytes()),
                "evaluation failure changed published bytes");

        std::cout << "All 256 byte values and byte iteration ownership checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
