// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "GenericMemberCalls.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main() {
    generic_member_calls::App app;
    assert(app.Primitive());
    assert(app.Records());
    assert(app.Local().value());
    assert(app.NestedField());
    assert(app.Constructed().value());
    assert(app.Inherited());
    assert(app.Overloads() == 7);
    assert(app.CallReceiver().value() == 101);
    assert(app.snapshot().acquisitions == 1);
    assert(app.CallReceiver().value() == 202);
    assert(app.snapshot().acquisitions == 2);

    generic_member_calls::App independent;
    assert(independent.CallReceiver().value() == 101);
    assert(app.snapshot().acquisitions == 2);
}
