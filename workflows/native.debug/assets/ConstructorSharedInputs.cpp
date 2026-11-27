// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_constructor_inputs;
extern "C" const native::HostedModel proof_constructor_inputs_graph;

struct Value {
    std::int64_t id;
};

void require(bool condition, const char* diagnostic)
{
    if (!condition)
        throw std::runtime_error(diagnostic);
}

void verify(const native::HostedModel& descriptor)
{
    const model::ConcurrentQueue<std::string> queue;
    const model::ConcurrentDictionary<std::string, Value> dictionary;
    const auto calls = model::Atomic<std::int64_t>::Create(0);
    const std::string type = "NativeProof.ConstructorSharedInputs.Value";
    const native::HostedValueCodec<std::string> textCodec{
        "string", native::HostedValue::fromText,
        [](const native::HostedValue& input) { return std::string(input.text()); }
    };
    const native::HostedValueCodec<Value> recordCodec{
        type, [&](const Value& input) {
            return native::HostedValue::fromRecord(type, {{"id", native::HostedValue::fromInteger(input.id)}});
        },
        [](const native::HostedValue& input) {
            return Value{input.field("id").integer()};
        }
    };

    const auto hostQueue = native::HostedValue::fromConcurrentQueue(queue, textCodec);
    const auto hostDictionary = native::HostedValue::fromConcurrentDictionary(dictionary, textCodec, recordCodec);

    std::array<native::HostedConstructorArgument, 4> arguments{
        hostQueue, hostDictionary, native::HostedValue::fromAtomic(model::Atomic<bool>::Create(false)), std::int64_t{0}
    };
    const auto mismatched = native::HostedInstance::bind(descriptor, arguments);
    require(mismatched.is_failure() && mismatched.error() == "native shared argument does not match the checked type" &&
                queue.count() == 0 && dictionary.count() == 0 && calls.load() == 0,
            "invalid shared constructor input reached source or changed a host cell");

    arguments[2] = native::HostedValue::fromAtomic(calls);
    arguments[3] = std::int64_t{1};
    const auto failed = native::HostedInstance::bind(descriptor, arguments);
    require(failed.is_failure() && failed.error() == "constructor failed after shared effects" &&
                queue.count() == 1 && dictionary.at("constructor").id == 17 && calls.load() == 1,
            "failed binding lost the effects on its borrowed cells");

    arguments[3] = std::int64_t{0};
    const auto bound = native::HostedInstance::bind(descriptor, arguments);
    if (!bound.is_success())
        throw std::runtime_error(bound.error());
    arguments = {};
    model::TaskScope scope;
    const auto task = bound.value().start(scope, {});
    require(task.completed() && task.result().is_success() && task.result().value() == 8 &&
                queue.count() == 3 && dictionary.at("method").id == 42 && calls.load() == 3,
            "the bound receiver lost or copied its host cells");
}

int main()
{
    try {
        verify(proof_constructor_inputs);
        verify(proof_constructor_inputs_graph);
        std::cout << "Native constructors borrowed the host cells and preserved direct failure and shared effects.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
