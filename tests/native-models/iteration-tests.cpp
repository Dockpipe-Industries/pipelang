// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Iteration.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
using namespace pipelang_models;
using pipelang::model::ExecutionFailure;
using pipelang::model::List;

int main() {
    IterationModel model;
    assert(model.Seed().is_success());
    assert(model.SnapshotIteration().value() == 36);
    assert(model.Size() == 6);
    assert(model.ReadFirst() == 1);
    assert(model.ControlFlow() == 12);
    const auto before = model.snapshot();
    assert(model.IndexRollback().is_failure());
    assert(model.snapshot() == before);
    assert(model.BudgetRollback().is_failure());
    assert(model.snapshot() == before);
    bool stopped = false;
    try {
        model.PureBudget();
    } catch (const ExecutionFailure&) {
        stopped = true;
    }
    assert(stopped);
    // A failed invocation must release/reset its budget for the next invocation.
    assert(model.ControlFlow() == 12);

    auto values = List<std::int64_t>::Empty();
    for (int index = 0; index < 4096; ++index)
        values.add(index);
    bool capped = false;
    try {
        values.add(4096);
    } catch (const ExecutionFailure&) {
        capped = true;
    }
    assert(capped && values.count() == 4096);
    auto nested = List<List<std::int64_t>>::Empty();
    capped = false;
    try {
        nested.add(values);
    } catch (const ExecutionFailure&) {
        capped = true;
    }
    assert(capped && nested.count() == 0);
    auto envelopes = List<IterationEnvelope>::Empty();
    capped = false;
    try {
        envelopes.add(IterationEnvelope{pipelang::model::Optional<
            pipelang::model::Result<List<std::int64_t>, std::string>>::Some(
                pipelang::model::Result<List<std::int64_t>, std::string>::Success(values))});
    } catch (const ExecutionFailure&) {
        capped = true;
    }
    assert(capped && envelopes.count() == 0);
    auto text = List<std::string>::Empty();
    text.add("saved");
    capped = false;
    try {
        text.add(std::string(1024 * 1024, 'x'));
    } catch (const ExecutionFailure&) {
        capped = true;
    }
    assert(capped && text.count() == 1 && text.at(0) == "saved");
    bool malformed = false;
    try {
        text.add(std::string("\xff", 1));
    } catch (const ExecutionFailure&) {
        malformed = true;
    }
    assert(malformed && text.count() == 1);
}
