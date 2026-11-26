// SPDX-License-Identifier: Apache-2.0
#include "Uuid.hpp"
#include "WorkspaceError.hpp"
#include <sys/random.h>
#include <array>
#include <cerrno>

namespace pipelang::workspace {
std::string Uuid::create() {
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, GRND_NONBLOCK);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw WorkspaceError("random_unavailable");
        offset += static_cast<std::size_t>(count);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 15) | 64);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 63) | 128);
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            result.push_back('-');
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 15]);
    }
    return result;
}
}
