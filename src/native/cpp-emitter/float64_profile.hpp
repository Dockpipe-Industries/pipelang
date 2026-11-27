#pragma once

#include "core_json.hpp"

namespace pipelang::native {

inline constexpr char float64Profile[] = "pipelang.cpp.float64-comparison.v1";
inline constexpr char float64RequestContract[] = "pipelang.cpp-emitter.request.v2";

inline constexpr char float64ConditionalProfile[] = "pipelang.cpp.float64-conditional.v1";
inline constexpr char float64ConditionalRequestContract[] = "pipelang.cpp-emitter.request.v3";

inline constexpr char float64CallsProfile[] = "pipelang.cpp.float64-calls.v1";
inline constexpr char float64CallsRequestContract[] = "pipelang.cpp-emitter.request.v4";

// This selected wire projection contains only an already Core-admitted closed
// comparison subset. It is independently checked before any output is emitted.
Json generateFloat64Comparison(const Json& request);
Json generateFloat64Conditional(const Json& request);
Json generateFloat64Calls(const Json& request);

}
