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
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using pipelang::model::ExecutionFailure;
using pipelang::model::OperationCancelled;
using pipelang::model::Result;

#ifdef PIPELANG_REFERENCE
using ProbeInterface = reference::source::NativeProof::HostedConstructionCleanup::Probe;
#else
using namespace pipelang::native_app_v19;
using ProbeInterface = HostedService;
extern "C" const HostedModel test_construction_cleanup;
#endif

class Probe final : public ProbeInterface {
public:
#ifdef PIPELANG_REFERENCE
    std::int64_t Mark(std::int64_t event) const override { return mark(event); }
    Result<std::int64_t> Check() const override { return Result<std::int64_t>::Failure("fixture.denied"); }
#else
    std::string_view interfaceName() const noexcept override
    {
        return "NativeProof.HostedConstructionCleanup.Probe";
    }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        if (signature == "Mark(int)" && arguments.size() == 1)
            return Result<HostedServiceValue>::Success(mark(std::get<std::int64_t>(arguments[0])));
        if (signature == "Check()" && arguments.empty())
            return Result<HostedServiceValue>::Failure("fixture.denied");
        throw std::runtime_error("unexpected constructor cleanup host call");
    }
#endif

    const std::vector<std::int64_t>& events() const { return events_; }

private:
    std::int64_t mark(std::int64_t event) const
    {
        events_.push_back(event);
        if (event == 3)
            throw OperationCancelled("fixture.cancelled");
        return event;
    }

    mutable std::vector<std::int64_t> events_;
};

Result<std::int64_t> run(const std::shared_ptr<Probe>& probe, std::int64_t scenario, std::size_t steps)
{
#ifdef PIPELANG_REFERENCE
    try {
        pipelang::model::TaskOptions options;
        options.initializationSteps = steps;
        reference::source::NativeProof::HostedConstructionCleanup::Session session(probe, scenario, options);
        return session.Run();
    } catch (const OperationCancelled& cancellation) {
        return Result<std::int64_t>::Cancelled(cancellation.what());
    } catch (const ExecutionFailure& failure) {
        return Result<std::int64_t>::Failure(failure.what());
    }
#else
    const std::array<HostedConstructorArgument, 2> arguments{probe, scenario};
    const auto session = HostedInstance::bind(test_construction_cleanup, arguments, steps);
    if (session.is_failure())
        return Result<std::int64_t>::Failure(session.error());
    if (session.is_cancelled())
        return Result<std::int64_t>::Cancelled(session.error());

    pipelang::model::TaskScope scope;
    return session.value().start(scope, {}).result();
#endif
}

void verify(std::int64_t scenario, const Result<std::int64_t>& expected, const std::vector<std::int64_t>& events,
            std::size_t steps = pipelang::model::defaultEvaluationSteps)
{
    const auto probe = std::make_shared<Probe>();
    const auto result = run(probe, scenario, steps);
    if (result != expected || probe->events() != events)
        throw std::runtime_error("constructor cleanup changed the outcome or callback order in case " +
                                 std::to_string(scenario) + ": " + result.error());
}

void printBudgetBoundaries()
{
    constexpr std::size_t maximumTestBudget = 4096;
    const std::array scenarios{0, 1, 6, 7, 9, 10, 11, 13, 15, 16, 17, 18, 19, 21, 22, 23, 24,
        25, 27, 28, 29, 30};
    for (const auto scenario : scenarios) {
        const auto fullProbe = std::make_shared<Probe>();
        const auto expected = run(fullProbe, scenario, pipelang::model::defaultEvaluationSteps);
        if (!expected.is_success())
            throw std::runtime_error("expected a successful construction at the default budget");

        std::string previous;
        for (std::size_t budget = 1; budget <= maximumTestBudget; ++budget) {
            const auto probe = std::make_shared<Probe>();
            const auto outcome = run(probe, scenario, budget);
            std::ostringstream observation;
            observation << static_cast<int>(outcome.outcome()) << ' ';
            if (outcome.is_success())
                observation << outcome.value();
            else
                observation << outcome.error();
            for (const auto event : probe->events())
                observation << ' ' << event;
            const auto current = observation.str();
            if (current != previous) {
                std::cout << scenario << ' ' << budget << ' ' << current << '\n';
                previous = current;
            }
            if (outcome.is_success()) {
                if (outcome != expected || probe->events() != fullProbe->events())
                    throw std::runtime_error("minimum successful construction changed its result or effects");
                break;
            }
            if (budget == maximumTestBudget)
                throw std::runtime_error("no successful construction under the test budget");
        }
    }
}

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--budget-boundaries") {
            printBudgetBoundaries();
            return 0;
        }
        if (argc != 1)
            return 64;
        verify(0, Result<std::int64_t>::Success(42), {2, 10});
        verify(1, Result<std::int64_t>::Success(19), {2, 10});
        verify(2, Result<std::int64_t>::Failure("fixture.primary"), {2, 20, 10});
        verify(3, Result<std::int64_t>::Cancelled("fixture.cancelled"), {3, 2, 10});
        verify(4, Result<std::int64_t>::Failure("fixture.cleanup"), {2, 20, 10});
        verify(5, Result<std::int64_t>::Failure("source evaluation budget exceeded"), {10}, 128);
        verify(6, Result<std::int64_t>::Success(20), {2, 10});
        verify(7, Result<std::int64_t>::Success(25), {2, 10});
        verify(8, Result<std::int64_t>::Success(13), {2, 10}, 1000000);
        verify(9, Result<std::int64_t>::Success(23), {2, 10});
        verify(10, Result<std::int64_t>::Success(21), {2, 10});
        verify(11, Result<std::int64_t>::Success(42), {2, 10});
        verify(12, Result<std::int64_t>::Failure("source call depth limit exceeded"), {2, 10});
        verify(13, Result<std::int64_t>::Success(42), {2, 10});
        verify(14, Result<std::int64_t>::Failure("source call depth limit exceeded"), {2, 10});
        verify(15, Result<std::int64_t>::Success(37), {2, 10});
        verify(16, Result<std::int64_t>::Success(21), {2, 10});
        verify(17, Result<std::int64_t>::Success(23), {2, 10});
        verify(18, Result<std::int64_t>::Success(16), {2, 10});
        verify(19, Result<std::int64_t>::Success(31), {2, 10});
        verify(20, Result<std::int64_t>::Success(43), {2, 10}, 1000000);
        verify(21, Result<std::int64_t>::Success(42), {2, 10});
        verify(22, Result<std::int64_t>::Success(42), {2, 10});
        verify(23, Result<std::int64_t>::Success(42), {2, 10});
        verify(24, Result<std::int64_t>::Success(42), {2, 10});
        verify(25, Result<std::int64_t>::Success(42), {2, 10});
        verify(26, Result<std::int64_t>::Failure("collection index outside admitted range"), {2, 10});
        verify(27, Result<std::int64_t>::Success(42), {2, 10});
        verify(28, Result<std::int64_t>::Success(42), {2, 10});
        verify(29, Result<std::int64_t>::Success(42), {2, 10});
        verify(30, Result<std::int64_t>::Success(30), {2, 10});
        std::cout << "Hosted construction preserved catch, nested cleanup, transfer and failure precedence.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
