// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <typed_result/CompositionFactory.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
namespace typed_host_errors {
using ErrorKind = typed_host_project::source::Errors::ErrorKind;
using DomainError = typed_host_project::source::Errors::DomainError;
using TypedHost = typed_host_project::source::Errors::TypedHost;
}
#include "TypedHostChecks.hpp"

int main() {
    using namespace typed_host_checks;
    using Startup = typed_host_project::composition::Composition::Startup;
    const auto created = Startup::Create();
    assert(created.is_success());
    const auto root = created.value();
    auto host = std::make_shared<Host>();
    const auto scoped = root->CreateScope(host);
    assert(scoped.is_success());
    const auto scope = scoped.value();
    const auto resolved = scope->ResolveTypedHostErrors();
    assert(resolved.is_success());
    auto model = resolved.value();
    outcomes(model, host);
    assert(model.Calls().value() == 4 && model.Cleanups().value() == 4);
    const auto preserved = model.Preserve(ReadResult::Cancelled(domainError(91)));
    assert(preserved.is_success() && preserved.value().is_cancelled());
    checkError(preserved.value().error(), 91);
    auto invalid = domainError(92);
    invalid.origin.provider = std::string("\xff", 1);
    const auto rejected = model.Preserve(ReadResult::Failure(invalid));
    assert(rejected.is_failure() && rejected.error() == "string contains invalid UTF-8");
    CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled = model.Read(0, cancellation.token());
    assert(cancelled.is_cancelled() && cancelled.error() == "di.execution_cancelled");
    assert(host->reads == 8);
    auto moved = std::move(model);
    assert(model.Read(0).is_failure());
    assert(scope->Close().is_success());
    assert(moved.Read(0).is_failure());
    assert(host->reads == 8);
    assert(root->Close().is_success());
}
