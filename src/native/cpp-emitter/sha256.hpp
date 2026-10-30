#pragma once

#include <string>

namespace pipelang::native {

// Deterministic symbol names use the first twelve bytes of SHA-256.
std::string symbolName(const std::string& prefix, const std::string& value);

}
