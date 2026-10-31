// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Collections.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
using namespace pipelang_models;

int main() {
    CollectionModel model;
    assert(model.Seed().is_success());
    assert(model.ArrayCopies());
    assert(model.TemporaryOptionalRead() == 7);
    assert(model.TemporaryResultRead() == 13);
    assert(model.ReceiverBeforeArguments().value() == 17);
    assert(model.WrappedIsEmpty());
    const auto ordered = model.MappingOrder();
    assert(ordered.count() == 3);
    assert(ordered.at(0).key == "one" && ordered.at(0).value == 10);
    assert(ordered.at(1).key == "three" && ordered.at(1).value == 3);
    assert(ordered.at(2).key == "two" && ordered.at(2).value == 20);
    assert(model.MappingIteration() == "firstsecondfirsttyped");
    assert(model.DictionaryOptions());
    const auto setOrder = model.SetOrder();
    assert(setOrder.count() == 2 && setOrder.at(0) == "second" && setOrder.at(1) == "first");
    assert(model.PrimitiveKeys());
    assert(model.SequenceOrder() == 1243);
    assert(model.SequenceOperations());
    auto bundle = model.OwnedBundle();
    assert(bundle.values.at(0) == (CollectionValue{5, "array"}));
    assert(bundle.mapping.at("key") == (CollectionValue{6, "map"}));
    assert(bundle.pair.key == "pair" && bundle.pair.value == (CollectionValue{7, "pair"}));
    auto detached = bundle;
    detached.values.set(0, CollectionValue{88, "host"});
    detached.mapping.set("key", CollectionValue{88, "host"});
    assert(bundle.values.at(0).number == 5 && bundle.mapping.at("key").number == 6);

    const auto before = model.snapshot();
    assert(model.MissingRollback().is_failure());
    assert(model.snapshot() == before);
    assert(model.IndexRollback().is_failure());
    assert(model.snapshot() == before);
    assert(model.DuplicateRollback().is_failure());
    assert(model.snapshot() == before);
    assert(model.EmptyRollback().is_failure());
    assert(model.snapshot() == before);
    assert(model.EditFields().value() == 22);
    assert(model.snapshot().array.at(1) == 11);
    assert(model.snapshot().map.at("third") == 3);
    assert(model.snapshot().queue.peek() == 5 && model.snapshot().stack.peek() == 6);
}
