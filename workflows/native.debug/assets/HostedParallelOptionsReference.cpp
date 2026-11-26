// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
using Session = reference::source::NativeProof::HostedParallelOptions::Session;
using namespace std::chrono_literals;

void verifyParallelism(Session& session, std::size_t degree)
{
    const auto arrivals = model::Atomic<std::int64_t>::Create(0);
    const auto proceed = model::SemaphoreSlim::Create(0, 8);
    model::List<std::int64_t> jobs;
    for (std::int64_t job = 0; job < 8; ++job)
        jobs.add(job);
    std::int64_t observed = -1;
    std::jthread observer([&] {
        const auto deadline = std::chrono::steady_clock::now() + 900ms;
        while (arrivals.load() < static_cast<std::int64_t>(degree) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(60ms);
        observed = arrivals.load();
        proceed.release(8);
    });
    const auto result = session.Run(jobs, arrivals, proceed);
    observer.join();
    if (!result.is_success() || result.value() != 8 || observed != static_cast<std::int64_t>(degree))
        throw std::runtime_error("reference model ignored its worker limit");
}

int main()
{
    try {
        Session single({}, {1});
        verifyParallelism(single, 1);
        Session pair({}, {2});
        verifyParallelism(pair, 2);
        Session defaults;
        verifyParallelism(defaults, 4);
        for (std::size_t degree : {std::size_t{0}, model::ParallelOptions::maximumDegree + 1}) {
            bool rejected = false;
            try {
                Session invalid({}, {degree});
            } catch (const model::ExecutionFailure& failure) {
                rejected = std::string_view(failure.what()) == "parallel degree must be between 1 and 32";
            }
            if (!rejected)
                throw std::runtime_error("reference accepted an invalid degree");
        }
        std::cout << "The C++ reference honored per-binding parallel worker limits.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
