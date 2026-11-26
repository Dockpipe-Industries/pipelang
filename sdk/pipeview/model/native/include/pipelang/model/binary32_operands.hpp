// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

namespace pipelang::model {
// Aggregate list-initialization evaluates first before second, including abrupt
// completion. Do not replace this with two ordinary function arguments.
struct Binary32Operands {
    float first;
    float second;
};
}
