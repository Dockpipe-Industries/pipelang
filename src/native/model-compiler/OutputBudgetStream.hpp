// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResourceBudget.hpp"
#include <streambuf>

namespace pipelang::models {
// Count JSON serialization without materializing another output-sized string.
class OutputBudgetStream final : public std::streambuf {
    std::size_t limit_;
    std::size_t bytes_ = 0;
    bool exceeded_ = false;
protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        if (count < 0 || budgetExceeded(bytes_, static_cast<std::size_t>(count), limit_)) {
            exceeded_ = true;
            return 0;
        }
        bytes_ += static_cast<std::size_t>(count);
        return count;
    }
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof()))
            return traits_type::not_eof(character);
        if (budgetExceeded(bytes_, 1, limit_)) {
            exceeded_ = true;
            return traits_type::eof();
        }
        ++bytes_;
        return character;
    }
public:
    explicit OutputBudgetStream(std::size_t limit) : limit_(limit) {}
    bool exceeded() const noexcept { return exceeded_; }
};
}
