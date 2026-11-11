// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeNumericExports.hpp"

#include <algorithm>
#include <charconv>
#include <bit>
#include <cmath>
#include <limits>
#include <pipelang/model/collection.hpp>
#include <pipelang/model/limits.hpp>

namespace pipelang::models {
NativeNumericExports::NativeNumericExports(const Program& program, const Declaration& owner, bool observation)
    : program_(program), owner_(&owner), observation_(observation), remaining_(pipelang::model::defaultEvaluationSteps)
{
}

bool NativeNumericExports::eligibleOwner(const Declaration& owner) const
{
    if (!owner.modelExported || owner.generatedObjectFacet || !owner.fields.empty() ||
        !owner.baseClass.empty() || !owner.interfaces.empty() || !owner.typeParameters.empty() ||
        !owner.genericSourceName.empty())
        return false;
    return std::none_of(owner.methods.begin(), owner.methods.end(), [](const Method& candidate) {
        return candidate.constructor && (!candidate.body.empty() || !candidate.parameters.empty());
    });
}

bool NativeNumericExports::enumeration(const Type& type) const
{
    return type.arguments.empty() && std::any_of(program_.declarations.begin(), program_.declarations.end(), [&](const Declaration& candidate) {
        return candidate.name == type.name && candidate.kind == "Enum";
    });
}

bool NativeNumericExports::scalar(const Type& type) const
{
    if (observation_ && (type.name == "float" || type.name == "float32"))
        return false;
    return type.arguments.empty() &&
        (type.name == "bool" || type.name == "int" || type.name == "uint32" || type.name == "float" ||
         type.name == "float32" || enumeration(type));
}

bool NativeNumericExports::floatingConversion(const Expr& node) const
{
    if (node->kind != "call" || node->resolution != "numeric_conversion" || node->children.size() != 2)
        return false;
    const auto& target = node->children.front();
    if (target->kind != "member" || target->children.size() != 1 ||
        target->children.front()->kind != "type" || target->children.front()->type != node->type)
        return false;
    const auto& input = node->children.at(1)->type;
    if (!input.arguments.empty() || !node->type.arguments.empty())
        return false;
    if (standaloneHeader_) {
        // The explicit caller-host projection preserves native compiler folding,
        // contraction and rounding. Narrowing remains outside this safe subset.
        return (target->text == "FromInt" && input.name == "int" &&
                (node->type.name == "float" || node->type.name == "float32")) ||
            (target->text == "FromFloat32" && node->type.name == "float" && input.name == "float32");
    }
    if (program_.activeHostNumericEnvironment) {
        // Signed integers fit both floating domains. The selected native facet
        // converts in the active host environment, without changing controls.
        return (target->text == "FromInt" && input.name == "int" &&
                (node->type.name == "float" || node->type.name == "float32")) ||
            (target->text == "FromFloat32" && node->type.name == "float" && input.name == "float32") ||
            (target->text == "FromFloat" && node->type.name == "float32" && input.name == "float");
    }
    // These existing runtime conversions are total IEEE value operations in
    // the caller-admitted floating environment. Integer and unsigned conversion
    // tags retain their separate potentially throwing/environment contracts.
    return (target->text == "FromFloat32" && node->type.name == "float" && input.name == "float32") ||
        (target->text == "FromFloat" && node->type.name == "float32" && input.name == "float");
}

bool NativeNumericExports::spend()
{
    if (remaining_ == 0)
        return false;
    --remaining_;
    return true;
}

bool NativeNumericExports::spendValue(const Type& type)
{
    if (scalar(type))
        return spendValue(type, true);
    std::set<std::string> records;
    return valueBounds(type, records).has_value() && spendValue(type, true);
}

std::optional<NativeNumericExports::ValueBounds> NativeNumericExports::valueBounds(
    const Type& type, std::set<std::string>& records)
{
    constexpr std::size_t maximumElements = pipelang::model::maximumListElements;
    constexpr std::size_t maximumBytes = pipelang::model::maximumListBytes;
    const auto add = [](std::size_t left, std::size_t right, std::size_t cap) {
        return pipelang::model::boundedOwnedSum(left, right, cap);
    };
    const auto multiply = [](std::size_t left, std::size_t right, std::size_t cap) {
        if (left > cap || right > cap || (right != 0 && left > cap / right))
            return cap + 1;
        return left * right;
    };
    const auto admitted = [&](const ValueBounds& bounds) {
        return bounds.footprint <= maximumElements && bounds.ownedElements <= maximumElements &&
            bounds.storageBytes <= maximumBytes && bounds.ownedBytes <= maximumBytes;
    };
    if (!spend())
        return std::nullopt;
    // Eight-byte slots conservatively bound every admitted scalar's storage,
    // including enums. Each aggregate bound stays aligned to this slot size.
    if (scalar(type))
        return ValueBounds{1, 0, 8, 8};
    if (type.name == "void" && type.arguments.empty())
        return ValueBounds{0, 0, 0, 0};
    if (type.name == "FixedArray") {
        if (type.arguments.size() != 1 || !type.fixedExtent || *type.fixedExtent > maximumElements)
            return std::nullopt;
        // Traverse even zero extents: empty storage cannot hide a dynamic type,
        // a cycle, or an independently over-budget element shape.
        const auto element = valueBounds(type.arguments.front(), records);
        if (!element)
            return std::nullopt;
        const auto extent = *type.fixedExtent;
        ValueBounds bounds{
            std::max<std::size_t>(1, multiply(extent, element->footprint, maximumElements)),
            add(extent, multiply(extent, element->ownedElements, maximumElements), maximumElements),
            std::max<std::size_t>(8, multiply(extent, element->storageBytes, maximumBytes)),
            0
        };
        bounds.ownedBytes = add(bounds.storageBytes,
            multiply(extent, element->ownedBytes, maximumBytes), maximumBytes);
        return admitted(bounds) ? std::optional(bounds) : std::nullopt;
    }
    if (type.name == "Optional" && type.arguments.size() == 1) {
        const auto payload = valueBounds(type.arguments.front(), records);
        if (!payload)
            return std::nullopt;
        ValueBounds bounds{add(payload->footprint, 1, maximumElements), payload->ownedElements,
            add(payload->storageBytes, 8, maximumBytes), 0};
        bounds.ownedBytes = add(bounds.storageBytes, payload->ownedBytes, maximumBytes);
        return admitted(bounds) ? std::optional(bounds) : std::nullopt;
    }
    if (type.name == "Result" && type.arguments.size() == 2) {
        const auto payload = valueBounds(type.arguments.front(), records);
        const auto error = valueBounds(type.arguments.back(), records);
        if (!payload || !error)
            return std::nullopt;
        ValueBounds bounds{
            add(add(payload->footprint, error->footprint, maximumElements), 1, maximumElements),
            add(payload->ownedElements, error->ownedElements, maximumElements),
            add(add(payload->storageBytes, error->storageBytes, maximumBytes), 24, maximumBytes),
            0
        };
        bounds.ownedBytes = add(bounds.storageBytes,
            add(payload->ownedBytes, error->ownedBytes, maximumBytes), maximumBytes);
        return admitted(bounds) ? std::optional(bounds) : std::nullopt;
    }
    if (!type.arguments.empty())
        return std::nullopt;
    const auto record = std::find_if(program_.declarations.begin(), program_.declarations.end(), [&](const Declaration& candidate) {
        return candidate.name == type.name && candidate.kind == "Record";
    });
    if (record == program_.declarations.end() || !records.insert(type.name).second)
        return std::nullopt;
    ValueBounds bounds{0, 0, 0, 0};
    for (const auto& field : record->fields) {
        const auto child = valueBounds(field.type, records);
        if (!child) {
            records.erase(type.name);
            return std::nullopt;
        }
        bounds.footprint = add(bounds.footprint, child->footprint, maximumElements);
        bounds.ownedElements = add(bounds.ownedElements, child->ownedElements, maximumElements);
        bounds.storageBytes = add(bounds.storageBytes, child->storageBytes, maximumBytes);
        bounds.ownedBytes = add(bounds.ownedBytes, child->ownedBytes, maximumBytes);
        if (!admitted(bounds)) {
            records.erase(type.name);
            return std::nullopt;
        }
    }
    records.erase(type.name);
    // Empty values still occupy material storage in an enclosing fixed array.
    bounds.footprint = std::max<std::size_t>(1, bounds.footprint);
    bounds.storageBytes = std::max<std::size_t>(8, bounds.storageBytes);
    bounds.ownedBytes = add(bounds.storageBytes, bounds.ownedBytes, maximumBytes);
    return admitted(bounds) ? std::optional(bounds) : std::nullopt;
}

bool NativeNumericExports::spendValue(const Type& type, bool allowResult)
{
    // Checked record layouts already satisfy the 256-node/32-depth bounds.
    // Charge every transitive visit as well; shared shapes cannot hide work.
    if (!spend())
        return false;
    if (scalar(type)) {
        if (type.name == "float" || type.name == "float32")
            projection_.constantEvaluable = false;
        if (enumeration(type))
            projection_.valueDeclarations.insert(type.name);
        if (type.name == "float32")
            projection_.requiresBinary32Environment = true;
        return true;
    }
    if (type.name == "Optional")
        return type.arguments.size() == 1 && spendValue(type.arguments.front(), false);
    if (type.name == "FixedArray") {
        if (type.arguments.size() != 1 || !type.fixedExtent ||
            *type.fixedExtent > pipelang::model::maximumListElements)
            return false;
        // Even an empty array must admit its element's owned closed shape.
        const auto visits = std::max<std::size_t>(1, *type.fixedExtent);
        for (std::size_t index = 0; index < visits; ++index) {
            if (!spendValue(type.arguments.front(), false))
                return false;
        }
        return true;
    }
    if (type.name == "Result") {
        if (!allowResult || type.arguments.size() != 2)
            return false;
        const auto& error = type.arguments[1];
        const auto record = std::find_if(program_.declarations.begin(), program_.declarations.end(), [&](const Declaration& candidate) {
            return candidate.name == error.name && candidate.kind == "Record";
        });
        if (!enumeration(error) && (record == program_.declarations.end() || !error.arguments.empty()))
            return false;
        // Results remain an outer outcome boundary. Records and Optional own
        // closed values, without acquiring nested Result or dynamic channels.
        return ((type.arguments[0].name == "void" && type.arguments[0].arguments.empty()) ||
                spendValue(type.arguments[0], false)) &&
            spendValue(error, false);
    }
    if (!type.arguments.empty())
        return false;
    const auto record = std::find_if(program_.declarations.begin(), program_.declarations.end(), [&](const Declaration& candidate) {
        return candidate.name == type.name;
    });
    if (record == program_.declarations.end() || record->kind != "Record" ||
        !record->typeParameters.empty() || !record->genericSourceName.empty() ||
        !activeValueDeclarations_.insert(type.name).second)
        return false;
    projection_.valueDeclarations.insert(type.name);
    bool admitted = !standaloneHeader_ || recordDefaults(type);
    if (admitted) {
        for (const auto& field : record->fields) {
            if (!spendValue(field.type, false)) {
                admitted = false;
                break;
            }
        }
    }
    activeValueDeclarations_.erase(type.name);
    return admitted;
}

bool NativeNumericExports::defaultValue(const Type& type)
{
    if (!spend())
        return false;
    // Optional's empty default does not construct its payload. Its complete
    // payload layout was nevertheless admitted by spendValue.
    if (scalar(type) || (type.name == "Optional" && type.arguments.size() == 1))
        return true;
    if (type.name == "FixedArray" && type.arguments.size() == 1 && type.fixedExtent) {
        for (std::size_t index = 0; index < *type.fixedExtent; ++index) {
            if (!defaultValue(type.arguments.front()))
                return false;
        }
        return true;
    }
    return recordDefaults(type);
}

bool NativeNumericExports::fixedDefaultExpression(const Expr& node)
{
    if (!node || !spend())
        return false;
    if (!node->convertTo.name.empty()) {
        const auto& target = node->convertTo;
        if (target.name != "Optional" || target.arguments.size() != 1)
            return false;
        if (node->kind == "null")
            return true;
        auto payload = std::make_shared<Expression>(*node);
        payload->convertTo = target.arguments.front() == node->type ? Type{} : target.arguments.front();
        return fixedDefaultExpression(payload);
    }
    if (node->kind == "integer" || node->kind == "uint32" || node->kind == "float" ||
        node->kind == "float32" || node->kind == "boolean" || node->resolution == "constant" ||
        node->resolution == "enum" || node->resolution == "ieee_constant" ||
        node->resolution == "minimum_int")
        return true;
    if (node->kind == "unary" && node->text == "-" && node->children.size() == 1)
        return fixedDefaultExpression(node->children.front());
    if (node->kind == "named_construct") {
        return std::all_of(node->children.begin(), node->children.end(), [&](const Expr& initializer) {
            return spend() && initializer->children.size() == 1 && fixedDefaultExpression(initializer->children.front());
        });
    }
    if (node->type.name == "FixedArray" && node->resolution == "collection_construct" &&
        node->children.size() == 2)
        return fixedDefaultExpression(node->children.at(1));
    if (node->kind != "call" || node->children.empty() ||
        (node->resolution != "construct" && node->resolution != "optional_construct"))
        return false;
    return std::all_of(node->children.begin() + 1, node->children.end(), [&](const Expr& argument) {
        return fixedDefaultExpression(argument);
    });
}

bool NativeNumericExports::recordDefaults(const Type& type)
{
    if (!spend() || !activeRecordDefaults_.insert(type.name).second)
        return false;
    const auto record = std::find_if(program_.declarations.begin(), program_.declarations.end(), [&](const Declaration& candidate) {
        return candidate.name == type.name;
    });
    if (record == program_.declarations.end() || record->kind != "Record") {
        activeRecordDefaults_.erase(type.name);
        return false;
    }
    // Record initializers have their own lexical context. Caller-local outcome
    // facts cannot establish safety inside a type's default expressions.
    const OutcomeFlow initializerFlow;
    bool admitted = true;
    for (const auto& field : record->fields) {
        if (!spend()) {
            admitted = false;
            break;
        }
        if (!field.initializer) {
            if (!defaultValue(field.type)) {
                admitted = false;
                break;
            }
            continue;
        }
        if (!observation_ && (program_.activeHostNumericEnvironment || standaloneHeader_) && field.initializer) {
            // Types.hpp emits record defaults through the ordinary facet. Only
            // fixed closed values can cross into an active-host closure until
            // computed default evaluation has its own active lowering.
            if (!fixedDefaultExpression(field.initializer)) {
                admitted = false;
                break;
            }
        }
        if (!expression(field.initializer, initializerFlow)) {
            admitted = false;
            break;
        }
    }
    activeRecordDefaults_.erase(type.name);
    return admitted;
}

NativeNumericExports::FloatingRange NativeNumericExports::joinFloating(
    const FloatingRange& first, const FloatingRange& second)
{
    FloatingRange joined;
    joined.minimum = std::min(first.minimum, second.minimum);
    joined.maximum = std::max(first.maximum, second.maximum);
    joined.minimumOpen = first.minimum == second.minimum ? first.minimumOpen && second.minimumOpen :
        (first.minimum < second.minimum ? first.minimumOpen : second.minimumOpen);
    joined.maximumOpen = first.maximum == second.maximum ? first.maximumOpen && second.maximumOpen :
        (first.maximum > second.maximum ? first.maximumOpen : second.maximumOpen);
    joined.mayNaN = first.mayNaN || second.mayNaN;
    return joined;
}

void NativeNumericExports::constrainFloating(FloatingRange& range, const std::string& comparison, double bound)
{
    if (comparison == ">" || comparison == ">=") {
        const bool open = comparison == ">";
        if (bound > range.minimum) {
            range.minimum = bound;
            range.minimumOpen = open;
        } else if (bound == range.minimum) {
            range.minimumOpen = range.minimumOpen || open;
        }
    } else {
        const bool open = comparison == "<";
        if (bound < range.maximum) {
            range.maximum = bound;
            range.maximumOpen = open;
        } else if (bound == range.maximum) {
            range.maximumOpen = range.maximumOpen || open;
        }
    }
}

void NativeNumericExports::storeFloating(OutcomeFlow& flow, const std::string& name, const FloatingRange& range)
{
    const FloatingRange unknown;
    if (range.minimum == unknown.minimum && range.maximum == unknown.maximum &&
        !range.minimumOpen && !range.maximumOpen && range.mayNaN)
        flow.floating.erase(name);
    else
        flow.floating[name] = range;
    const bool empty = range.minimum > range.maximum ||
        (range.minimum == range.maximum && (range.minimumOpen || range.maximumOpen));
    if (empty && !range.mayNaN)
        flow.reachable = false;
}

std::optional<NativeNumericExports::FloatingRange> NativeNumericExports::floatingRange(
    const Expr& node, const OutcomeFlow& flow)
{
    if (observation_ || !node || (node->type.name != "float" && node->type.name != "float32") || !spend())
        return std::nullopt;
    if (node->resolution == "constant")
        return floatingRange(node->constantValue, flow);
    if (node->kind == "name" && node->resolution == "local") {
        const auto found = flow.floating.find(node->text);
        return found == flow.floating.end() ? FloatingRange{} : found->second;
    }
    if (node->kind == "float" || node->kind == "float32") {
        std::uint64_t bits = 0;
        const auto parsed = std::from_chars(node->resolution.data(), node->resolution.data() + node->resolution.size(), bits);
        if (parsed.ec != std::errc{} || parsed.ptr != node->resolution.data() + node->resolution.size())
            return std::nullopt;
        double value;
        if (node->type.name == "float32") {
            // Decode into normal binary64 arithmetic: compiler DAZ must not
            // erase an authored binary32 subnormal bound during widening.
            const auto single = static_cast<std::uint32_t>(bits);
            const auto exponent = (single >> 23) & 255U;
            if (exponent == 255U)
                return FloatingRange{};
            const auto significand = (single & 0x7fffffU) | (exponent == 0 ? 0U : 0x800000U);
            value = std::ldexp(static_cast<double>(significand), exponent == 0 ? -149 : static_cast<int>(exponent) - 150);
            if ((single & 0x80000000U) != 0)
                value = -value;
        } else {
            value = std::bit_cast<double>(bits);
        }
        if (!std::isfinite(value))
            return FloatingRange{};
        return FloatingRange{value, value, false, false, false};
    }
    if (node->kind == "unary" && node->text == "-") {
        const auto operand = floatingRange(node->children.at(0), flow);
        if (!operand)
            return std::nullopt;
        return FloatingRange{-operand->maximum, -operand->minimum,
            operand->maximumOpen, operand->minimumOpen, operand->mayNaN};
    }
    if (floatingConversion(node) && node->type.name == "float" && node->children.at(1)->type.name == "float32")
        return floatingRange(node->children.at(1), flow);
    if (node->kind == "conditional") {
        OutcomeFlow whenTrue;
        OutcomeFlow whenFalse;
        if (!splitCondition(node->children.at(0), flow, whenTrue, whenFalse))
            return std::nullopt;
        const auto first = floatingRange(node->children.at(1), whenTrue);
        const auto second = floatingRange(node->children.at(2), whenFalse);
        if (!first || !second)
            return std::nullopt;
        if (!whenTrue.reachable)
            return second;
        if (!whenFalse.reachable)
            return first;
        return joinFloating(*first, *second);
    }
    if (node->kind == "call" && node->resolution == "float_math" &&
        node->children.front()->text == "Clamp" && node->children.size() == 4) {
        const auto value = floatingRange(node->children.at(1), flow);
        const auto minimum = floatingRange(node->children.at(2), flow);
        const auto maximum = floatingRange(node->children.at(3), flow);
        const auto finite = [](const auto& range) {
            return range && !range->mayNaN && std::isfinite(range->minimum) && std::isfinite(range->maximum);
        };
        // Independent intervals must prove ordering for every admitted input.
        // Unknown or overlapping bound ranges retain the checked model path.
        if (!finite(value) || !finite(minimum) || !finite(maximum) || minimum->maximum > maximum->minimum)
            return std::nullopt;
        // DAZ may compare distinct subnormal arguments as equal. Enclose that
        // neighborhood even when the nominal bounds exclude its values.
        const double normalFloor = node->type.name == "float32" ? 0x1p-126 : 0x1p-1022;
        double lower = minimum->minimum;
        double upper = maximum->maximum;
        if (lower <= normalFloor && upper >= -normalFloor) {
            lower = std::min(lower, -normalFloor);
            upper = std::max(upper, normalFloor);
        }
        return FloatingRange{lower, upper, false, false, false};
    }
    // Binary32 multiplication is bounded in wider binary64 arithmetic.
    // A deliberately broad relative-error enclosure plus
    // a minimum-normal floor covers every directed rounding and DAZ/FTZ.
    // All proof arithmetic stays normal binary64, including tiny products;
    // compiler floating controls cannot collapse subnormal binary32 bounds.
    // This is proof only: the emitted operation still observes the host state.
    if ((program_.activeHostNumericEnvironment || standaloneHeader_) && node->type.name == "float32" &&
        node->kind == "binary" && node->text == "*") {
        const auto left = floatingRange(node->children.at(0), flow);
        const auto right = floatingRange(node->children.at(1), flow);
        if (!left || !right || left->mayNaN || right->mayNaN ||
            !std::isfinite(left->minimum) || !std::isfinite(left->maximum) ||
            !std::isfinite(right->minimum) || !std::isfinite(right->maximum))
            return FloatingRange{};
        const double products[] = {0.0, left->minimum * right->minimum, left->minimum * right->maximum,
            left->maximum * right->minimum, left->maximum * right->maximum};
        const auto [minimum, maximum] = std::minmax_element(std::begin(products), std::end(products));
        const double limit = std::numeric_limits<float>::max();
        if (*minimum < -limit || *maximum > limit)
            return FloatingRange{};
        const double lower = std::min(-0x1p-126, *minimum * 2.0);
        const double upper = std::max(0x1p-126, *maximum * 2.0);
        return FloatingRange{lower, upper, false, false, false};
    }
    // Other arithmetic, narrowing and helper return values are not inferred. A later
    // explicit guard can establish facts for the resulting local binding.
    return FloatingRange{};
}

std::optional<NativeNumericExports::IntegerRange> NativeNumericExports::floatingIntegerRange(
    const Expr& node, const OutcomeFlow& flow)
{
    if (!node || node->kind != "call" || (node->resolution != "numeric_conversion" && node->resolution != "numeric_round") ||
        node->type.name != "int" || node->children.size() != 2)
        return std::nullopt;
    const auto& target = node->children.front();
    if (target->kind != "member" || target->children.size() != 1 ||
        target->children.front()->kind != "type" || target->children.front()->type != node->type)
        return std::nullopt;
    const auto& input = node->children.at(1);
    if (!(((target->text == "FromFloat" || target->text == "RoundFromFloat") && input->type.name == "float") ||
          ((target->text == "FromFloat32" || target->text == "RoundFromFloat32") && input->type.name == "float32")))
        return std::nullopt;
    const auto range = floatingRange(input, flow);
    constexpr double lowerLimit = -0x1p63;
    constexpr double upperLimit = 0x1p63;
    if (!range || range->mayNaN || !std::isfinite(range->minimum) || !std::isfinite(range->maximum) ||
        range->minimum < lowerLimit || range->maximum > upperLimit ||
        (range->maximum == upperLimit && !range->maximumOpen) || range->minimum > range->maximum ||
        (range->minimum == range->maximum && (range->minimumOpen || range->maximumOpen)))
        return std::nullopt;
    if (node->resolution == "numeric_round") {
        const auto minimum = static_cast<std::int64_t>(std::round(range->minimum));
        const auto maximum = range->maximum == upperLimit ? std::numeric_limits<std::int64_t>::max() :
            static_cast<std::int64_t>(std::round(range->maximum));
        return IntegerRange{minimum, maximum};
    }
    // Only evaluate host casts after proving exactly the runtime's signed64
    // domain. The excluded upper endpoint is handled without casting it.
    auto minimum = static_cast<std::int64_t>(range->minimum);
    auto maximum = range->maximum == upperLimit ? std::numeric_limits<std::int64_t>::max() :
        static_cast<std::int64_t>(range->maximum);
    if (range->minimumOpen && range->minimum < 0.0 && std::trunc(range->minimum) == range->minimum)
        ++minimum;
    if (range->maximumOpen && range->maximum > 0.0 && range->maximum != upperLimit &&
        std::trunc(range->maximum) == range->maximum)
        --maximum;
    if (minimum > maximum)
        return std::nullopt;
    return IntegerRange{minimum, maximum};
}

bool NativeNumericExports::refineIntegerCondition(const Expr& condition, const OutcomeFlow& input,
                                                 OutcomeFlow& whenTrue, OutcomeFlow& whenFalse)
{
    if (condition->kind != "binary" || condition->children.size() != 2 ||
        (condition->text != "<" && condition->text != "<=" && condition->text != ">" && condition->text != ">="))
        return true;
    const auto& left = condition->children.front();
    const auto& right = condition->children.at(1);
    const auto isBinding = [](const Expr& node) {
        return node->kind == "name" && node->resolution == "local" && node->type.name == "int";
    };
    if (!isBinding(left) && !isBinding(right))
        return true;
    const auto leftRange = integerRange(left, input);
    const auto rightRange = integerRange(right, input);
    if (!leftRange || !rightRange)
        return false;
    if (isBinding(left) && isBinding(right) && left->text == right->text) {
        const bool holds = condition->text == "<=" || condition->text == ">=";
        whenTrue.reachable = input.reachable && holds;
        whenFalse.reachable = input.reachable && !holds;
        return true;
    }
    const auto reverse = [](const std::string& operation) {
        if (operation == "<")
            return std::string(">");
        if (operation == "<=")
            return std::string(">=");
        if (operation == ">")
            return std::string("<");
        return std::string("<=");
    };
    const auto constrain = [&](OutcomeFlow& flow, const Expr& binding, const IntegerRange& known,
                               const IntegerRange& bound, const std::string& operation) {
        if (!spend())
            return false;
        if (!isBinding(binding))
            return true;
        IntegerRange narrowed = known;
        // A comparison with an interval supplies necessary bounds rather than
        // equality with an arbitrary endpoint. Refine each local independently
        // from the input facts; no relation survives a later assignment.
        const auto limit = operation == "<" || operation == "<=" ? bound.maximum : bound.minimum;
        if (operation == "<") {
            if (limit == std::numeric_limits<std::int64_t>::min()) {
                flow.reachable = false;
                return true;
            }
            narrowed.maximum = std::min(narrowed.maximum, limit - 1);
        } else if (operation == "<=") {
            narrowed.maximum = std::min(narrowed.maximum, limit);
        } else if (operation == ">") {
            if (limit == std::numeric_limits<std::int64_t>::max()) {
                flow.reachable = false;
                return true;
            }
            narrowed.minimum = std::max(narrowed.minimum, limit + 1);
        } else {
            narrowed.minimum = std::max(narrowed.minimum, limit);
        }
        if (narrowed.minimum > narrowed.maximum) {
            // Keep a valid interval even for hypothetical checks of unreachable
            // syntax. Empty ranges must never reach proof-time arithmetic.
            flow.reachable = false;
            return true;
        }
        flow.integers.at(binding->text) = narrowed;
        return true;
    };
    std::string complement;
    if (condition->text == "<")
        complement = ">=";
    else if (condition->text == "<=")
        complement = ">";
    else if (condition->text == ">")
        complement = "<=";
    else
        complement = "<";
    return constrain(whenTrue, left, *leftRange, *rightRange, condition->text) &&
        constrain(whenTrue, right, *rightRange, *leftRange, reverse(condition->text)) &&
        constrain(whenFalse, left, *leftRange, *rightRange, complement) &&
        constrain(whenFalse, right, *rightRange, *leftRange, reverse(complement));
}

bool NativeNumericExports::refineFloatingCondition(const Expr& condition, const OutcomeFlow& input,
                                                  OutcomeFlow& whenTrue, OutcomeFlow& whenFalse)
{
    if (condition->kind == "call" && condition->resolution == "float_math" && condition->children.size() == 2 &&
        condition->children.front()->text == "IsFinite") {
        const auto& binding = condition->children.at(1);
        if (binding->kind != "name" || binding->resolution != "local")
            return true;
        const auto known = floatingRange(binding, input);
        if (!known)
            return false;
        const double maximum = binding->type.name == "float32" ?
            static_cast<double>(std::numeric_limits<float>::max()) : std::numeric_limits<double>::max();
        FloatingRange finite = *known;
        constrainFloating(finite, ">=", -maximum);
        constrainFloating(finite, "<=", maximum);
        finite.mayNaN = false;
        storeFloating(whenTrue, binding->text, finite);
        if (!known->mayNaN && std::isfinite(known->minimum) && std::isfinite(known->maximum))
            whenFalse.reachable = false;
        return true;
    }
    if (condition->kind != "binary" || condition->children.size() != 2 ||
        (condition->text != "<" && condition->text != "<=" && condition->text != ">" && condition->text != ">="))
        return true;
    auto binding = condition->children.front();
    auto endpoint = condition->children.at(1);
    std::string comparison = condition->text;
    const auto isBinding = [](const Expr& node) {
        return node->kind == "name" && node->resolution == "local" &&
            (node->type.name == "float" || node->type.name == "float32");
    };
    if (!isBinding(binding)) {
        if (!isBinding(endpoint))
            return true;
        std::swap(binding, endpoint);
        if (comparison == "<")
            comparison = ">";
        else if (comparison == "<=")
            comparison = ">=";
        else if (comparison == ">")
            comparison = "<";
        else
            comparison = "<=";
    }
    const auto bound = floatingRange(endpoint, input);
    if (!bound)
        return false;
    if (bound->mayNaN || bound->minimum != bound->maximum || !std::isfinite(bound->minimum))
        return true;
    const auto known = floatingRange(binding, input);
    if (!known)
        return false;
    FloatingRange trueRange = *known;
    FloatingRange falseRange = *known;
    constrainFloating(trueRange, comparison, bound->minimum);
    // Ordered comparisons exclude NaN only on their true path. Their false
    // path also contains unordered inputs until a separate finiteness proof.
    trueRange.mayNaN = false;
    std::string complement;
    if (comparison == "<")
        complement = ">=";
    else if (comparison == "<=")
        complement = ">";
    else if (comparison == ">")
        complement = "<=";
    else
        complement = "<";
    constrainFloating(falseRange, complement, bound->minimum);
    storeFloating(whenTrue, binding->text, trueRange);
    storeFloating(whenFalse, binding->text, falseRange);
    return true;
}

std::optional<NativeNumericExports::IntegerRange> NativeNumericExports::returnedIntegerRange(
    const Expr& node, const OutcomeFlow& flow)
{
    const Declaration* declaring = nullptr;
    const auto* callee = pureTarget(node, declaring);
    const auto extents = callExtents(node, flow);
    std::optional<IntegerRange> returned;
    if (!callee || !extents || !method(*declaring, *callee, *extents, &returned))
        return std::nullopt;
    return returned.value_or(IntegerRange{std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()});
}

std::optional<NativeNumericExports::IntegerRange> NativeNumericExports::resultIntegerRange(
    const Expr& node, const OutcomeFlow& flow)
{
    if (!spend() || !node || node->type.name != "Result" || node->type.arguments.front().name != "int")
        return std::nullopt;
    if (node->kind == "name" && node->resolution == "local") {
        const auto found = flow.integers.find(node->text);
        if (found != flow.integers.end())
            return found->second;
    }
    if (node->kind == "call") {
        if (node->resolution == "result_construct" && node->children.front()->text == "Success")
            return integerRange(node->children.at(1), flow);
        if (node->resolution == "pure" || node->resolution == "qualified_pure")
            return returnedIntegerRange(node, flow);
    }
    return IntegerRange{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
}

std::optional<NativeNumericExports::IntegerRange> NativeNumericExports::arrayIntegerRange(
    const Expr& node, const OutcomeFlow& flow)
{
    if (!spend() || !node || (node->type.name != "Array" && node->type.name != "FixedArray") ||
        node->type.arguments.front().name != "int")
        return std::nullopt;
    if (node->kind == "name" && node->resolution == "local") {
        const auto found = flow.integers.find(node->text);
        if (found != flow.integers.end())
            return found->second;
    }
    if (node->kind == "call" && node->resolution == "collection_construct" && node->children.size() == 3)
        return integerRange(node->children.at(2), flow);
    if (node->type.name == "FixedArray" && node->kind == "call" &&
        node->resolution == "collection_construct" && node->children.size() == 2)
        return integerRange(node->children.at(1), flow);
    // Private array helpers retain unknown element ranges unless the caller has
    // explicit evidence. Extent proof alone never implies an element bound.
    return IntegerRange{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
}

std::optional<NativeNumericExports::IntegerRange> NativeNumericExports::integerRange(
    const Expr& node, const OutcomeFlow& flow)
{
    if (!node || node->type.name != "int" || !spend())
        return std::nullopt;
    if (node->resolution == "constant")
        return integerRange(node->constantValue, flow);
    if (node->kind == "integer") {
        std::int64_t value = 0;
        const auto parsed = std::from_chars(node->text.data(), node->text.data() + node->text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != node->text.data() + node->text.size())
            return std::nullopt;
        return IntegerRange{value, value};
    }
    if (node->kind == "name" && node->resolution == "local") {
        const auto found = flow.integers.find(node->text);
        return found == flow.integers.end() ? std::nullopt : std::optional(found->second);
    }
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (node->kind == "member" && node->resolution == "record")
        return IntegerRange{minimum, maximum};
    if (node->kind == "member" && node->resolution == "optional" && node->text == "Value")
        return IntegerRange{minimum, maximum};
    if (node->kind == "member" && node->resolution == "result" && node->text == "Value")
        return resultIntegerRange(node->children.at(0), flow);
    if (node->kind == "call") {
        if (node->resolution == "float_math" && node->children.front()->text == "ILogB")
            return IntegerRange{INT64_C(-2147483648), INT64_C(2147483647)};
        if (node->resolution == "numeric_conversion" || node->resolution == "numeric_round")
            return floatingIntegerRange(node, flow);
        if (node->resolution == "string_byte_at_or") {
            const auto fallback = integerRange(node->children.at(3), flow);
            if (!fallback)
                return std::nullopt;
            return IntegerRange{std::min(INT64_C(0), fallback->minimum), std::max(INT64_C(255), fallback->maximum)};
        }
        if (node->resolution == "pure" || node->resolution == "qualified_pure")
            return returnedIntegerRange(node, flow);
        if (node->resolution == "collection_read_at")
            return arrayIntegerRange(node->children.front()->children.front(), flow);
    }
    if (node->kind == "member" && node->resolution == "list") {
        const auto extent = arrayExtent(node->children.at(0), flow);
        if (!extent)
            return std::nullopt;
        const auto count = static_cast<std::int64_t>(*extent);
        return IntegerRange{count, count};
    }
    if (node->resolution == "minimum_int")
        return IntegerRange{minimum, minimum};
    if (node->kind == "unary" && node->text == "-") {
        const auto operand = integerRange(node->children.at(0), flow);
        if (!operand || operand->minimum == minimum)
            return std::nullopt;
        return IntegerRange{-operand->maximum, -operand->minimum};
    }
    if (node->kind == "conditional") {
        OutcomeFlow whenTrue;
        OutcomeFlow whenFalse;
        if (!splitCondition(node->children.at(0), flow, whenTrue, whenFalse))
            return std::nullopt;
        const auto first = integerRange(node->children.at(1), whenTrue);
        const auto second = integerRange(node->children.at(2), whenFalse);
        if (!first || !second)
            return std::nullopt;
        return IntegerRange{std::min(first->minimum, second->minimum), std::max(first->maximum, second->maximum)};
    }
    if (node->kind != "binary")
        return std::nullopt;
    const auto left = integerRange(node->children.at(0), flow);
    const auto right = integerRange(node->children.at(1), flow);
    if (!left || !right)
        return std::nullopt;
    if (node->text == "+") {
        if ((right->minimum < 0 && left->minimum < minimum - right->minimum) ||
            (right->maximum > 0 && left->maximum > maximum - right->maximum))
            return std::nullopt;
        return IntegerRange{left->minimum + right->minimum, left->maximum + right->maximum};
    }
    if (node->text == "-") {
        if ((right->maximum > 0 && left->minimum < minimum + right->maximum) ||
            (right->minimum < 0 && left->maximum > maximum + right->minimum))
            return std::nullopt;
        return IntegerRange{left->minimum - right->maximum, left->maximum - right->minimum};
    }
    // Nonnegative arithmetic avoids signed product and division edge cases.
    // Other operand ranges retain the ordinary checked runtime boundary.
    if (left->minimum < 0 || right->minimum < 0)
        return std::nullopt;
    if (node->text == "*") {
        if (right->maximum != 0 && left->maximum > maximum / right->maximum)
            return std::nullopt;
        return IntegerRange{left->minimum * right->minimum, left->maximum * right->maximum};
    }
    // A nonnegative dividend and strictly positive divisor exclude both signed
    // division faults. Quotients increase with the dividend and decrease with
    // the divisor, including truncation toward zero over these intervals.
    if (node->text == "/" && right->minimum > 0)
        return IntegerRange{left->minimum / right->maximum, left->maximum / right->minimum};
    if (node->text == "%" && right->minimum > 0 && right->minimum == right->maximum)
        return IntegerRange{0, std::min(left->maximum, right->maximum - 1)};
    return std::nullopt;
}

std::optional<std::size_t> NativeNumericExports::arrayExtent(const Expr& node, const OutcomeFlow& flow)
{
    if (node && node->type.name == "FixedArray") {
        if (!spendValue(node->type))
            return std::nullopt;
        return node->type.fixedExtent;
    }
    if (observation_ || !node || node->type.name != "Array" || node->type.arguments.size() != 1 ||
        !scalar(node->type.arguments.front()) || !spend())
        return std::nullopt;
    // The element channel matters even for an empty fixed-extent array.
    if (node->type.arguments.front().name == "float32")
        projection_.requiresBinary32Environment = true;
    if (node->kind == "name" && node->resolution == "local") {
        const auto found = flow.arrays.find(node->text);
        return found == flow.arrays.end() ? std::nullopt : std::optional(found->second);
    }
    if (node->kind == "call" && node->resolution == "pure") {
        const Declaration* declaring = nullptr;
        const auto* callee = pureTarget(node, declaring);
        if (!callee || callee->visible || activeMethods_.contains(callee->identity))
            return std::nullopt;
        const auto extents = callExtents(node, flow);
        if (!extents)
            return std::nullopt;
        auto signature = projection_.arrayHelpers.find(callee);
        if (signature == projection_.arrayHelpers.end() || !signature->second.returnedArray) {
            if (!method(*declaring, *callee, *extents))
                return std::nullopt;
            signature = projection_.arrayHelpers.find(callee);
        }
        if (signature == projection_.arrayHelpers.end() || signature->second.parameters != *extents)
            return std::nullopt;
        return signature->second.returnedArray;
    }
    if (node->kind != "call" || node->resolution != "collection_construct" || node->children.size() != 3)
        return std::nullopt;
    const auto size = integerRange(node->children.at(1), flow);
    if (!size || size->minimum < 0 || size->minimum != size->maximum ||
        static_cast<std::uint64_t>(size->maximum) > pipelang::model::maximumListElements)
        return std::nullopt;
    return static_cast<std::size_t>(size->maximum);
}

bool NativeNumericExports::arrayExpression(const Expr& node, const OutcomeFlow& flow)
{
    const auto extent = arrayExtent(node, flow);
    if (node->type.name == "FixedArray") {
        if (!extent)
            return false;
        const auto [proof, inserted] = projection_.arrayValues.emplace(node.get(), *extent);
        return (inserted || proof->second == *extent) && expression(node, flow);
    }
    if (node->type.arguments.front().name == "float" || node->type.arguments.front().name == "float32")
        projection_.constantEvaluable = false;
    if (!extent || !spend())
        return false;
    // Charge stack storage and every deep copy at its full scalar footprint.
    for (std::size_t index = 0; index < *extent; ++index) {
        if (!spendValue(node->type.arguments.front()))
            return false;
    }
    const auto [proof, inserted] = projection_.arrayValues.emplace(node.get(), *extent);
    if (!inserted && proof->second != *extent)
        return false;
    if (node->kind == "name")
        return true;
    if (node->resolution == "pure")
        return pureCall(node, flow);
    return expression(node->children.at(1), flow) && expression(node->children.at(2), flow);
}

const Method* NativeNumericExports::pureTarget(const Expr& node, const Declaration*& declaring) const
{
    declaring = owner_;
    if (node->resolution == "qualified_pure") {
        const auto found = std::find_if(program_.declarations.begin(), program_.declarations.end(), [&](const Declaration& candidate) {
            return candidate.name == node->owner;
        });
        if (found == program_.declarations.end() || !eligibleOwner(*found))
            return nullptr;
        declaring = &*found;
    } else if ((node->resolution != "pure" && !(observation_ && node->resolution == "effect")) ||
        node->owner != owner_->name) {
        return nullptr;
    }
    const auto found = std::find_if(declaring->methods.begin(), declaring->methods.end(), [&](const Method& candidate) {
        return candidate.identity == node->callable;
    });
    if (found == declaring->methods.end() || (node->resolution == "qualified_pure" && !found->visible))
        return nullptr;
    return &*found;
}

std::optional<std::map<std::size_t, std::size_t>> NativeNumericExports::callExtents(
    const Expr& node, const OutcomeFlow& flow)
{
    std::map<std::size_t, std::size_t> extents;
    for (std::size_t index = 1; index < node->children.size(); ++index) {
        if (!spend())
            return std::nullopt;
        const auto& argument = node->children[index];
        if (argument->type.name != "Array")
            continue;
        const auto extent = arrayExtent(argument, flow);
        if (!extent)
            return std::nullopt;
        extents.emplace(index - 1, *extent);
    }
    return extents;
}

bool NativeNumericExports::pureCall(const Expr& node, const OutcomeFlow& flow)
{
    const Declaration* declaring = nullptr;
    const auto* callee = pureTarget(node, declaring);
    const auto extents = callExtents(node, flow);
    // Every invocation is rechecked and charged, with fresh unknown scalar and
    // Result/Optional parameters. Only private, owned Array extents cross this boundary.
    if (!callee || !extents || !method(*declaring, *callee, *extents))
        return false;
    return std::all_of(node->children.begin() + 1, node->children.end(), [&](const Expr& argument) {
        return expression(argument, flow);
    });
}

std::optional<std::string> NativeNumericExports::stableKey(const Expr& node, const OutcomeFlow& flow)
{
    if (!node || !node->convertTo.name.empty() || !spend())
        return std::nullopt;
    if (node->kind == "name" && node->resolution == "local" && flow.bindings.contains(node->text))
        return node->text;
    if (node->kind != "member" || node->children.size() != 1)
        return std::nullopt;
    const bool ownedMember = node->resolution == "record" ||
        (node->resolution == "result" && (node->text == "Value" || node->text == "Error")) ||
        (node->resolution == "optional" && node->text == "Value");
    if (!ownedMember)
        return std::nullopt;
    const auto receiver = stableKey(node->children.front(), flow);
    if (!receiver)
        return std::nullopt;
    // Assignment admission forbids replacing these owned roots or their fields.
    // Each extraction still independently checks its receiver's outcome proof.
    return *receiver + "." + node->text;
}

unsigned NativeNumericExports::resultOutcomes(const Expr& node, const OutcomeFlow& flow)
{
    if (!node || node->type.name != "Result")
        return AnyOutcome;
    if (const auto key = stableKey(node, flow)) {
        const auto found = flow.results.find(*key);
        return found == flow.results.end() ? AnyOutcome : found->second;
    }
    if (node->kind == "call" && node->resolution == "result_construct" && !node->children.empty()) {
        const auto& constructor = node->children.front()->text;
        if (constructor == "Success")
            return Success;
        if (constructor == "Failure")
            return Failure;
        if (constructor == "Cancelled")
            return Cancelled;
    }
    // In particular, a helper call is not a stable local receiver. A second
    // invocation cannot inherit a fact proved about an earlier invocation.
    return AnyOutcome;
}

unsigned NativeNumericExports::optionalOutcomes(const Expr& node, const OutcomeFlow& flow)
{
    if (!node || !spend())
        return AnyOptional;
    if (node->convertTo.name == "Optional" && node->convertTo.arguments.size() == 1) {
        if (node->kind == "null")
            return None;
        const Type* payload = &node->convertTo.arguments.front();
        while (*payload != node->type) {
            if (payload->name != "Optional" || payload->arguments.size() != 1 || !spend())
                return AnyOptional;
            payload = &payload->arguments.front();
        }
        return Some;
    }
    if (node->type.name != "Optional")
        return AnyOptional;
    if (const auto key = stableKey(node, flow)) {
        const auto found = flow.optionals.find(*key);
        return found == flow.optionals.end() ? AnyOptional : found->second;
    }
    if (node->kind == "call" && node->resolution == "optional_construct" && !node->children.empty()) {
        if (node->children.front()->text == "Some")
            return Some;
        if (node->children.front()->text == "None")
            return None;
    }
    if (node->kind == "conditional" && node->children.size() == 3) {
        OutcomeFlow whenTrue;
        OutcomeFlow whenFalse;
        if (!splitCondition(node->children.front(), flow, whenTrue, whenFalse))
            return AnyOptional;
        const unsigned first = whenTrue.reachable ? optionalOutcomes(node->children.at(1), whenTrue) : 0U;
        const unsigned second = whenFalse.reachable ? optionalOutcomes(node->children.at(2), whenFalse) : 0U;
        return first | second;
    }
    return AnyOptional;
}

bool NativeNumericExports::copyFlow(const OutcomeFlow& source, OutcomeFlow& destination)
{
    // Outcome analysis shares the original allowance, including environment
    // copies and joins. Large branch graphs cannot multiply analysis work.
    if (!spend())
        return false;
    for (const auto& name : source.bindings) {
        (void)name;
        if (!spend())
            return false;
    }
    for (const auto& entry : source.results) {
        (void)entry;
        if (!spend())
            return false;
    }
    for (const auto& entry : source.integers) {
        (void)entry;
        if (!spend())
            return false;
    }
    for (const auto& entry : source.arrays) {
        (void)entry;
        if (!spend())
            return false;
    }
    for (const auto& entry : source.floating) {
        (void)entry;
        if (!spend())
            return false;
    }
    for (const auto& entry : source.optionals) {
        (void)entry;
        if (!spend())
            return false;
    }
    destination = source;
    return true;
}

bool NativeNumericExports::mergeOutcomes(const std::map<std::string, unsigned>& first,
                                       const std::map<std::string, unsigned>& second,
                                       unsigned unknown, bool firstReachable, bool secondReachable,
                                       std::map<std::string, unsigned>& destination)
{
    const auto mergeEntry = [&](const std::string& name) {
        if (!spend())
            return false;
        const auto left = first.find(name);
        const auto right = second.find(name);
        const unsigned leftOutcomes = firstReachable ? (left == first.end() ? unknown : left->second) : 0U;
        const unsigned rightOutcomes = secondReachable ? (right == second.end() ? unknown : right->second) : 0U;
        destination.emplace(name, leftOutcomes | rightOutcomes);
        return true;
    };
    for (const auto& [name, outcomes] : first) {
        (void)outcomes;
        if (!mergeEntry(name))
            return false;
    }
    for (const auto& [name, outcomes] : second) {
        (void)outcomes;
        if (!spend())
            return false;
        if (!first.contains(name) && !mergeEntry(name))
            return false;
    }
    return true;
}

bool NativeNumericExports::mergeFlows(const OutcomeFlow& first, const OutcomeFlow& second,
                                    OutcomeFlow& destination)
{
    if (!spend() || first.bindings.size() != second.bindings.size())
        return false;
    OutcomeFlow merged;
    merged.reachable = first.reachable || second.reachable;
    for (const auto& name : first.bindings) {
        if (!spend() || !second.bindings.contains(name))
            return false;
        merged.bindings.insert(name);
    }
    if (!mergeOutcomes(first.results, second.results, AnyOutcome, first.reachable, second.reachable, merged.results) ||
        !mergeOutcomes(first.optionals, second.optionals, AnyOptional, first.reachable, second.reachable, merged.optionals))
        return false;
    if (first.integers.size() != second.integers.size() || first.arrays != second.arrays)
        return false;
    for (const auto& [name, range] : first.integers) {
        if (!spend())
            return false;
        const auto other = second.integers.find(name);
        if (other == second.integers.end())
            return false;
        IntegerRange joined = range;
        if (!first.reachable)
            joined = other->second;
        else if (second.reachable)
            joined = {std::min(range.minimum, other->second.minimum), std::max(range.maximum, other->second.maximum)};
        merged.integers.emplace(name, joined);
    }
    for (const auto& entry : first.arrays) {
        if (!spend())
            return false;
        merged.arrays.insert(entry);
    }
    std::set<std::string> floatingNames;
    for (const auto& entry : first.floating) {
        if (!spend())
            return false;
        floatingNames.insert(entry.first);
    }
    for (const auto& entry : second.floating) {
        if (!spend())
            return false;
        floatingNames.insert(entry.first);
    }
    for (const auto& name : floatingNames) {
        if (!spend())
            return false;
        const auto firstBinding = first.floating.find(name);
        const auto secondBinding = second.floating.find(name);
        const FloatingRange firstRange = firstBinding == first.floating.end() ? FloatingRange{} : firstBinding->second;
        const FloatingRange secondRange = secondBinding == second.floating.end() ? FloatingRange{} : secondBinding->second;
        FloatingRange joined;
        if (!first.reachable)
            joined = secondRange;
        else if (!second.reachable)
            joined = firstRange;
        else
            joined = joinFloating(firstRange, secondRange);
        storeFloating(merged, name, joined);
    }
    destination = std::move(merged);
    return true;
}

bool NativeNumericExports::splitCondition(const Expr& condition, const OutcomeFlow& input,
                                        OutcomeFlow& whenTrue, OutcomeFlow& whenFalse)
{
    if (!condition || !spend())
        return false;
    if (condition->kind == "unary" && condition->text == "!")
        return splitCondition(condition->children.at(0), input, whenFalse, whenTrue);
    if (condition->kind == "binary" && (condition->text == "&&" || condition->text == "||")) {
        OutcomeFlow leftTrue;
        OutcomeFlow leftFalse;
        if (!splitCondition(condition->children.at(0), input, leftTrue, leftFalse))
            return false;
        OutcomeFlow rightTrue;
        OutcomeFlow rightFalse;
        const auto& rightInput = condition->text == "&&" ? leftTrue : leftFalse;
        if (!splitCondition(condition->children.at(1), rightInput, rightTrue, rightFalse))
            return false;
        if (condition->text == "&&") {
            whenTrue = std::move(rightTrue);
            return mergeFlows(leftFalse, rightFalse, whenFalse);
        }
        whenFalse = std::move(rightFalse);
        return mergeFlows(leftTrue, rightTrue, whenTrue);
    }
    if (!copyFlow(input, whenTrue) || !copyFlow(input, whenFalse))
        return false;
    if (!refineIntegerCondition(condition, input, whenTrue, whenFalse) ||
        !refineFloatingCondition(condition, input, whenTrue, whenFalse))
        return false;
    if (condition->kind == "boolean") {
        whenTrue.reachable = input.reachable && condition->text == "true";
        whenFalse.reachable = input.reachable && condition->text == "false";
        return true;
    }
    if (condition->kind == "member" && condition->resolution == "optional" && condition->text == "HasValue") {
        const auto& receiver = condition->children.at(0);
        const unsigned outcomes = optionalOutcomes(receiver, input);
        const unsigned trueOutcomes = outcomes & Some;
        const unsigned falseOutcomes = outcomes & None;
        if (const auto key = stableKey(receiver, input)) {
            whenTrue.optionals[*key] = trueOutcomes;
            whenFalse.optionals[*key] = falseOutcomes;
        }
        whenTrue.reachable = input.reachable && trueOutcomes != 0;
        whenFalse.reachable = input.reachable && falseOutcomes != 0;
        return true;
    }
    if (condition->kind != "member" || condition->resolution != "result")
        return true;
    unsigned selectedOutcome = 0;
    if (condition->text == "IsSuccess")
        selectedOutcome = Success;
    else if (condition->text == "IsFailure")
        selectedOutcome = Failure;
    else if (condition->text == "IsCancelled")
        selectedOutcome = Cancelled;
    if (selectedOutcome == 0)
        return true;
    const auto& receiver = condition->children.at(0);
    const unsigned outcomes = resultOutcomes(receiver, input);
    const unsigned trueOutcomes = outcomes & selectedOutcome;
    const unsigned falseOutcomes = outcomes & ~selectedOutcome;
    if (const auto key = stableKey(receiver, input)) {
        whenTrue.results[*key] = trueOutcomes;
        whenFalse.results[*key] = falseOutcomes;
    }
    whenTrue.reachable = input.reachable && trueOutcomes != 0;
    whenFalse.reachable = input.reachable && falseOutcomes != 0;
    return true;
}

bool NativeNumericExports::expression(const Expr& node, const OutcomeFlow& flow, bool payloadOnly)
{
    if (!node)
        return true;
    if (!payloadOnly && !node->convertTo.name.empty()) {
        const auto& target = node->convertTo;
        if (target.name != "Optional" || target.arguments.size() != 1 || !spendValue(target))
            return false;
        if (node->kind == "null")
            return node->type.name == "$null" && spend();
        const Type* payload = &target.arguments.front();
        while (*payload != node->type) {
            if (payload->name != "Optional" || payload->arguments.size() != 1 || !spend())
                return false;
            payload = &payload->arguments.front();
        }
        // Preserve checked node identity for array-operation lowering proofs.
        // Only owned nullable lifts are bypassed; payload arithmetic/access and
        // every child's conversion still require their ordinary proofs.
        return expression(node, flow, true);
    }
    if (node->type.name == "string") {
        projection_.constantEvaluable = false;
        return !observation_ && node->kind == "name" && node->resolution == "local" && spend();
    }
    if (node->type.name == "Array")
        return arrayExpression(node, flow);
    if (node->type.name == "int") {
        if (!integerRange(node, flow))
            return false;
    } else if (node->type.name == "void") {
        if (node->resolution != "collection_mutate_set" || !spend())
            return false;
    } else if (!spendValue(node->type)) {
        return false;
    }
    if (node->kind == "integer" || node->resolution == "minimum_int")
        return true;
    if (node->kind == "member" && node->resolution == "list")
        return arrayExpression(node->children.at(0), flow);
    if (node->resolution == "constant")
        return node->constantValue && (scalar(node->constantValue->type) || node->constantValue->type.name == "int");
    if (node->resolution == "ieee_constant")
        return true;
    if (node->kind == "name")
        return node->resolution == "local";
    if (node->kind == "float" || node->kind == "float32" || node->kind == "boolean" || node->kind == "uint32")
        return true;
    if (node->kind == "member" && node->resolution == "enum")
        return enumeration(node->type);
    if (node->kind == "member" && node->resolution == "result") {
        const auto& receiver = node->children.at(0);
        // The ordinary accessor may throw. Its numeric projection is safe only
        // when every path reaching this extraction proves the same value Success.
        if (node->text == "Value" && flow.reachable && resultOutcomes(receiver, flow) != Success)
            return false;
        return expression(receiver, flow);
    }
    if (node->kind == "member" && node->resolution == "optional") {
        const auto& receiver = node->children.at(0);
        if (node->text == "Value" && flow.reachable && optionalOutcomes(receiver, flow) != Some)
            return false;
        return expression(receiver, flow);
    }
    if (node->kind == "member" && node->resolution == "record")
        return expression(node->children.at(0), flow);
    if (node->kind == "named_construct") {
        // Named construction default-initializes every field before authored
        // assignments, including fields explicitly overwritten afterwards.
        if (!recordDefaults(node->type))
            return false;
        return std::all_of(node->children.begin(), node->children.end(), [&](const Expr& initializer) {
            return expression(initializer->children.at(0), flow);
        });
    }
    if (node->kind == "call") {
        if (node->resolution == "result_construct") {
            const auto& constructor = node->children.front()->text;
            const bool constructsDefaultError = constructor == "Success" ||
                (constructor == "Cancelled" && node->children.size() == 1);
            if (constructsDefaultError && !defaultValue(node->type.arguments.at(1)))
                return false;
        }
        if (node->resolution == "floating_environment_observe") {
            if (!observation_)
                return false;
            projection_.observesCurrentThread = true;
            return node->children.size() == 1;
        }
        if (node->resolution == "collection_read_at" || node->resolution == "collection_mutate_set") {
            const auto& receiver = node->children.at(0)->children.at(0);
            const auto extent = arrayExtent(receiver, flow);
            const auto index = integerRange(node->children.at(1), flow);
            // Fixed arrays are true owned values. Only standalone local or
            // parameter arrays may mutate; record/outcome paths remain immutable
            // so their established nullable and Result facts stay valid.
            if (receiver->type.name == "FixedArray" && node->resolution == "collection_mutate_set" &&
                (receiver->kind != "name" || receiver->resolution != "local" ||
                 !flow.bindings.contains(receiver->text)))
                return false;
            // Unreachable branches still need lowering metadata, but cannot
            // execute an access. Reachable accesses require the full index proof.
            if (!extent || !index || (flow.reachable && (index->minimum < 0 ||
                static_cast<std::uint64_t>(index->maximum) >= *extent)) ||
                !arrayExpression(receiver, flow) || !expression(node->children.at(1), flow))
                return false;
            if (node->resolution == "collection_mutate_set" && !expression(node->children.at(2), flow))
                return false;
            projection_.arrayOperations.insert(node.get());
            return true;
        }
        if (node->resolution == "pure" || node->resolution == "qualified_pure" ||
            (observation_ && node->resolution == "effect"))
            return pureCall(node, flow);
        if (node->resolution == "float_math" && node->children.front()->text == "Clamp" &&
            !floatingRange(node, flow))
            return false;
        if (standaloneHeader_ && node->resolution == "float_math") {
            static const std::set<std::string> supported{"IsFinite", "Abs", "CopySign", "Floor", "Ceiling", "Fmod", "Clamp"};
            if (!supported.contains(node->children.front()->text))
                return false;
        }
        if (program_.activeHostNumericEnvironment && node->resolution == "float_math") {
            static const std::set<std::string> supported{
                "IsFinite", "Abs", "CopySign", "Floor", "Ceiling", "Fmod", "ILogB", "ScaleB", "Clamp", "Sqrt", "Sin", "Cos"};
            if (!supported.contains(node->children.front()->text))
                return false;
        }
        const bool fixedConstruction = node->type.name == "FixedArray" &&
            node->resolution == "collection_construct" && node->children.size() == 2;
        if (!fixedConstruction && node->resolution != "string_byte_length_is" && node->resolution != "string_byte_at_or" &&
            node->resolution != "numeric_round" &&
            node->resolution != "float_math" && node->resolution != "float_bits" &&
            node->resolution != "unsigned_wrapping" && node->resolution != "result_construct" &&
            node->resolution != "optional_construct" &&
            !floatingConversion(node) &&
            !(node->resolution == "numeric_conversion" && node->type.name == "int") &&
            !(node->resolution == "construct" && node->type.arguments.empty() && !scalar(node->type)))
            return false;
        return std::all_of(node->children.begin() + 1, node->children.end(), [&](const Expr& child) {
            return expression(child, flow);
        });
    }
    if (node->kind == "binary" && (node->text == "&&" || node->text == "||")) {
        if (!expression(node->children.at(0), flow))
            return false;
        OutcomeFlow leftTrue;
        OutcomeFlow leftFalse;
        if (!splitCondition(node->children.at(0), flow, leftTrue, leftFalse))
            return false;
        const auto& rightFlow = node->text == "&&" ? leftTrue : leftFalse;
        return expression(node->children.at(1), rightFlow);
    }
    if (node->kind == "conditional") {
        if (!expression(node->children.at(0), flow))
            return false;
        OutcomeFlow whenTrue;
        OutcomeFlow whenFalse;
        return splitCondition(node->children.at(0), flow, whenTrue, whenFalse) &&
            expression(node->children.at(1), whenTrue) && expression(node->children.at(2), whenFalse);
    }
    if (node->kind != "unary" && node->kind != "binary")
        return false;
    if (node->kind == "binary") {
        // Integer arithmetic and shifts retain checked, potentially throwing
        // semantics. Only comparisons/boolean logic and uint32 bitwise operators
        // enter this projection; explicit wrapping calls are separately safe.
        if (node->text == "<<" || node->text == ">>")
            return false;
        const auto& operand = node->children.at(0)->type;
        if (operand.name == "string")
            return false;
        if (operand.name == "uint32" && (node->text == "+" || node->text == "-" ||
            node->text == "*" || node->text == "/" || node->text == "%"))
            return false;
    }
    return std::all_of(node->children.begin(), node->children.end(), [&](const Expr& child) {
        return expression(child, flow);
    });
}

bool NativeNumericExports::collectLocalReads(const Expr& node, std::set<std::string>& names)
{
    if (!node)
        return true;
    if (!spend())
        return false;
    if (node->kind == "name" && node->resolution == "local")
        names.insert(node->text);
    for (const auto& child : node->children) {
        if (!collectLocalReads(child, names))
            return false;
    }
    return true;
}

bool NativeNumericExports::preservesLoopControls(const std::vector<Statement>& body,
                                                const Statement& increment,
                                                const std::set<std::string>& controls)
{
    for (const auto& statement : body) {
        if (!spend() || statement.kind == "break" || statement.kind == "continue" ||
            !statement.cleanup.empty() || statement.hasFinally)
            return false;
        if (statement.kind == "assign" && statement.target && statement.target->kind == "name" &&
            controls.contains(statement.target->text) && &statement != &increment)
            return false;
        if (statement.kind == "local" && controls.contains(statement.name))
            return false;
        if (!preservesLoopControls(statement.initialization, increment, controls) ||
            !preservesLoopControls(statement.iteration, increment, controls) ||
            !preservesLoopControls(statement.body, increment, controls) ||
            !preservesLoopControls(statement.alternative, increment, controls))
            return false;
    }
    // Admitted helpers are receiver-free pure calls with owned arguments. Their
    // independently checked local writes cannot mutate these caller bindings.
    return true;
}

bool NativeNumericExports::countedLoop(const Statement& statement, OutcomeFlow& flow)
{
    const bool forLoop = statement.kind == "for";
    const auto& condition = statement.expression;
    if (!condition || condition->kind != "binary" ||
        (condition->text != "<" && condition->text != "<=") ||
        condition->children.size() != 2 ||
        (forLoop ? statement.iteration.size() != 1 : statement.body.empty()) ||
        !statement.alternative.empty() || !statement.cleanup.empty() || statement.hasFinally)
        return false;
    const auto& counter = condition->children.front();
    if (counter->kind != "name" || counter->resolution != "local" || counter->type.name != "int")
        return false;
    const auto start = integerRange(counter, flow);
    const auto bound = integerRange(condition->children.at(1), flow);
    if (!start || !bound || start->minimum != start->maximum || bound->minimum != bound->maximum)
        return false;

    const auto& increment = forLoop ? statement.iteration.front() : statement.body.back();
    if (increment.kind != "assign" || !increment.target || increment.target->kind != "name" ||
        increment.target->resolution != "local" || increment.target->text != counter->text ||
        !increment.expression || increment.expression->kind != "binary" || increment.expression->text != "+" ||
        increment.expression->children.size() != 2)
        return false;
    const auto& addedCounter = increment.expression->children.front();
    if (addedCounter->kind != "name" || addedCounter->resolution != "local" || addedCounter->text != counter->text)
        return false;
    const auto& stepExpression = increment.expression->children.at(1);
    const auto step = integerRange(stepExpression, flow);
    if (!step || step->minimum <= 0 || step->minimum != step->maximum)
        return false;

    std::set<std::string> controls;
    if (!collectLocalReads(condition->children.at(1), controls) || !collectLocalReads(stepExpression, controls) ||
        controls.contains(counter->text))
        return false;
    controls.insert(counter->text);
    if (!preservesLoopControls(statement.body, increment, controls) ||
        (forLoop && !preservesLoopControls(statement.iteration, increment, controls)))
        return false;

    const bool inclusive = condition->text == "<=";
    const bool enters = inclusive ? start->minimum <= bound->minimum : start->minimum < bound->minimum;
    std::uint64_t visits = 0;
    if (enters) {
        // Unsigned subtraction expresses the full positive signed64 distance
        // without evaluating a potentially overflowing signed subtraction.
        const auto distance = static_cast<std::uint64_t>(bound->minimum) - static_cast<std::uint64_t>(start->minimum);
        const auto stride = static_cast<std::uint64_t>(step->minimum);
        const auto quotient = distance / stride;
        if (inclusive) {
            if (quotient >= remaining_)
                return false;
            visits = quotient + 1;
        } else {
            visits = quotient + (distance % stride != 0 ? 1U : 0U);
            if (visits > remaining_)
                return false;
        }
    }
    projection_.countedLoops.insert(&statement);
    if (visits == 0) {
        // A zero-trip loop still evaluates its condition. Prove its complete
        // closure before considering the unreachable body; range inference
        // alone can collapse conditional bounds without admitting their calls.
        if (!expression(condition, flow))
            return false;
        // The last inverse-elimination pivot commonly has no following rows.
        // Check the unreachable body once to produce complete lowering proof;
        // its hypothetical writes cannot change the actual fallthrough facts.
        OutcomeFlow unreachable;
        if (!copyFlow(flow, unreachable))
            return false;
        unreachable.reachable = false;
        return statements(statement.body, unreachable) &&
            (!forLoop || statements(statement.iteration, unreachable));
    }

    auto expectedCounter = start->minimum;
    for (std::uint64_t visit = 0; visit < visits; ++visit) {
        if (!spend() || !expression(condition, flow) || !statements(statement.body, flow) ||
            (forLoop && !statements(statement.iteration, flow)))
            return false;
        if (expectedCounter > std::numeric_limits<std::int64_t>::max() - step->minimum)
            return false;
        expectedCounter += step->minimum;
        const auto actualCounter = flow.integers.find(counter->text);
        if (actualCounter == flow.integers.end() || actualCounter->second.minimum != expectedCounter ||
            actualCounter->second.maximum != expectedCounter)
            return false;
    }
    // Charge and prove the terminating condition as well as every body visit.
    return expression(condition, flow) &&
        (inclusive ? expectedCounter > bound->minimum : expectedCounter >= bound->minimum);
}

bool NativeNumericExports::statements(const std::vector<Statement>& body, OutcomeFlow& flow,
                                      std::vector<std::string>* retainedLocals)
{
    std::vector<std::string> locals;
    for (const auto& statement : body) {
        if (!spend())
            return false;
        if (statement.kind != "return" && statement.kind != "if" && statement.kind != "block" &&
            statement.kind != "local" && statement.kind != "assign" && statement.kind != "foreach" &&
            statement.kind != "expression" && statement.kind != "while" && statement.kind != "for")
            return false;
        if (statement.kind == "for") {
            if (statement.initialization.size() > 1 || !statement.cleanup.empty() || statement.hasFinally)
                return false;
            std::vector<std::string> loopLocals;
            if (!statements(statement.initialization, flow, &loopLocals) || !countedLoop(statement, flow))
                return false;
            // The initializer owns its lexical scope. Body and iteration writes
            // to pre-existing bindings survive; initializer locals do not.
            for (const auto& name : loopLocals) {
                if (!removeBinding(flow, name))
                    return false;
            }
            continue;
        }
        if (statement.kind == "local" && statement.type.name != "Array" && statement.type.name != "int" &&
            !spendValue(statement.type))
            return false;
        if (statement.kind == "expression" && (!statement.expression ||
            statement.expression->resolution != "collection_mutate_set"))
            return false;
        // Arrays, records and Result/Optional snapshots cannot be rebound.
        // Only scalars and independently range-proven integer locals are assignable.
        if (statement.kind == "assign" && (!statement.target || statement.target->kind != "name" ||
            (!scalar(statement.target->type) && statement.target->type.name != "int")))
            return false;
        if (!statement.cleanup.empty() || !expression(statement.expression, flow) ||
            !expression(statement.target, flow))
            return false;
        if (statement.kind == "if") {
            OutcomeFlow whenTrue;
            OutcomeFlow whenFalse;
            if (!splitCondition(statement.expression, flow, whenTrue, whenFalse) ||
                !statements(statement.body, whenTrue) || !statements(statement.alternative, whenFalse) ||
                !mergeFlows(whenTrue, whenFalse, flow))
                return false;
        } else if (statement.kind == "block") {
            if (!statements(statement.body, flow))
                return false;
        } else if (statement.kind == "while") {
            if (!countedLoop(statement, flow))
                return false;
        } else if (statement.kind == "foreach") {
            const auto extent = arrayExtent(statement.expression, flow);
            const bool fixedArray = statement.expression->type.name == "FixedArray";
            if (!extent || (!fixedArray && !scalar(statement.type)) || !statement.itemConvertTo.name.empty() ||
                !statement.alternative.empty())
                return false;
            projection_.arrayLoops.insert(&statement);
            // Analyze each visit under the same shared allowance. Source foreach
            // captures an owned snapshot; mutation of the original array cannot
            // change the fixed trip count or the element stream.
            const auto visits = std::max<std::size_t>(1, *extent);
            for (std::size_t index = 0; index < visits; ++index) {
                OutcomeFlow iteration;
                if (!copyFlow(flow, iteration) || !spendValue(statement.type))
                    return false;
                if (!iteration.bindings.insert(statement.name).second)
                    return false;
                if (*extent == 0)
                    iteration.reachable = false;
                if (statement.type.name == "int") {
                    const auto range = arrayIntegerRange(statement.expression, flow);
                    if (!range)
                        return false;
                    iteration.integers.emplace(statement.name, *range);
                }
                if (!statements(statement.body, iteration))
                    return false;
                if (!removeBinding(iteration, statement.name))
                    return false;
                if (*extent != 0)
                    flow = std::move(iteration);
            }
        } else {
            if (!statement.body.empty() || !statement.alternative.empty())
                return false;
            if (statement.kind == "return") {
                if (flow.reachable && statement.expression) {
                    std::optional<IntegerRange> returned;
                    if (statement.expression->type.name == "int") {
                        returned = integerRange(statement.expression, flow);
                    } else if (statement.expression->type.name == "Result" &&
                        statement.expression->type.arguments.front().name == "int" &&
                        (resultOutcomes(statement.expression, flow) & Success) != 0) {
                        returned = resultIntegerRange(statement.expression, flow);
                    }
                    const bool needsIntegerSummary = statement.expression->type.name == "int" ||
                        (statement.expression->type.name == "Result" &&
                         statement.expression->type.arguments.front().name == "int" &&
                         (resultOutcomes(statement.expression, flow) & Success) != 0);
                    if (needsIntegerSummary && !returned)
                        return false;
                    if (returned) {
                        if (currentIntegerReturn_) {
                            currentIntegerReturn_->minimum = std::min(currentIntegerReturn_->minimum, returned->minimum);
                            currentIntegerReturn_->maximum = std::max(currentIntegerReturn_->maximum, returned->maximum);
                        } else {
                            currentIntegerReturn_ = returned;
                        }
                    }
                }
                if (statement.expression && statement.expression->type.name == "Array") {
                    if (!currentMethod_ || currentMethod_->visible || currentMethod_->result.name != "Array")
                        return false;
                    const auto extent = arrayExtent(statement.expression, flow);
                    if (!extent || (currentArrayReturn_ && currentArrayReturn_ != extent))
                        return false;
                    currentArrayReturn_ = *extent;
                }
                flow.reachable = false;
            }
            if (statement.kind == "expression" && statement.expression->resolution == "collection_mutate_set") {
                const auto& receiver = statement.expression->children.front()->children.front();
                if (receiver->type.arguments.front().name == "int") {
                    const auto stored = integerRange(statement.expression->children.at(2), flow);
                    const auto elements = flow.integers.find(receiver->text);
                    if (!stored || elements == flow.integers.end())
                        return false;
                    elements->second.minimum = std::min(elements->second.minimum, stored->minimum);
                    elements->second.maximum = std::max(elements->second.maximum, stored->maximum);
                }
            }
            if (statement.kind == "local") {
                if (!flow.bindings.insert(statement.name).second)
                    return false;
                if (statement.type.name == "Result") {
                    flow.results.emplace(statement.name, resultOutcomes(statement.expression, flow));
                    if (statement.type.arguments.front().name == "int") {
                        const auto range = resultIntegerRange(statement.expression, flow);
                        if (!range)
                            return false;
                        flow.integers.emplace(statement.name, *range);
                    }
                } else if (statement.type.name == "Optional") {
                    flow.optionals.emplace(statement.name, optionalOutcomes(statement.expression, flow));
                } else if (statement.type.name == "int") {
                    const auto range = integerRange(statement.expression, flow);
                    if (!range)
                        return false;
                    flow.integers.emplace(statement.name, *range);
                } else if (statement.type.name == "Array" || statement.type.name == "FixedArray") {
                    const auto extent = arrayExtent(statement.expression, flow);
                    if (!extent)
                        return false;
                    flow.arrays.emplace(statement.name, *extent);
                    if (statement.type.arguments.front().name == "int") {
                        const auto range = arrayIntegerRange(statement.expression, flow);
                        if (!range)
                            return false;
                        flow.integers.emplace(statement.name, *range);
                    }
                    if (statement.type.name == "Array") {
                        const auto [proof, inserted] = projection_.arrayLocals.emplace(&statement, *extent);
                        if (!inserted && proof->second != *extent)
                            return false;
                    }
                }
                if (statement.type.name == "float" || statement.type.name == "float32") {
                    const auto range = floatingRange(statement.expression, flow);
                    if (!range)
                        return false;
                    storeFloating(flow, statement.name, *range);
                }
                locals.push_back(statement.name);
            } else if (statement.kind == "assign" && statement.target->type.name == "int") {
                const auto range = integerRange(statement.expression, flow);
                const auto local = flow.integers.find(statement.target->text);
                if (!range || local == flow.integers.end())
                    return false;
                local->second = *range;
            } else if (statement.kind == "assign" &&
                (statement.target->type.name == "float" || statement.target->type.name == "float32")) {
                const auto range = floatingRange(statement.expression, flow);
                if (!range)
                    return false;
                storeFloating(flow, statement.target->text, *range);
            }
        }
    }
    if (retainedLocals) {
        retainedLocals->insert(retainedLocals->end(), locals.begin(), locals.end());
    } else {
        for (const auto& name : locals) {
            if (!removeBinding(flow, name))
                return false;
        }
    }
    return true;
}

bool NativeNumericExports::removeBinding(OutcomeFlow& flow, const std::string& name)
{
    if (!spend())
        return false;
    const auto removePaths = [&](std::map<std::string, unsigned>& outcomes) {
        outcomes.erase(name);
        const auto prefix = name + ".";
        auto entry = outcomes.lower_bound(prefix);
        while (entry != outcomes.end() && entry->first.starts_with(prefix)) {
            if (!spend())
                return false;
            entry = outcomes.erase(entry);
        }
        return true;
    };
    if (!removePaths(flow.results) || !removePaths(flow.optionals))
        return false;
    flow.bindings.erase(name);
    flow.integers.erase(name);
    flow.arrays.erase(name);
    flow.floating.erase(name);
    return true;
}

bool NativeNumericExports::method(const Declaration& owner, const Method& selected,
                                  const std::map<std::size_t, std::size_t>& arrayParameters,
                                  std::optional<IntegerRange>* returnedInteger)
{
    const bool returnsArray = selected.result.name == "Array";
    if (!eligibleOwner(owner) || (!selected.pure && !observation_) || selected.asynchronous || selected.constructor || selected.virtualMethod ||
        selected.overrideMethod || !selected.typeParameters.empty() || selected.sourceName.find('<') != std::string::npos)
        return false;
    if (returnsArray) {
        if (selected.result.arguments.front().name == "float" || selected.result.arguments.front().name == "float32")
            projection_.constantEvaluable = false;
        if (observation_ || selected.visible || selected.result.arguments.size() != 1 || !scalar(selected.result.arguments.front()) || !spend())
            return false;
        if (selected.result.arguments.front().name == "float32")
            projection_.requiresBinary32Environment = true;
    } else if (!spendValue(selected.result)) {
        return false;
    }
    if (!activeMethods_.insert(selected.identity).second)
        return false;
    OutcomeFlow flow;
    std::size_t arrayCount = 0;
    for (std::size_t index = 0; index < selected.parameters.size(); ++index) {
        const auto& parameter = selected.parameters[index];
        if (!spend() || !flow.bindings.insert(parameter.name).second)
            return false;
        if (parameter.type.name == "Array") {
            if (parameter.type.arguments.front().name == "float" || parameter.type.arguments.front().name == "float32")
                projection_.constantEvaluable = false;
            const auto extent = arrayParameters.find(index);
            if (observation_ || selected.visible || extent == arrayParameters.end() || parameter.type.arguments.size() != 1 ||
                !scalar(parameter.type.arguments.front()) || extent->second > pipelang::model::maximumListElements || !spend())
                return false;
            if (parameter.type.arguments.front().name == "float32")
                projection_.requiresBinary32Environment = true;
            for (std::size_t element = 0; element < extent->second; ++element) {
                if (!spendValue(parameter.type.arguments.front()))
                    return false;
            }
            flow.arrays.emplace(parameter.name, extent->second);
            if (parameter.type.arguments.front().name == "int")
                flow.integers.emplace(parameter.name, IntegerRange{std::numeric_limits<std::int64_t>::min(),
                    std::numeric_limits<std::int64_t>::max()});
            ++arrayCount;
        } else {
            if (!observation_ && parameter.type.name == "string" && parameter.type.arguments.empty()) {
                projection_.constantEvaluable = false;
                if (!spend())
                    return false;
                continue;
            }
            if (!spendValue(parameter.type))
                return false;
            if (parameter.type.name == "Result")
                flow.results.emplace(parameter.name, AnyOutcome);
            if (parameter.type.name == "int")
                flow.integers.emplace(parameter.name, IntegerRange{std::numeric_limits<std::int64_t>::min(),
                    std::numeric_limits<std::int64_t>::max()});
            if (parameter.type.name == "FixedArray" && parameter.type.arguments.front().name == "int")
                flow.integers.emplace(parameter.name, IntegerRange{std::numeric_limits<std::int64_t>::min(),
                    std::numeric_limits<std::int64_t>::max()});
        }
    }
    if (arrayCount != arrayParameters.size())
        return false;
    const bool arrayHelper = returnsArray || arrayCount != 0;
    if (arrayHelper) {
        const auto [signature, inserted] = projection_.arrayHelpers.emplace(&selected, ArrayHelper{arrayParameters, std::nullopt});
        if (!inserted && signature->second.parameters != arrayParameters)
            return false;
    }
    const auto* previousOwner = owner_;
    const auto* previousMethod = currentMethod_;
    const auto previousReturn = currentArrayReturn_;
    const auto previousIntegerReturn = currentIntegerReturn_;
    owner_ = &owner;
    currentMethod_ = &selected;
    currentArrayReturn_.reset();
    currentIntegerReturn_.reset();
    bool admitted = statements(selected.body, flow);
    if (arrayHelper && admitted) {
        auto& signature = projection_.arrayHelpers.at(&selected);
        if ((returnsArray && !currentArrayReturn_) ||
            (signature.returnedArray && signature.returnedArray != currentArrayReturn_))
            admitted = false;
        else
            signature.returnedArray = currentArrayReturn_;
    }
    if (returnedInteger)
        *returnedInteger = currentIntegerReturn_;
    currentIntegerReturn_ = previousIntegerReturn;
    currentArrayReturn_ = previousReturn;
    currentMethod_ = previousMethod;
    owner_ = previousOwner;
    activeMethods_.erase(selected.identity);
    return admitted;
}

bool NativeNumericExports::admitsHeader(const Program& program, const Declaration& owner,
                                       const Method& selected, bool publicOnly, Projection* projection)
{
    NativeNumericExports analysis(program, owner);
    analysis.standaloneHeader_ = true;
    if ((publicOnly && !selected.visible) || !analysis.method(owner, selected))
        return false;
    if (projection)
        *projection = std::move(analysis.projection_);
    return true;
}

bool NativeNumericExports::admitsObservation(const Program& program, const Declaration& owner,
                                             const Method& selected, bool publicOnly,
                                             Projection* projection, bool requireObservation)
{
    NativeNumericExports analysis(program, owner, true);
    analysis.projection_.constantEvaluable = false;
    if ((publicOnly && (!selected.visible || selected.pure)) || !analysis.method(owner, selected) ||
        (requireObservation && !analysis.projection_.observesCurrentThread))
        return false;
    if (projection)
        *projection = std::move(analysis.projection_);
    return true;
}

bool NativeNumericExports::admits(const Program& program, const Declaration& owner,
                                 const Method& selected, bool publicOnly, Projection* projection)
{
    NativeNumericExports analysis(program, owner);
    if ((publicOnly && !selected.visible) || !analysis.method(owner, selected))
        return false;
    if (projection)
        *projection = std::move(analysis.projection_);
    return true;
}
}
