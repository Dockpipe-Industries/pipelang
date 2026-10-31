// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ObjectComposition.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>

int main() {
    object_composition::App model;
    assert(model.InternalNames().value() == 20);
    assert(model.BagWork().value() == 14101);
    assert(model.BagState().value() == 14101);
    const auto before = model.snapshot();
    const auto failed = model.FailBagMutation();
    assert(failed.is_failure() && failed.error() == "bag fault");
    assert(model.snapshot() == before);
    assert(model.BagState().value() == 14101);
    assert(model.UpcastEnumeration().value() == 23);
    assert(model.PrivateFactory().value() == 42);
    assert(model.Text().value() == "pl_scope pl_transaction.commit() [this]");
    assert(model.IncrementField().value() == 5);
    assert(model.snapshot().mypl_scopeMarker == 5);
}
