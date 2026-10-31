// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "support.hpp"
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace pipelang::tools {
namespace {
constexpr std::array<std::uint32_t, 64> constants = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
std::uint32_t rotate(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
}

class Digest {
public:
    void update(std::string_view input) {
        require(input.size() <= (UINT64_MAX / 8) - bytes_, "SHA256 input exceeds its capacity");
        bytes_ += input.size();
        for (unsigned char value : input) {
            pending_[used_++] = value;
            if (used_ == pending_.size()) {
                block();
                used_ = 0;
            }
        }
    }
    std::string finish() {
        const auto bits = bytes_ * 8;
        pending_[used_++] = 0x80;
        if (used_ > 56) {
            while (used_ < 64)
                pending_[used_++] = 0;
            block();
            used_ = 0;
        }
        while (used_ < 56)
            pending_[used_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8)
            pending_[used_++] = static_cast<unsigned char>(bits >> shift);
        block();
        std::ostringstream result;
        result << std::hex << std::setfill('0');
        for (auto word : state_)
            result << std::setw(8) << word;
        return result.str();
    }
private:
    void block() {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4;
            words[index] = (std::uint32_t(pending_[offset]) << 24) |
                (std::uint32_t(pending_[offset + 1]) << 16) |
                (std::uint32_t(pending_[offset + 2]) << 8) | pending_[offset + 3];
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto first = rotate(words[index - 15], 7) ^ rotate(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const auto second = rotate(words[index - 2], 17) ^ rotate(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + first + words[index - 7] + second;
        }
        auto working = state_;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum = rotate(working[4], 6) ^ rotate(working[4], 11) ^ rotate(working[4], 25);
            const auto choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
            const auto first = working[7] + sum + choice + constants[index] + words[index];
            const auto upper = rotate(working[0], 2) ^ rotate(working[0], 13) ^ rotate(working[0], 22);
            const auto majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            for (std::size_t position = 7; position > 0; --position)
                working[position] = working[position - 1];
            working[4] += first;
            working[0] = first + upper + majority;
        }
        for (std::size_t index = 0; index < state_.size(); ++index)
            state_[index] += working[index];
    }
    std::array<std::uint32_t, 8> state_{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<unsigned char, 64> pending_{};
    std::size_t used_ = 0;
    std::uint64_t bytes_ = 0;
};
}

std::string sha256(std::string_view bytes) {
    Digest digest;
    digest.update(bytes);
    return digest.finish();
}

std::string fileHash(const fs::path& path) {
    regularFile(path);
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open hash input: " + path.string());
    Digest digest;
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0)
        digest.update(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount())));
    require(input.eof(), "failed to read hash input: " + path.string());
    return digest.finish();
}
}
