// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "TypedHostErrors.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "TypedHostChecks.hpp"

int main() {
    using namespace typed_host_checks;
    auto host = std::make_shared<Host>();
    TypedHostErrors model(host);
    outcomes(model, host);
    assert(model.Calls() == 4 && model.Cleanups() == 4);
    const auto snapshot = model.snapshot();
    assert(snapshot.calls == 4 && snapshot.cleanup == 4);
    const auto original = ReadResult::Cancelled(domainError(91));
    const auto preserved = model.Preserve(original);
    static_assert(std::is_same_v<std::remove_cv_t<decltype(preserved)>, ReadResult>);
    assert(preserved.is_cancelled());
    checkError(preserved.error(), 91);
}
