// SPDX-License-Identifier: Apache-2.0
#include "pipelang_crypto.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

int main() {
    try {
        if (pipelang_crypto_abi_version() != 1)
            throw std::runtime_error("Unexpected crypto ABI");
        std::mt19937 generator(0x43414249);
        for (std::size_t trial = 0; trial < 4096; ++trial) {
            std::vector<unsigned char> input(trial);
            for (auto& value : input)
                value = static_cast<unsigned char>(generator());
            std::array<unsigned char, 34> guarded;
            guarded.fill(0xa5);
            if (pipelang_crypto_v1_sha256(input.data(), input.size(), guarded.data() + 1, 32) != 0)
                throw std::runtime_error("Valid native input was refused");
            if (guarded.front() != 0xa5 || guarded.back() != 0xa5)
                throw std::runtime_error("Output ownership boundary exceeded");
            const auto retained = guarded;
            if (pipelang_crypto_v1_sha256(input.data(), input.size(), guarded.data() + 1, 33) != 1 || guarded != retained)
                throw std::runtime_error("Invalid output extent changed memory");
        }
        if (pipelang_crypto_v1_sha256(nullptr, 1, nullptr, 32) != 1)
            throw std::runtime_error("Null native buffers admitted");
        std::cout << "Crypto ABI1 deterministic boundary fuzz and ownership checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
