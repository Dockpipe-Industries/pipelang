// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;
using native::HostedValue;
using model::Outcome;

extern "C" const native::HostedModel proof_typed_propagation;

namespace {
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

HostedValue checked(const native::HostedValueResult& result, std::string_view name)
{
    if (!result.is_success())
        throw std::runtime_error(std::string(name) + ": " + result.error());
    return result.value();
}

void domainError(const HostedValue& result, bool cancelled, std::string_view origin, std::int64_t request)
{
    require(result.outcome() == (cancelled ? Outcome::Cancelled : Outcome::Failure), "typed outcome changed");
    const auto error = result.error();
    require(error.field("code").enumerator() == (cancelled ? "Missing" : "Conflict"), "nominal enum changed");
    require(error.field("origin").text() == origin && error.field("request").integer() == request,
            "typed error record changed");
}

template<class... Arguments>
std::array<native::HostedCallArgument, sizeof...(Arguments)> arguments(Arguments... values)
{
    return {native::HostedCallArgument{values}...};
}
}

int main()
{
    try {
        const auto binding = native::HostedInstance::bind(proof_typed_propagation, {});
        require(binding.is_success(), binding.is_success() ? "" : binding.error());
        const auto& instance = binding.value();
        model::TaskScope scope;
        const auto invoke = [&](std::string_view name, std::span<const native::HostedCallArgument> values = {}) {
            const auto task = instance.startMethod(scope, name, values);
            require(task.completed(), "synchronous propagation suspended");
            return task.result();
        };
        const auto call = [&](std::string_view name, std::span<const native::HostedCallArgument> values = {}) {
            return checked(invoke(name, values), name);
        };
        const auto trace = [&] { return call("Trace").integer(); };
        const auto cleanup = [&] { return call("CleanupCount").integer(); };
        const auto caught = [&] { return call("CaughtCount").integer(); };
        constexpr auto zero = std::int64_t{0};
        constexpr auto one = std::int64_t{1};
        constexpr auto three = std::int64_t{3};
        constexpr auto never = std::int64_t{-1};

        for (std::int64_t mode = 0; mode < 3; ++mode) {
            const auto check = [&](const HostedValue& result, std::int64_t expected, std::int64_t request) {
                if (mode == 0)
                    require(result.value().integer() == expected, "successful result changed");
                else
                    domainError(result, mode == 2, "reader", request);
            };
            check(call("Arguments", arguments(mode)), 123, 2);
            require(trace() == (mode == 0 ? 123 : 12), "argument evaluation did not stop on propagation");
            check(call("Binary", arguments(mode)), 7, 2);
            require(trace() == (mode == 0 ? 123 : 12), "binary evaluation did not stop on propagation");
            require(!call("And", arguments(HostedValue::fromBoolean(false), mode)).value().boolean(),
                    "short-circuited conjunction changed");
            require(trace() == 0, "short-circuited conjunction evaluated its right side");
            require(call("Or", arguments(HostedValue::fromBoolean(true), mode)).value().boolean(),
                    "short-circuited disjunction changed");
            require(trace() == 0, "short-circuited disjunction evaluated its right side");
            check(call("Choice", arguments(HostedValue::fromBoolean(true), mode, three)), 1, 1);
            require(trace() == 1, "conditional evaluated the wrong arm");
            check(call("Choice", arguments(HostedValue::fromBoolean(false), three, mode)), 2, 2);
            require(trace() == 2, "conditional evaluated the wrong arm");
            require(!call("NullableChoice", arguments(HostedValue::fromBoolean(false), mode)).value().hasValue(),
                    "conditional optional result changed");

            const auto beforeCleanup = cleanup();
            const auto beforeCaught = caught();
            check(call("WithCleanup", arguments(mode)), 1, 1);
            require(cleanup() == beforeCleanup + 1 && caught() == beforeCaught,
                    "typed propagation did not run only the finally clause");
            check(call("Collection", arguments(mode)), 2, 2);
            check(call("Assign", arguments(mode)), 2, 2);
            check(call("ForInitializer", arguments(mode)), 3, 1);
        }

        require(call("While", arguments(never)).value().integer() == 3 && trace() == 4,
                "normal while condition changed");
        domainError(call("While", arguments(one)), false, "condition", 1);
        require(trace() == 2, "while continued after condition propagation");
        const auto beforeFor = cleanup();
        require(call("For", arguments(never, never)).value().integer() == 1 && trace() == 3,
                "normal for control changed");
        require(cleanup() == beforeFor + 3, "for loop lost cleanup across transfers");
        domainError(call("For", arguments(never, zero)), true, "iteration", 0);
        require(trace() == 1, "for continued after cancelled iterator");
        domainError(call("For", arguments(one, never)), false, "condition", 1);
        require(trace() == 2, "for continued after condition failure");
        domainError(call("ForNoContinue", arguments(one)), true, "iteration", 1);
        require(call("Stored", arguments(zero)).value().integer() == 2, "stored success changed");
        domainError(call("Stored", arguments(one)), false, "reader", 2);

        const auto failure = HostedValue::fromRecord("FailureData", {
            {"code", HostedValue::fromEnum("FailureCode", 1, "Conflict")},
            {"origin", HostedValue::fromText("caller")},
            {"request", HostedValue::fromInteger(77)}});
        const auto input = HostedValue::fromResult("Result<int,FailureData>", Outcome::Failure, failure);
        domainError(call("CrossPayload", arguments(input)), false, "caller", 77);
        const auto inner = HostedValue::fromResult(
            "Result<int,FailureData>", Outcome::Success, HostedValue::fromInteger(20));
        constexpr auto nestedType = "Result<Result<int,FailureData>,FailureData>";
        const auto nested = HostedValue::fromResult(nestedType, Outcome::Success, inner);
        require(call("Nested", arguments(nested)).value().integer() == 20, "nested result changed");
        const auto failedNested = HostedValue::fromResult(nestedType, Outcome::Success, input);
        domainError(call("Nested", arguments(failedNested)), false, "caller", 77);
        const auto voidResult = HostedValue::fromResult("Result<void,FailureData>", Outcome::Failure, failure);
        domainError(call("Void", arguments(voidResult)), false, "caller", 77);

        const auto beforeFault = trace();
        const auto exhausted = invoke("Exhaustion");
        require(exhausted.is_failure() && exhausted.error() == "source evaluation budget exceeded",
                "budget exhaustion changed");
        require(trace() == beforeFault, "budget failure published receiver writes");
        const auto fault = invoke("Arguments", arguments(three));
        require(fault.is_failure() && fault.error() == "reader execution fault", "execution fault changed");
        require(trace() == beforeFault, "execution fault published receiver writes");
        require(call("WithCleanup", arguments(three)).value().integer() == -1 && caught() == 1,
                "catch failed to recover from execution fault");

        std::cout << "Typed result propagation preserved tags, effect order, cleanup and rollback.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
