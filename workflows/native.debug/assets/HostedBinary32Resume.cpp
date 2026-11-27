// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;

extern "C" const native::HostedModel proof_float_suspension;

namespace {

class SavedRounding {
public:
    SavedRounding() : mode_(std::fegetround()) {}
    ~SavedRounding() { std::fesetround(mode_); }

    void restore() const
    {
        if (std::fesetround(mode_) != 0)
            throw std::runtime_error("could not restore the floating-point rounding mode");
    }

private:
    int mode_;
};

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

native::HostedInstance bind()
{
    const auto result = native::HostedInstance::bind(proof_float_suspension, {});
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return result.value();
}

auto park(const native::HostedInstance& instance, model::TaskScope& scope)
{
    constexpr auto half = std::bit_cast<float>(UINT32_C(0x33800000));
    const std::array<native::HostedCallArgument, 1> input{native::HostedValue::fromFloat32(half)};
    const auto task = instance.startMethod(scope, "Run", input);
    require(!task.completed(), "the float32 task did not park on the semaphore");
    return task;
}

void checkPoll(bool activeHost)
{
    const SavedRounding initial;
    model::TaskScope scope;
    const auto instance = bind();
    const auto task = park(instance, scope);
    require(instance.startMethod(scope, "Release", {}).result().is_success(), "the source semaphore did not release");

    require(std::fesetround(FE_UPWARD) == 0, "could not enter upward rounding mode");
    bool refused = false;
    try {
        scope.poll();
    } catch (const model::ExecutionFailure& error) {
        refused = std::string_view(error.what()) == "float32 requires round-to-nearest floating mode";
    }
    require(std::fegetround() == FE_UPWARD, "the poll changed the caller's rounding mode");
    require(refused != activeHost && task.completed() == activeHost, "the poll did not honor the selected float32 profile");

    initial.restore();
    scope.poll();
    const auto result = task.result();
    require(result.is_success(), "the source task did not complete after restoring the mode");
    const auto expected = activeHost ? UINT32_C(0x3f800001) : UINT32_C(0x3f800000);
    require(std::bit_cast<std::uint32_t>(result.value().float32()) == expected, "float32 result used the wrong rounding mode");
}

void checkShutdown()
{
    const SavedRounding initial;
    model::TaskScope scope;
    const auto instance = bind();
    const auto task = park(instance, scope);

    require(std::fesetround(FE_UPWARD) == 0, "could not enter upward rounding mode for shutdown");
    scope.shutdown();
    require(task.completed() && task.result().is_cancelled(), "closing the scope did not drain the parked task");
    require(std::fegetround() == FE_UPWARD, "closing the scope changed the caller's rounding mode");
}

}

int main(int argc, char** argv)
{
    try {
        const bool activeHost = argc == 2 && std::string_view(argv[1]) == "--active-host";
        checkPoll(activeHost);
        checkShutdown();
        std::cout << "Hosted binary32 polling and shutdown contracts passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
