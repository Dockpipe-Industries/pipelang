// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang/model/collection.hpp>
#include <pipelang/model/native_host_value.hpp>

namespace pipelang::native_app_v19 {

struct NativeConcurrentValue {
    HostedValue value;
    model::ListOwnership ownership;
};

inline std::size_t ownedBytes(const NativeConcurrentValue& value) noexcept { return value.ownership.bytes; }
inline std::size_t ownedElements(const NativeConcurrentValue& value) noexcept { return value.ownership.elements; }

}
