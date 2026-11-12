// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <result_interface/CompositionFactory.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
namespace result_interface {
using ReadCode = interface_project::source::Results::ReadCode;
using ReadError = interface_project::source::Results::ReadError;
}
#include "ResultInterfaceChecks.hpp"

int main() {
    using namespace result_interface_checks;
    using Startup = interface_project::composition::Composition::Startup;
    const auto created = Startup::Create();
    assert(created.is_success());
    const auto root = created.value();
    const auto scoped = root->CreateScope();
    assert(scoped.is_success());
    const auto scope = scoped.value();
    const auto resolved = scope->ResolveResultInterfaceModel();
    assert(resolved.is_success());
    auto model = resolved.value();
    boundary(model);
    const auto number = model.Number(Result<std::int64_t,ReadError>::Success(7));
    assert(number.is_success() && number.value().is_success());
    const auto text = model.Text(Result<std::string,ReadError>::Failure(error(42)));
    assert(text.is_success() && text.value().is_failure());
    exact(text.value(), 42);
    const auto nothing = model.Nothing(Result<void,ReadError>::Cancelled(error(43)));
    assert(nothing.is_success() && nothing.value().is_cancelled());
    exact(nothing.value(), 43);
    assert(model.NumberMessage(Result<std::int64_t,ReadError>::Failure(error(42))).value() == "version conflict");
    assert(model.TextMessage(Result<std::string,ReadError>::Cancelled(error(43))).value() == "version conflict");
    assert(model.Retain(Result<std::int64_t,ReadError>::Success(19)).value() == 19);
    assert(model.Snapshot().value() && model.Identity().value());
    const auto outcomes = model.Outcomes();
    assert(outcomes.is_success() && outcomes.value().count() == 3);
    assert(outcomes.value().at(0).is_success());
    assert(outcomes.value().at(1).is_failure() && outcomes.value().at(1).error().request == 42);
    assert(outcomes.value().at(2).is_cancelled() && outcomes.value().at(2).error().request == 43);
    assert(model.CountFailures(outcomes.value()).value() == 1);
    const auto temporary = model.Temporary();
    assert(temporary.is_success());
    const auto escaped = temporary.value();
    assert(scope->Close().is_success());
    assert(root->Close().is_success());
    assert(escaped.is_failure());
    exact(escaped, 41);
}
