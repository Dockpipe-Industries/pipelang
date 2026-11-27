// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SyncHost.hpp"
#include <functional>

namespace pure_host_tests {
class PureOwnedMathProbe final : public pipelang_sync_host::PureOwnedMath {
public:
    mutable int calls = 0;
    std::function<void()> callback;
    pipelang_sync_host::PureOwnedRecord record;
    pipelang_sync_host::PureOwnedKind kind = pipelang_sync_host::PureOwnedKind::First;
    pipelang::model::Optional<float> maybe;

    float Support(float compression, float relativeSpeed) const override {
        (void)relativeSpeed;
        ++calls;
        if (callback)
            callback();
        return compression;
    }
    pipelang_sync_host::PureOwnedRecord ReadRecord() const override { return record; }
    pipelang_sync_host::PureOwnedKind ReadKind() const override { return kind; }
    pipelang::model::Optional<float> ReadMaybe() const override { return maybe; }
    float FromRecord(pipelang_sync_host::PureOwnedRecord value) const override {
        ++calls;
        return value.value;
    }
    pipelang::model::Result<float, std::string> Fallible() const override {
        return pipelang::model::Result<float, std::string>::Failure("expected rejection");
    }
};
}
