// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace native = pipelang::native_app_v19;
namespace model = pipelang::model;

extern "C" const native::HostedModel proof_assignment;

void require(bool valid, const char* message)
{
    if (!valid)
        throw std::runtime_error(message);
}

class AssignmentPort final : public native::HostedService {
public:
    std::string_view interfaceName() const noexcept override
    {
        return "AssignmentOrder.AssignmentPort";
    }

    model::Result<native::HostedServiceValue> invoke(
        std::string_view signature, std::span<const native::HostedServiceValue> arguments) override
    {
        using Outcome = model::Result<native::HostedServiceValue>;
        if (signature == "Acquire()" && arguments.empty()) {
            ++acquired;
            if (onAcquire)
                onAcquire();
            return Outcome::Success(acquired);
        }
        if (signature == "Release(int,CancellationToken)" && arguments.size() == 2) {
            const auto& token = std::get<model::CancellationToken>(arguments[1]);
            require(!token.is_cancelled(), "cleanup inherited the source request token");
            released.push_back(std::get<std::int64_t>(arguments[0]));
            return Outcome::Success(native::HostedValue{});
        }
        if (signature == "Mark(int)" && arguments.size() == 1) {
            const auto phase = std::get<std::int64_t>(arguments[0]);
            order.push_back(phase);
            return Outcome::Success(phase);
        }
        return Outcome::Failure(std::string("unexpected source port call ") + std::string(signature));
    }

    std::int64_t acquired = 0;
    std::function<void()> onAcquire;
    std::vector<std::int64_t> released;
    std::vector<std::int64_t> order;
};

int main()
{
    try {
        auto port = std::make_shared<AssignmentPort>();
        const std::array<native::HostedConstructorArgument, 1> inputs{port};
        const auto bound = native::HostedInstance::bind(proof_assignment, inputs);
        if (bound.is_failure())
            throw std::runtime_error(bound.error());
        require(bound.is_success(), "hosted assignment fixture refused its source constructor");

        model::TaskScope tasks;
        model::CancellationSource first;
        port->onAcquire = [&] { first.cancel(); };
        const auto local = bound.value().startMethod(tasks, "Run", {}, first.token()).result();
        require(local.is_cancelled(), "local assignment lost its cancellation");
        require(port->released == std::vector<std::int64_t>{1},
                "protected local assignment lost a successful acquisition");

        model::CancellationSource second;
        port->onAcquire = [&] { second.cancel(); };
        const auto record = bound.value().startMethod(tasks, "RunRecord", {}, second.token()).result();
        require(record.is_cancelled(), "nested record assignment lost its cancellation");
        require(port->released == std::vector<std::int64_t>{1, 2},
                "protected nested record assignment lost a successful acquisition");

        port->onAcquire = {};
        const auto order = bound.value().startMethod(tasks, "Ordered", {}).result();
        if (order.is_failure())
            throw std::runtime_error(order.error());
        require(order.is_success() && order.value().integer() == 2, "receiver assignment failed");
        require(port->order == std::vector<std::int64_t>{1, 2}, "receiver evaluation did not precede its RHS");
        std::cout << "native assignment order passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
