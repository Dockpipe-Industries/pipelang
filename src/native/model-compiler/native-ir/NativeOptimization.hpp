// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ScalarFunction.hpp"

#include <pipelang/model/result.hpp>

#include <string_view>
#include <unordered_map>

namespace pipelang::models::native_ir {

inline std::optional<std::int64_t> evaluateLiteralInteger(IntegerOperation operation,
                                                          std::int64_t left, std::int64_t right)
{
    try {
        switch (operation) {
        case IntegerOperation::Add: return model::add(left, right);
        case IntegerOperation::Subtract: return model::subtract(left, right);
        case IntegerOperation::Multiply: return model::multiply(left, right);
        case IntegerOperation::Divide: return model::divide(left, right);
        case IntegerOperation::Remainder: return model::remainder(left, right);
        }
    } catch (const model::ExecutionFailure&) {
        // A fault belongs to the original instruction and its source handler.
        return std::nullopt;
    }
    throw std::logic_error("unknown checked integer operation");
}

inline void foldLiteralIntegerOperations(ScalarFunction& function)
{
    for (auto& block : function.blocks) {
        std::unordered_map<std::size_t, std::int64_t> constants;
        const auto integer = [&](const Operand& operand) -> std::optional<std::int64_t> {
            if (const auto* immediate = std::get_if<std::int64_t>(&operand.value))
                return *immediate;
            if (const auto* reg = std::get_if<Register>(&operand.value)) {
                const auto found = constants.find(reg->index);
                if (found != constants.end())
                    return found->second;
            }
            return std::nullopt;
        };

        for (auto& instruction : block.instructions) {
            auto* checked = std::get_if<CheckedInteger>(&instruction.operation);
            if (!checked)
                continue;
            const auto left = integer(checked->left);
            const auto right = integer(checked->right);
            if (!left || !right)
                continue;
            const auto folded = evaluateLiteralInteger(checked->operation, *left, *right);
            if (!folded)
                continue;

            constants.emplace(checked->result.index, *folded);
            checked->knownValue = folded;
        }
    }
}

inline void foldLiteralConcatenation(ScalarFunction& function)
{
    for (auto& block : function.blocks) {
        std::unordered_map<std::size_t, std::string_view> literals;
        for (auto& instruction : block.instructions) {
            const auto* call = std::get_if<TextCall>(&instruction.operation);
            if (call && call->operation == TextOperation::Concatenate) {
                const auto* leftRegister = std::get_if<Register>(&call->arguments[0].value);
                const auto* rightRegister = std::get_if<Register>(&call->arguments[1].value);
                if (leftRegister && rightRegister) {
                    const auto left = literals.find(leftRegister->index);
                    const auto right = literals.find(rightRegister->index);
                    if (left != literals.end() && right != literals.end() &&
                        right->second.size() <= model::maximumStringBytes - left->second.size()) {
                        auto bytes = model::concatenate(left->second, right->second);
                        instruction.operation = TextLiteral{call->result, std::move(bytes)};
                    }
                }
            }

            // Earlier instructions own these validated strings for the pass.
            // Budget instructions remain in their original evaluation order.
            if (const auto* literal = std::get_if<TextLiteral>(&instruction.operation))
                literals.emplace(literal->result.index, literal->bytes);
        }
    }
}

}
