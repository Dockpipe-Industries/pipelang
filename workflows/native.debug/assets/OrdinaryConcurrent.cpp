// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_concurrent;

template<class Value>
Value requireValue(model::Result<Value> result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return std::move(result).value();
}

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int main()
{
    try {
        model::TaskOptions options;
        options.evaluationStepsPerSegment = 131072;
        model::TaskScope tasks(options);
        const auto instance = requireValue(native::HostedInstance::bind(proof_concurrent, {}));
        const auto invoke = [&](const native::HostedInstance& binding, std::string_view name,
                              std::span<const native::HostedCallArgument> arguments = {}) {
            const auto task = binding.startMethod(tasks, name, arguments);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!task.completed() && std::chrono::steady_clock::now() < deadline) {
                tasks.poll();
                if (!task.completed())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(task.completed(), "concurrent source task did not settle");
            return task.result();
        };
        const auto call = [&](std::string_view name, std::span<const native::HostedCallArgument> arguments = {}) {
            return requireValue(invoke(instance, name, arguments));
        };

        std::vector<native::HostedValue> values;
        for (std::int64_t index = 0; index < 8; ++index)
            values.push_back(native::HostedValue::fromInteger(index));
        const std::array<native::HostedCallArgument, 1> items{
            native::HostedValue::fromList("List<int>", std::move(values))
        };

        require(call("DefaultCounts").integer() == 0, "default count differs from the C++ reference");
        require(call("CompoundOperations").boolean(), "compound dictionary operations diverged");
        require(call("SequenceOperations").boolean(), "concurrent FIFO and LIFO operations diverged");
        require(call("GenericModuleHelper").integer() == 13, "cross-module generic record use diverged");
        require(call("ParallelShared", items).integer() == 8028 && call("QueueCount").integer() == 8 &&
                call("StackCount").integer() == 8, "parallel shared mutations diverged");
        for (std::int64_t index = 0; index < 8; ++index) {
            const std::array<native::HostedCallArgument, 1> key{index};
            require(call("MapValue", key).integer() == index * 2, "a parallel dictionary value was lost");
        }
        const auto snapshot = call("QueueSnapshot");
        require(snapshot.size() == 8 && call("ParallelSnapshot").integer() == 8006 && snapshot.size() == 8,
                "parallel iteration lost its owned snapshot");
        require(call("OrdinarySnapshot").integer() == 403, "ordinary iteration lost its snapshot");
        require(call("AsyncLocalCapture").integer() == 2, "async capture lost concurrent identity");

        const model::ConcurrentQueue<std::int64_t> external;
        const std::array<native::HostedCallArgument, 1> shared{
            native::HostedValue::fromConcurrentQueue(external)
        };
        require(call("PublicAsyncHandle", shared).integer() == 1 && external.tryDequeue().value() == 8,
                "the original fixture lost the host queue's identity");

        const auto independent = requireValue(native::HostedInstance::bind(proof_concurrent, {}));
        const auto fault = invoke(independent, "FailAfterSharedEffects");
        const std::array<native::HostedCallArgument, 1> faultKey{std::int64_t{999}};
        require(fault.is_failure() && fault.error() == "after shared effects" &&
                requireValue(invoke(independent, "MapValue", faultKey)).integer() == 5 &&
                requireValue(invoke(independent, "QueueCount")).integer() == 1,
                "ordinary failure changed committed shared effects");

        model::CancellationSource cancellation;
        cancellation.cancel();
        const std::array<native::HostedCallArgument, 2> cancelledArguments{items.front(), cancellation.token()};
        const std::array<native::HostedCallArgument, 1> cancelledKey{std::int64_t{777}};
        require(invoke(independent, "Cancelled", cancelledArguments).is_cancelled() &&
                requireValue(invoke(independent, "MapValue", cancelledKey)).integer() == 7 &&
                requireValue(invoke(independent, "QueueCount")).integer() == 1,
                "cancelled parallel mutation changed earlier shared effects");

        const std::array<native::HostedCallArgument, 2> bigStrings{
            native::HostedValue::fromText(std::string(700000, 'a')),
            native::HostedValue::fromText(std::string(500000, 'b'))
        };
        require(invoke(independent, "ByteLimitPersistent", bigStrings).is_failure() &&
                requireValue(invoke(independent, "TextCount")).integer() == 1 &&
                requireValue(invoke(independent, "FailedTextEntryAbsent")).boolean(),
                "a rejected concurrent write changed persistent storage");
        std::cout << "Original concurrent operations matched the C++ reference values and effects.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
