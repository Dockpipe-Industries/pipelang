// SPDX-License-Identifier: Apache-2.0
#include "Hashing.hpp"
#include "WorkspaceError.hpp"
#include <pipelang_crypto.h>
#include <array>

namespace pipelang::workspace {
std::string Hashing::bytes(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > 1024 * 1024)
        throw WorkspaceError("limit");
    std::array<unsigned char, 32> digest{};
    if (pipelang_crypto_abi_version() != 1 ||
        pipelang_crypto_v1_sha256(bytes.data(), bytes.size(), digest.data(), digest.size()) != 0)
        throw WorkspaceError("hash_failed");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
std::string Hashing::text(const std::string& text) {
    return bytes(std::vector<std::uint8_t>(text.begin(), text.end()));
}
}
