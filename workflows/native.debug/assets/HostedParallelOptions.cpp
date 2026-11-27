// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;
using namespace std::chrono_literals;

extern "C" const native::HostedModel proof_parallel_options;

void require(bool condition, const char* diagnostic)
{
    if (!condition)
        throw std::runtime_error(diagnostic);
}

template<class T>
T take(model::Result<T> result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return std::move(result).value();
}

void verifyParallelism(std::size_t degree, bool useDefault)
{
    const auto binding = useDefault ? native::HostedInstance::bind(proof_parallel_options, {})
        : native::HostedInstance::bind(proof_parallel_options, {}, model::defaultEvaluationSteps, {degree});
    const auto instance = take(binding);
    model::TaskScope tasks;
    const auto arrivals = model::Atomic<std::int64_t>::Create(0);
    const auto proceed = model::SemaphoreSlim::Create(0, 8);
    std::vector<native::HostedValue> jobs;
    for (std::int64_t job = 0; job < 8; ++job)
        jobs.push_back(native::HostedValue::fromInteger(job));
    const std::array<native::HostedCallArgument, 3> arguments{
        native::HostedValue::fromList("List<int>", std::move(jobs)),
        native::HostedValue::fromAtomic(arrivals),
        native::HostedValue::fromSemaphore(proceed)
    };

    std::int64_t observed = -1;
    std::jthread observer([&] {
        const auto expected = static_cast<std::int64_t>(degree);
        const auto deadline = std::chrono::steady_clock::now() + 900ms;
        while (arrivals.load() < expected && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);

        // Every entered item waits on the same SDK cell, so this counts active workers.
        std::this_thread::sleep_for(60ms);
        observed = arrivals.load();
        proceed.release(8);
    });
    const auto result = instance.startWithValues(tasks, arguments);
    observer.join();
    require(result.completed() && take(result.result()).integer() == 8, "some parallel work failed to settle");
    require(observed == static_cast<std::int64_t>(degree), "hosted invocation ignored its worker limit");
}

int main()
{
    try {
        for (std::size_t degree : {std::size_t{0}, model::ParallelOptions::maximumDegree + 1}) {
            const auto rejected = native::HostedInstance::bind(proof_parallel_options, {},
                model::defaultEvaluationSteps, {degree});
            require(rejected.is_failure() && rejected.error() == "parallel degree must be between 1 and 32",
                    "invalid parallel degree entered the hosted model");
        }
        verifyParallelism(1, false);
        verifyParallelism(2, false);
        verifyParallelism(4, true);
        std::cout << "The native SDK honored per-binding parallel worker limits.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
