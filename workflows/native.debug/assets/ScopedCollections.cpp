// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;
using native::HostedValue;

extern "C" const native::HostedModel proof_root;

void require(bool valid, const char* diagnostic)
{
    if (!valid)
        throw std::runtime_error(diagnostic);
}

HostedValue bundle(std::string name, std::int64_t identity)
{
    return HostedValue::fromRecord("NativeProof.ScopedCollections.Bundle", {
        {"name", HostedValue::fromText(name)},
        {"labels", HostedValue::fromList("List<string>", {HostedValue::fromText(name)})},
        {"values", HostedValue::fromList("Array<int>", {HostedValue::fromInteger(identity), HostedValue::fromInteger(7)})},
        {"tags", HostedValue::fromHashSet("HashSet<string>", {HostedValue::fromText(name)})},
        {"queue", HostedValue::fromQueue("Queue<int>", {HostedValue::fromInteger(identity), HostedValue::fromInteger(1)})},
        {"stack", HostedValue::fromStack("Stack<int>", {HostedValue::fromInteger(identity), HostedValue::fromInteger(2)})},
        {"lookup", HostedValue::fromDictionary("Dictionary<string,int>", {
            HostedValue::fromRecord("KeyValuePair<string,int>", {
                {"key", HostedValue::fromText(name)}, {"value", HostedValue::fromInteger(identity)}
            })
        })}
    });
}

void check(const native::HostedValueResult& result, std::string_view name, std::int64_t identity, bool initialized)
{
    if (result.is_failure())
        throw std::runtime_error(result.error());
    require(result.is_success(), "scoped collection read did not finish");
    const auto& value = result.value();
    require(value.field("name").text() == name, "inline string field was not retained with the collections");
    const auto labels = value.field("labels");
    const auto array = value.field("values");
    const auto tags = value.field("tags");
    require(labels.size() == (initialized ? 2 : 1) && labels.at(0).text() == name &&
            (!initialized || labels.at(1).text() == "constructed"), "retained list changed");
    require(array.size() == 2 && array.at(0).integer() == identity + (initialized ? 1 : 0) && array.at(1).integer() == 7,
            "retained array changed");
    require(tags.size() == (initialized ? 2 : 1), "retained hash set changed");
    const auto queue = value.field("queue");
    const auto stack = value.field("stack");
    const auto lookup = value.field("lookup");
    require(queue.size() == 2 && queue.at(0).integer() == identity && queue.at(1).integer() == 1, "retained queue changed");
    require(stack.size() == 2 && stack.at(0).integer() == identity && stack.at(1).integer() == 2, "retained stack changed");
    require(lookup.size() == 1 && lookup.at(0).field("key").text() == name &&
            lookup.at(0).field("value").integer() == identity, "retained dictionary changed");
}

int main()
{
    try {
        const auto root = native::HostedInstance::bind(proof_root, {});
        require(root.is_success(), "could not initialize the native receiver graph");
        const auto value = [&] {
            const std::array<native::HostedConstructorArgument, 1> input{bundle("initial", 41)};
            return root.value().constructService("NativeProof.ScopedCollections.Consumer", input);
        }();
        if (value.is_failure())
            throw std::runtime_error(value.error());
        require(value.is_success(), "could not construct the scoped collection consumer");

        model::TaskScope tasks;
        const std::array<native::HostedCallArgument, 1> read{value.value()};
        check(root.value().startMethod(tasks, "Read", read).result(), "initial", 41, true);

        const auto first = tasks.createGroup();
        const auto second = tasks.createGroup();
        const std::array<native::HostedCallArgument, 2> waitArgs{value.value(), model::CancellationToken{}};
        const auto held = root.value().tryStartMethod(tasks, first, "Hold", waitArgs);
        require(held.is_success() && !held.value().completed(), "reader did not suspend with its scoped snapshot");
        const std::array<native::HostedCallArgument, 2> updateArgs{value.value(), bundle("updated", 22)};
        const auto changed = root.value().tryStartMethod(tasks, second, "Update", updateArgs);
        require(changed.is_success() && changed.value().result().is_success(), "sibling could not replace scoped collections");
        check(root.value().startMethod(tasks, "Read", read).result(), "updated", 22, false);

        const auto released = root.value().tryStartMethod(tasks, second, "Release", {});
        require(released.is_success() && released.value().result().is_success(), "sibling could not release the parked reader");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!held.value().completed() && std::chrono::steady_clock::now() < deadline) {
            tasks.poll();
            std::this_thread::yield();
        }
        require(held.value().completed(), "scoped collection reader did not resume");
        check(held.value().result(), "initial", 41, true);
        std::cout << "Scoped native collections retained exact values through constructor writes and independent tasks.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
