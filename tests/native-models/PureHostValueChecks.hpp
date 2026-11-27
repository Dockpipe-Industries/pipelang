// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PureOwnedMathProbe.hpp"
#include <bit>
#include <cassert>
#include <cfenv>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace pure_host_tests {
template<class Callback>
inline void failure(Callback callback, const std::string& message) {
    bool refused = false;
    try {
        callback();
    } catch (const pipelang::model::ExecutionFailure& error) {
        refused = error.what() == message;
    }
    assert(refused);
}
inline void valuesAndValidation() {
    auto host = std::make_shared<PureOwnedMathProbe>();
    pipelang_sync_host::PureHostOwner model(host);
    for (const auto word : {UINT32_C(0), UINT32_C(0x80000000), UINT32_C(0x7fc01234), UINT32_C(1)}) {
        const auto value = std::bit_cast<float>(word);
        assert(std::bit_cast<std::uint32_t>(model.Read(value)) == word);
    }
    host->record = {3.5f, "owned record", pipelang_sync_host::PureOwnedKind::Second};
    const auto published = model.ReadRecord();
    host->record.label = "host edit";
    assert(published.label == "owned record" && published.value == 3.5f);
    assert(model.FromRecord(published) == 3.5f);
    auto invalidArgument = published;
    invalidArgument.label = std::string("\xff", 1);
    const auto beforeArgument = host->calls;
    failure([&] { (void)model.FromRecord(invalidArgument); }, "string contains invalid UTF-8");
    assert(host->calls == beforeArgument);
    assert(model.ReadKind() == pipelang_sync_host::PureOwnedKind::First);
    host->kind = static_cast<pipelang_sync_host::PureOwnedKind>(99);
    failure([&] { (void)model.ReadKind(); }, "invalid enum value");
    host->kind = pipelang_sync_host::PureOwnedKind::First;
    assert(!model.ReadMaybe().has_value());
    host->maybe = pipelang::model::Optional<float>::Some(-0.0f);
    assert(std::bit_cast<std::uint32_t>(model.ReadMaybe().value()) == UINT32_C(0x80000000));
    host->record.label = std::string("\xff", 1);
    failure([&] { (void)model.ReadRecord(); }, "string contains invalid UTF-8");
    host->record.label = "recovered";
    assert(model.ReadRecord().label == "recovered");
    const auto fallible = model.Fallible();
    assert(fallible.is_failure() && fallible.error() == "expected rejection");
    const auto beforeOrder = host->calls;
    assert(model.Ordered(7.0f) == 7.0f && host->calls == beforeOrder + 2);
}
inline void faultsAndCancellation() {
    auto host = std::make_shared<PureOwnedMathProbe>();
    pipelang_sync_host::PureHostOwner model(host);
    const auto before = model.snapshot();
    host->callback = [] { throw std::runtime_error("native pure fault"); };
    failure([&] { (void)model.Read(1.0f); }, "native pure fault");
    const auto failed = model.Checked(1.0f);
    assert(failed.is_failure() && failed.error() == "native pure fault" && model.snapshot() == before);
    host->callback = [] { throw 17; };
    failure([&] { (void)model.Read(1.0f); }, "unexpected synchronous host service failure");
    const auto unknown = model.Checked(1.0f);
    assert(unknown.is_failure() && unknown.error() == "unexpected synchronous host service failure"
        && model.snapshot() == before);
    host->callback = [] { throw pipelang::model::OperationCancelled("pure host cancelled"); };
    bool directCancelled = false;
    try {
        (void)model.Read(1.0f);
    } catch (const pipelang::model::OperationCancelled& error) {
        directCancelled = std::string(error.what()) == "pure host cancelled";
    }
    assert(directCancelled);
    const auto cancelled = model.Checked(1.0f);
    assert(cancelled.is_cancelled() && cancelled.error() == "pure host cancelled" && model.snapshot() == before);
    host->callback = {};
    assert(model.Checked(2.0f).is_success());
}
inline void ownerAndReentry() {
    auto host = std::make_shared<PureOwnedMathProbe>();
    auto otherHost = std::make_shared<PureOwnedMathProbe>();
    pipelang_sync_host::PureHostOwner model(host);
    pipelang_sync_host::PureHostOwner other(otherHost);
    host->callback = [&] {
        const auto message = "model reentry during a synchronous host callback";
        failure([&] { (void)model.Read(1.0f); }, message);
        failure([&] { (void)model.Checked(1.0f); }, message);
        failure([&] { (void)model.snapshot(); }, message);
        assert(other.Read(9.0f) == 9.0f);
    };
    assert(model.Read(3.0f) == 3.0f);
    host->callback = {};
    const auto calls = host->calls;
    std::thread foreign([&] {
        failure([&] { (void)model.Read(1.0f); }, "model accessed from a different owner thread");
    });
    foreign.join();
    assert(host->calls == calls && model.Read(4.0f) == 4.0f);
}
inline void floatingEnvironment() {
    auto host = std::make_shared<PureOwnedMathProbe>();
    pipelang_sync_host::PureHostOwner model(host);
    const auto before = model.snapshot();
    const auto rounding = std::fegetround();
    host->callback = [] { assert(std::fesetround(FE_DOWNWARD) == 0); };
    const auto disturbed = model.Checked(1.0f);
    assert(disturbed.is_failure() && disturbed.error() == "float32 requires round-to-nearest floating mode");
    assert(model.snapshot() == before && std::fegetround() == FE_DOWNWARD);
    const auto calls = host->calls;
    failure([&] { (void)model.Read(1.0f); }, "float32 requires round-to-nearest floating mode");
    assert(host->calls == calls);
    assert(std::fesetround(rounding) == 0);
    for (const bool cancel : {false, true}) {
        host->callback = [cancel] {
            assert(std::fesetround(FE_DOWNWARD) == 0);
            if (cancel)
                throw pipelang::model::OperationCancelled("cancelled with disturbed FP");
            throw std::runtime_error("fault with disturbed FP");
        };
        const auto primary = model.Checked(1.0f);
        assert((cancel ? primary.is_cancelled() : primary.is_failure())
            && primary.error() == (cancel ? "cancelled with disturbed FP" : "fault with disturbed FP"));
        assert(model.snapshot() == before && std::fegetround() == FE_DOWNWARD);
        assert(std::fesetround(rounding) == 0);
    }
    host->callback = {};
    assert(model.Read(5.0f) == 5.0f && model.Checked(5.0f).is_success());
}
inline void run() {
    valuesAndValidation();
    faultsAndCancellation();
    ownerAndReentry();
    floatingEnvironment();
}
}
