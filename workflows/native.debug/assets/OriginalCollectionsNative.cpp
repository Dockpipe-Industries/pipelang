// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <iostream>
#include <stdexcept>

using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_collections;

void require(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error(std::string(message));
}

HostedValue checked(const HostedValueResult& result)
{
    require(result.is_success(), result.is_success() ? "" : result.error());
    return result.value();
}

void requirePair(const HostedValue& pair, std::string_view key, std::int64_t value)
{
    require(pair.field("key").text() == key && pair.field("value").integer() == value, "unexpected mapping entry");
}

void requireSnapshot(const HostedValue& snapshot, bool edited)
{
    require(snapshot.field("marker").integer() == 0 && !snapshot.field("wrapped").hasValue(),
            "source results changed ordinary state");
    const auto array = snapshot.field("array");
    require(array.size() == 2 && array.at(0).integer() == 3 && array.at(1).integer() == (edited ? 11 : 3),
            "array state or rollback changed");
    const auto dictionary = snapshot.field("map");
    require(dictionary.size() == (edited ? 3 : 2), "dictionary size or rollback changed");
    requirePair(dictionary.at(0), "first", 1);
    requirePair(dictionary.at(1), "second", 2);
    if (edited)
        requirePair(dictionary.at(2), "third", 3);
    const auto queue = snapshot.field("queue");
    const auto stack = snapshot.field("stack");
    require(queue.size() == (edited ? 1 : 2) && queue.at(0).integer() == (edited ? 5 : 4),
            "queue order or rollback changed");
    require(stack.size() == (edited ? 1 : 2) && stack.at(0).integer() == (edited ? 6 : 7),
            "stack order or rollback changed");
}

int main()
{
    try {
        pipelang::model::TaskScope scope;
        auto instance = HostedInstance::bind(proof_collections, {});
        require(instance.is_success(), instance.is_success() ? "" : instance.error());
        const auto& model = instance.value();
        const auto call = [&](std::string_view method) { return checked(model.startMethod(scope, method, {}).result()); };
        require(checked(model.snapshot()).field("array").size() == 0, "initial array was not empty");
        (void)checked(model.startValue(scope, {}).result());
        require(call("ArrayCopies").boolean(), "copy-on-write array contract failed");
        require(call("TemporaryOptionalRead").integer() == 7, "optional array projection failed");
        require(call("TemporaryResultRead").integer() == 13, "result queue projection failed");
        require(call("ReceiverBeforeArguments").integer() == 17 && call("WrappedIsEmpty").boolean(),
                "receiver evaluation order changed");
        const auto ordered = call("MappingOrder");
        require(ordered.size() == 3, "mapping count changed");
        requirePair(ordered.at(0), "one", 10);
        requirePair(ordered.at(1), "three", 3);
        requirePair(ordered.at(2), "two", 20);
        require(call("MappingIteration").text() == "firstsecondfirsttyped", "mapping iteration changed");
        require(call("DictionaryOptions").boolean(), "dictionary optional results changed");
        const auto set = call("SetOrder");
        require(set.size() == 2 && set.at(0).text() == "second" && set.at(1).text() == "first", "set iteration changed");
        require(call("PrimitiveKeys").boolean(), "primitive keys changed");
        require(call("SequenceOrder").integer() == 1243 && call("SequenceOperations").boolean(), "queue or stack changed");

        const auto bundle = call("OwnedBundle");
        const auto array = bundle.field("values").at(0);
        const auto mapping = bundle.field("mapping").at(0);
        const auto pair = bundle.field("pair");
        require(array.field("number").integer() == 5 && array.field("text").text() == "array", "retained array alias changed");
        require(mapping.field("key").text() == "key" && mapping.field("value").field("number").integer() == 6,
                "retained mapping alias changed");
        require(pair.field("key").text() == "pair" && pair.field("value").field("number").integer() == 7,
                "retained pair changed");

        for (const auto* operation : {"MissingRollback", "IndexRollback", "DuplicateRollback", "EmptyRollback"}) {
            require(model.startMethod(scope, operation, {}).result().is_failure(), "invalid collection operation succeeded");
            requireSnapshot(checked(model.snapshot()), false);
        }
        require(call("EditFields").integer() == 22, "collection field edit changed");
        requireSnapshot(checked(model.snapshot()), true);
        std::cout << "Original PipeLang collections preserve aliases, order, optional values and rollback.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
