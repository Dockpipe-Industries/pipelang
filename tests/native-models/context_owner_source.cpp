// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ContextOwnerSource.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <memory>
#include <type_traits>

using context_owner_source::ContextOwnerModel;
using pipelang::model::ContextOwner;

static_assert(std::is_same_v<decltype(std::declval<ContextOwnerModel>().Lifetime()),
    std::shared_ptr<ContextOwner>>);

int main() {
    ContextOwnerModel first;
    ContextOwnerModel second;
    const auto identity = first.Lifetime();
    assert(identity && identity == first.Lifetime());
    assert(identity != second.Lifetime());

    const auto initial = identity->capture();
    assert(initial.is_current());
    assert(first.Replace().is_success());
    assert(!initial.is_current() && initial.cancellation().is_cancelled());
    assert(first.Lifetime() == identity);
    const auto replacement = identity->capture();
    assert(replacement.is_current());

    assert(first.Invalidate().is_success());
    assert(!replacement.is_current() && !identity->capture().is_current());
    assert(first.Lifetime() == identity && first.snapshot().changes == 2);
    assert(first.Replace().is_success());
    const auto next = identity->capture();
    assert(next.is_current() && !initial.is_current());

    // The resource revocation survives a source transaction rollback. It must
    // never restore authority to a response captured before an invalidation.
    const auto failed = first.InvalidateThenFail();
    assert(failed.is_failure() && failed.error() == "source failure");
    assert(!next.is_current() && !identity->capture().is_current());
    assert(first.snapshot().changes == 3);

    assert(first.Close().is_success());
    assert(first.Close().is_success());
    assert(first.Replace().is_failure());
    assert(second.Lifetime()->capture().is_current());

    std::shared_ptr<ContextOwner> escaped;
    {
        ContextOwnerModel temporary;
        escaped = temporary.Lifetime();
        assert(escaped->capture().is_current());
    }
    assert(!escaped->capture().is_current());
    (void)escaped->replace();
    assert(!escaped->capture().is_current());

    ContextOwner parent;
    std::unique_ptr<ContextOwnerModel> child;
    {
        pipelang::model::InvocationContextScope invocation(parent.capture().permit());
        child = std::make_unique<ContextOwnerModel>();
    }
    const auto childIdentity = child->Lifetime();
    assert(childIdentity->capture().is_current());
    parent.invalidate();
    assert(!childIdentity->capture().is_current());
    assert(child->Replace().is_success());
    assert(!childIdentity->capture().is_current());
}
