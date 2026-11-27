// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cassert>
#include <cstdint>
#include <string>

namespace result_interface_checks {
using namespace result_interface;
using pipelang::model::IResult;
using pipelang::model::Result;

inline ReadError error(std::int64_t request) {
    ReadError value;
    value.code = ReadCode::Conflict;
    value.message = "version conflict";
    value.origin = "reader";
    value.request = request;
    return value;
}

inline void exact(const IResult<ReadError>& outcome, std::int64_t request) {
    assert(!outcome.is_success());
    const auto& value = outcome.error();
    assert(value.code == ReadCode::Conflict);
    assert(value.message == "version conflict");
    assert(value.origin == "reader" && value.request == request);
}

template<class Model>
void boundary(Model& model) {
    const auto failed = IResult<ReadError>::From(Result<std::int64_t,ReadError>::Failure(error(44)));
    const auto described = model.Describe(failed);
    assert(described.is_success() && described.value() == "version conflict");
    const auto owned = model.Owned(Result<std::int64_t,ReadError>::Cancelled(error(45)));
    assert(owned.is_success() && owned.value().is_cancelled());
    exact(owned.value(), 45);
    const auto empty = model.Describe(IResult<ReadError>{});
    assert(empty.is_failure() && empty.error() == "result interface is empty");
}
}
