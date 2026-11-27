// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pipelang/model/object_model.hpp>

namespace execution_test {
struct CounterTag {
    static constexpr std::uint64_t typeId = 1;
    static bool accepts(std::uint64_t value) { return value == typeId; }
};

class CounterState final : public pipelang::model::ObjectState {
public:
    std::int64_t value = 0;
    std::unique_ptr<ObjectState> clone() const override {
        return std::make_unique<CounterState>(*this);
    }
    void trace(pipelang::model::ObjectTrace&) const override {}
    std::size_t logicalBytes() const override { return sizeof(CounterState); }
    void validate() const override {}
};
}
