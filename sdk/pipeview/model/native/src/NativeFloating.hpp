// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

namespace pipelang::native_app_v19 {

using FloatingEvaluation = std::uint64_t (*)(FloatingBuiltin, bool, std::uint64_t, std::uint64_t, std::uint64_t);

std::int32_t evaluateInInvocation(void* frame, FloatingEvaluation evaluate, FloatingBuiltin operation,
                                  bool binary32, std::uint64_t first, std::uint64_t second, std::uint64_t third,
                                  std::uint64_t* output) noexcept;

}
