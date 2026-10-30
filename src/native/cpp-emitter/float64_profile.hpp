#pragma once

#include "core_json.hpp"

namespace pipelang::native {

inline constexpr char float64Profile[] = "pipelang.cpp.float64-comparison.v1";
inline constexpr char float64RequestContract[] = "pipelang.cpp-emitter.request.v2";

// This selected wire projection contains only an already Core-admitted closed
// comparison subset. It is independently checked before any output is emitted.
Json generateFloat64Comparison(const Json& request);

}
