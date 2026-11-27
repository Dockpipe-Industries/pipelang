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

extern "C" const native::HostedModel proof_collection_inputs;

struct Row {
    std::int64_t id;
};

template<class Value>
Value take(model::Result<Value> result)
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
        const auto instance = take(native::HostedInstance::bind(proof_collection_inputs, {}));
        model::TaskScope tasks;
        const auto call = [&](std::string_view method, std::span<const native::HostedCallArgument> arguments) {
            const auto task = instance.startMethod(tasks, method, arguments);
            require(task.completed(), "synchronous hosted sequence task did not finish");
            return task.result();
        };

        const model::ConcurrentQueue<std::string> queue;
        const model::ConcurrentStack<Row> stack;
        const std::string rowType = "NativeProof.HostedCollectionInputs.Row";
        const native::HostedValueCodec<std::string> stringCodec{
            "string", native::HostedValue::fromText,
            [](const native::HostedValue& value) { return std::string(value.text()); }
        };
        const native::HostedValueCodec<Row> rowCodec{
            rowType, [&](const Row& item) {
                return native::HostedValue::fromRecord(rowType, {{"id", native::HostedValue::fromInteger(item.id)}});
            },
            [](const native::HostedValue& value) { return Row{value.field("id").integer()}; }
        };

        queue.enqueue("host");
        stack.push({5});
        const auto hostQueue = native::HostedValue::fromConcurrentQueue(queue, stringCodec);
        const auto hostStack = native::HostedValue::fromConcurrentStack(stack, rowCodec);
        const std::array<native::HostedCallArgument, 2> arguments{hostQueue, hostStack};
        const auto mixed = call("Run", arguments);
        require(take(mixed).integer() == 2 && queue.tryPeek().value() == "native" && stack.tryPeek().value().id == 42,
                "PipeLang and C++ did not share the same queue and stack");

        const std::array<native::HostedCallArgument, 2> wrongOrder{hostStack, hostQueue};
        require(call("Run", wrongOrder).is_failure() && queue.count() == 1 && stack.count() == 1,
                "a mismatched borrowed collection crossed the method boundary");
        auto brokenCodec = rowCodec;
        brokenCodec.encode = [](const Row&) { return native::HostedValue::fromInteger(-1); };
        const std::array<native::HostedCallArgument, 1> broken{native::HostedValue::fromConcurrentStack(stack, brokenCodec)};
        require(call("Pop", broken).is_failure() && stack.count() == 1 && stack.tryPeek().value().id == 42,
                "failed projection removed an item from the host stack");
        auto voidCodec = rowCodec;
        voidCodec.encode = [](const Row&) { return native::HostedValue{}; };
        const std::array<native::HostedCallArgument, 1> missingEncoding{
            native::HostedValue::fromConcurrentStack(stack, voidCodec)
        };
        const auto missing = call("Pop", missingEncoding);
        require(missing.is_failure() && missing.error() == "host collection codec encoded a void value" && stack.count() == 1,
                "a void codec projection was mistaken for an empty stack");

        const auto gate = model::SemaphoreSlim::Create(0, 1);
        const std::array<native::HostedCallArgument, 3> waiting{
            hostQueue, native::HostedValue::fromSemaphore(gate), model::CancellationToken{}
        };
        const auto parked = instance.startMethod(tasks, "Park", waiting);
        require(!parked.completed(), "the borrowed sequence did not survive suspension");
        gate.release();
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!parked.completed() && std::chrono::steady_clock::now() < deadline)
            tasks.poll();
        require(parked.completed() && take(parked.result()).integer() == 2 && queue.items().at(1) == "resumed",
                "resumed task did not use the original host queue");

        require(take(call("Workers", arguments)).integer() == 7 && queue.count() == 4 && stack.count() == 3,
                "parallel worker captures copied the borrowed collection");
        const std::array<native::HostedCallArgument, 1> snapshotArgs{hostStack};
        const auto snapshot = take(call("View", snapshotArgs));
        require(snapshot.size() == 3 && snapshot.at(2).field("id").integer() == 42,
                "borrowed stack snapshot did not preserve its order");
        const model::ConcurrentDictionary<std::string, Row> map;
        map.set("host", {5});
        const auto hostMap = native::HostedValue::fromConcurrentDictionary(map, stringCodec, rowCodec);
        const std::array<native::HostedCallArgument, 1> mapArguments{hostMap};
        require(take(call("Map", mapArguments)).integer() == 42 && map.count() == 2 &&
                    map.at("host").id == 17 && map.at("native").id == 17,
                "host and native code used different dictionary cells");

        const std::array<native::HostedCallArgument, 1> brokenMap{
            native::HostedValue::fromConcurrentDictionary(map, stringCodec, brokenCodec)
        };
        require(call("Remove", brokenMap).is_failure() && call("Ensure", brokenMap).is_failure() &&
                    map.containsKey("host") && !map.containsKey("staged"),
                "failed dictionary projection published a removal or insertion");
        const std::array<native::HostedCallArgument, 1> voidMap{
            native::HostedValue::fromConcurrentDictionary(map, stringCodec, voidCodec)
        };
        require(call("Remove", voidMap).is_failure() && map.containsKey("host"),
                "a void codec projection removed an existing dictionary entry");
        require(take(call("Remove", mapArguments)).integer() == 17 && !map.containsKey("host"),
                "native removal did not affect the host dictionary");
        require(take(call("Ensure", mapArguments)).integer() == 83 && map.at("staged").id == 83,
                "native get-or-add did not publish to the host dictionary");
        require(take(call("MapWorkers", mapArguments)).integer() == 4 &&
                    map.at("left").id == 9 && map.at("right").id == 9,
                "native workers did not retain the host dictionary");
        require(call("Clear", mapArguments).is_success() && map.count() == 0, "native clear did not affect the host dictionary");
        std::cout << "Native borrowed collections kept C++ cell identity, checked projections, and parallel writes.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
