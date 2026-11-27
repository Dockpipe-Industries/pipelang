// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ConnectionState.hpp"
#include "Failure.hpp"
#include <pipelang/model/runtime.hpp>
#include <chrono>

namespace pipelang::sqlite {
class Operation final {
public:
    Operation(ConnectionState& state, pipelang::model::CancellationToken cancellation);
    ~Operation();
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    void Check() const;
    void CheckCode(int code) const;
private:
    static int Progress(void* context) noexcept;
    static int Busy(void* context, int previousCalls) noexcept;
    ConnectionState& state_;
    pipelang::model::CancellationToken cancellation_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point deadline_;
    std::int64_t steps_ = 0;
};
}
