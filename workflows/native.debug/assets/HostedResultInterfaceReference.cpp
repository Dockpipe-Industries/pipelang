// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using reference::source::NativeProof::HostedResultInterface::Session;
using pipelang::model::IResult;
using pipelang::model::Optional;
using pipelang::model::Result;

namespace {

using Erased = IResult<std::string>;

Result<Erased> run(Session& session, std::int64_t scenario, Optional<Erased> incoming = {})
{
    const auto task = session.Run(scenario, std::move(incoming));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("reference result interface test timed out");
        session.poll();
        if (!task.completed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return task.result();
}

Erased checkedValue(const Result<Erased>& result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return result.value();
}

void verifyError(const Erased& value)
{
    if (!value.is_failure() || value.error() != "retained error")
        throw std::runtime_error("reference result lost the erased error");
}

}

int main()
{
    try {
        Session origin(true);
        Session other(true);
        bool rejected = false;
        try {
            Session invalid(false);
        } catch (const pipelang::model::ExecutionFailure& error) {
            rejected = std::string_view(error.what()) == "result interface is empty";
        }
        if (!rejected)
            throw std::runtime_error("reference construction admitted an uninitialized result interface");
        verifyError(checkedValue(run(origin, 5)));
        const auto empty = run(origin, 0);
        if (!empty.is_failure() || empty.error() != "Optional.Value requires Some")
            throw std::runtime_error("an absent result interface admitted observation");

        const auto original = checkedValue(run(origin, 1));
        verifyError(original);
        verifyError(checkedValue(run(origin, 2, Optional<Erased>::Some(original))));
        const auto transferred = checkedValue(run(other, 3, Optional<Erased>::Some(original)));
        verifyError(transferred);
        verifyError(checkedValue(run(origin, 2, Optional<Erased>::Some(transferred))));
        const auto failed = run(other, 4);
        if (!failed.is_failure() || failed.error() != "discard this result interface")
            throw std::runtime_error("expected reference result rollback failure");
        verifyError(checkedValue(run(other, 2, Optional<Erased>::Some(original))));
        std::cout << "Hosted result identity, error storage, and rollback passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
