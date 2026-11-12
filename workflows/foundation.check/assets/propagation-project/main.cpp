// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <result_propagation/CompositionFactory.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
namespace result_propagation {
using FailureCode = propagation_project::source::Propagation::FailureCode;
using FailureData = propagation_project::source::Propagation::FailureData;
}
#include "ResultPropagationChecks.hpp"

namespace {
class Host final : public propagation_project::source::Propagation::HostPort {
public:
    propagation_checks::IntResult Send(std::int64_t first, std::int64_t second) override {
        ++calls;
        assert(first == 1);
        return propagation_checks::IntResult::Success(first + second);
    }
    int calls = 0;
};
}

int main() {
    using namespace propagation_checks;
    using Startup = propagation_project::composition::Composition::Startup;
    const auto created = Startup::Create();
    assert(created.is_success());
    const auto root = created.value();
    const auto host = std::make_shared<Host>();
    const auto scoped = root->CreateScope(host);
    assert(scoped.is_success());
    const auto scope = scoped.value();
    const auto resolved = scope->ResolveResultPropagation();
    assert(resolved.is_success());
    auto model = resolved.value();
    verify(model);
    assert(model.Pure(IntResult::Success(8)).value().value() == 9);
    domainError(model.Pure(IntResult::Failure(error(88))), false, "caller", 88);
    assert(model.ObjectAssign(0).value().value() == 20);
    assert(model.Trace().value() == 12);
    domainError(model.ObjectAssign(1), false, "reader", 2);
    assert(model.Trace().value() == 12);
    assert(model.ObjectCollection(0).value().value() == 10);
    assert(model.Trace().value() == 12);
    domainError(model.ObjectCollection(2), true, "reader", 2);
    assert(model.ObjectFromResult(0).value().value() == 2);
    assert(model.Trace().value() == 12);
    domainError(model.ObjectFromResult(1), false, "reader", 1);
    assert(model.Trace().value() == 1);
    const auto client = scope->ResolvePortClient();
    assert(client.is_success());
    domainError(client.value().Send(IntResult::Failure(error(89))), false, "caller", 89);
    assert(host->calls == 0);
    assert(client.value().Send(IntResult::Success(9)).value().value() == 10);
    assert(host->calls == 1);
    const auto resolvedAsync = scope->ResolveAsyncPropagation();
    assert(resolvedAsync.is_success());
    const auto asynchronous = resolvedAsync.value();
    verifyAsync(asynchronous, [](const auto& admission) {
        assert(admission.is_success());
        return admission.value();
    });
    assert(scope->Close().is_success());
    assert(root->Close().is_success());
}
