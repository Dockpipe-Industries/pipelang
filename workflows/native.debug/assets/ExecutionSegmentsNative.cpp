// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_hosted_execution_segments;

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

class Host final : public HostedService {
public:
    std::string_view interfaceName() const noexcept override
    {
        return "ExecutionSegments.SegmentHost";
    }

    pipelang::model::Result<HostedServiceValue> invoke(
        std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        require(signature == "Mark(int)" && arguments.size() == 1, "unexpected host call");
        require(std::this_thread::get_id() == owner_, "synchronous host call moved to a worker");
        marks.push_back(std::get<std::int64_t>(arguments.front()));
        if (cancelConstruction)
            throw pipelang::model::OperationCancelled("constructor host cancelled");
        if (parentToRevoke)
            parentToRevoke->invalidate();
        return pipelang::model::Result<HostedServiceValue>::Success(HostedValue{});
    }

    std::vector<std::int64_t> marks;
    bool cancelConstruction = false;
    pipelang::model::ContextOwner* parentToRevoke = nullptr;

private:
    const std::thread::id owner_ = std::this_thread::get_id();
};

int main()
{
    try {
        const auto host = std::make_shared<Host>();
        const std::array<HostedConstructorArgument, 1> arguments{std::shared_ptr<HostedService>(host)};
        const auto invalid = HostedInstance::bind(proof_hosted_execution_segments, arguments, 0);
        require(invalid.is_failure() && host->marks.empty(), "invalid budget entered source initialization");
        const auto refusal = HostedInstance::bind(proof_hosted_execution_segments, arguments, 1);
        require(refusal.is_failure() && host->marks.empty(), "exhausted budget entered the host effect");

        host->cancelConstruction = true;
        const auto cancelled = HostedInstance::bind(proof_hosted_execution_segments, arguments);
        require(cancelled.is_cancelled() && cancelled.error() == "constructor host cancelled",
                cancelled.is_success() ? "cancelled constructor returned a binding" : cancelled.error());
        require(host->marks == std::vector<std::int64_t>{0}, "cancelled host call did not execute once");

        host->cancelConstruction = false;
        host->marks.clear();
        pipelang::model::ContextOwner parent;
        {
            host->parentToRevoke = &parent;
            const pipelang::model::InvocationContextScope invocation(parent.capture().permit());
            const auto stale = HostedInstance::bind(proof_hosted_execution_segments, arguments);
            require(stale.is_cancelled(), "constructor published a binding under a revoked parent");
        }
        require(host->marks == std::vector<std::int64_t>{0}, "revoking host call did not execute once");

        host->parentToRevoke = nullptr;
        host->marks.clear();
        const auto bound = HostedInstance::bind(proof_hosted_execution_segments, arguments);
        require(bound.is_success(), bound.is_success() ? "" : bound.error());
        require(host->marks == std::vector<std::int64_t>{0}, "initialization host effect did not execute once");
        auto& model = bound.value();
        pipelang::model::TaskOptions options;
        options.evaluationStepsPerSegment = 131072;
        pipelang::model::TaskScope scope(options);
        const std::array<HostedArgument, 1> input{std::int64_t{10000}};
        const auto query = model.start(scope, input).result();
        require(query.is_success() && query.value() == 10000,
                query.is_success() ? "ordinary call could not use the initialized source receiver" : query.error());
        const std::array<HostedCallArgument, 1> cleanup{std::int64_t{2000}};
        const auto failed = model.startMethod(scope, "PrimaryFailure", cleanup).result();
        require(failed.is_failure() && failed.error() == "primary segment failure",
                failed.is_success() ? "source failure was lost" : failed.error());
        require(host->marks == std::vector<std::int64_t>{0, 2000}, "finally host effect was skipped");
        std::cout << "Checked synchronous host calls preserve owner-thread execution during construction.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
