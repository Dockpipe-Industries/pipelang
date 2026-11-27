// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "syntax.hpp"
#include <cstdint>
#include <optional>
#include <limits>

namespace pipelang::models {
// Admission consumes checked syntax, never source spelling or consumer policy.
class NativeNumericExports final {
public:
    // Numeric-only lowering consumes this proof after successful admission.
    // Dynamic Array helpers retain their private, inferred stack extents.
    // FixedArray values carry their checked shape in ordinary public signatures.
    struct ArrayHelper {
        std::map<std::size_t, std::size_t> parameters;
        std::optional<std::size_t> returnedArray;
        bool operator==(const ArrayHelper&) const = default;
    };

    struct Projection {
        std::map<const Method*, ArrayHelper> arrayHelpers;
        std::map<const Expression*, std::size_t> arrayValues;
        std::map<const Statement*, std::size_t> arrayLocals;
        std::set<const Expression*> arrayOperations;
        std::set<const Statement*> arrayLoops;
        std::set<const Statement*> countedLoops;
        std::set<std::string> valueDeclarations;
        bool constantEvaluable = true;
        bool observesCurrentThread = false;
        // Set by the same bounded closure proof that admits numeric lowering.
        // Absence is meaningful only after the complete method is admitted.
        bool requiresBinary32Environment = false;
    };

    static bool admits(const Program& program, const Declaration& owner,
                       const Method& method, bool publicOnly = true, Projection* projection = nullptr);
    static bool admitsHeader(const Program& program, const Declaration& owner,
                             const Method& method, bool publicOnly = true, Projection* projection = nullptr);
    static bool admitsObservation(const Program& program, const Declaration& owner,
                                  const Method& method, bool publicOnly = true, Projection* projection = nullptr,
                                  bool requireObservation = true);

private:
    enum OutcomeMask : unsigned {
        Success = 1,
        Failure = 2,
        Cancelled = 4,
        AnyOutcome = Success | Failure | Cancelled
    };

    enum OptionalMask : unsigned {
        None = 1,
        Some = 2,
        AnyOptional = None | Some
    };

    struct IntegerRange {
        std::int64_t minimum;
        std::int64_t maximum;
    };

    struct FloatingRange {
        double minimum = -std::numeric_limits<double>::infinity();
        double maximum = std::numeric_limits<double>::infinity();
        bool minimumOpen = false;
        bool maximumOpen = false;
        bool mayNaN = true;
    };

    struct OutcomeFlow {
        bool reachable = true;
        std::set<std::string> bindings;
        std::map<std::string, unsigned> results;
        // Immutable owned snapshots permit stable paths such as input.Child,
        // outcome.Value.Child and outcome.Error.Child. Missing facts are unknown.
        std::map<std::string, unsigned> optionals;
        // Unique local names hold scalar bounds, successful Result<int> payload
        // bounds, or conservative owned Array<int> element unions. The checked
        // source type selects the interpretation; scopes remove all facts together.
        std::map<std::string, IntegerRange> integers;
        std::map<std::string, std::size_t> arrays;
        // Missing bindings have the unknown range. Facts are stored lazily so
        // ordinary floating calculations do not acquire whole-frame overhead.
        std::map<std::string, FloatingRange> floating;
    };

    NativeNumericExports(const Program& program, const Declaration& owner, bool observation = false);
    bool eligibleOwner(const Declaration& owner) const;
    bool enumeration(const Type& type) const;
    bool scalar(const Type& type) const;
    bool floatingConversion(const Expr& expression) const;
    bool spend();
    bool spendValue(const Type& type);
    bool spendValue(const Type& type, bool allowResult);
    struct ValueBounds {
        std::size_t footprint;
        std::size_t ownedElements;
        std::size_t storageBytes;
        std::size_t ownedBytes;
    };
    std::optional<ValueBounds> valueBounds(const Type& type, std::set<std::string>& records);
    bool defaultValue(const Type& type);
    bool recordDefaults(const Type& type);
    bool fixedDefaultExpression(const Expr& expression);
    std::optional<IntegerRange> integerRange(const Expr& expression, const OutcomeFlow& flow);
    std::optional<IntegerRange> resultIntegerRange(const Expr& expression, const OutcomeFlow& flow);
    std::optional<IntegerRange> arrayIntegerRange(const Expr& expression, const OutcomeFlow& flow);
    std::optional<IntegerRange> returnedIntegerRange(const Expr& expression, const OutcomeFlow& flow);
    std::optional<FloatingRange> floatingRange(const Expr& expression, const OutcomeFlow& flow);
    std::optional<IntegerRange> floatingIntegerRange(const Expr& expression, const OutcomeFlow& flow);
    bool refineIntegerCondition(const Expr& condition, const OutcomeFlow& input,
                                OutcomeFlow& whenTrue, OutcomeFlow& whenFalse);
    bool refineFloatingCondition(const Expr& condition, const OutcomeFlow& input,
                                 OutcomeFlow& whenTrue, OutcomeFlow& whenFalse);
    static FloatingRange joinFloating(const FloatingRange& first, const FloatingRange& second);
    static void constrainFloating(FloatingRange& range, const std::string& comparison, double bound);
    static void storeFloating(OutcomeFlow& flow, const std::string& name, const FloatingRange& range);
    std::optional<std::size_t> arrayExtent(const Expr& expression, const OutcomeFlow& flow);
    bool arrayExpression(const Expr& expression, const OutcomeFlow& flow);
    const Method* pureTarget(const Expr& expression, const Declaration*& declaring) const;
    std::optional<std::map<std::size_t, std::size_t>> callExtents(const Expr& expression, const OutcomeFlow& flow);
    bool pureCall(const Expr& expression, const OutcomeFlow& flow);
    std::optional<std::string> stableKey(const Expr& expression, const OutcomeFlow& flow);
    unsigned resultOutcomes(const Expr& expression, const OutcomeFlow& flow);
    unsigned optionalOutcomes(const Expr& expression, const OutcomeFlow& flow);
    bool mergeOutcomes(const std::map<std::string, unsigned>& first,
                       const std::map<std::string, unsigned>& second,
                       unsigned unknown, bool firstReachable, bool secondReachable,
                       std::map<std::string, unsigned>& destination);
    bool removeBinding(OutcomeFlow& flow, const std::string& name);
    bool copyFlow(const OutcomeFlow& source, OutcomeFlow& destination);
    bool mergeFlows(const OutcomeFlow& first, const OutcomeFlow& second, OutcomeFlow& destination);
    bool splitCondition(const Expr& condition, const OutcomeFlow& input,
                        OutcomeFlow& whenTrue, OutcomeFlow& whenFalse);
    bool expression(const Expr& expression, const OutcomeFlow& flow, bool payloadOnly = false);
    bool collectLocalReads(const Expr& expression, std::set<std::string>& names);
    bool preservesLoopControls(const std::vector<Statement>& body, const Statement& increment,
                               const std::set<std::string>& controls);
    bool countedLoop(const Statement& statement, OutcomeFlow& flow);
    bool statements(const std::vector<Statement>& body, OutcomeFlow& flow,
                    std::vector<std::string>* retainedLocals = nullptr);
    bool method(const Declaration& owner, const Method& method,
                const std::map<std::size_t, std::size_t>& arrayParameters = {},
                std::optional<IntegerRange>* returnedInteger = nullptr);

    const Program& program_;
    const Declaration* owner_;
    bool observation_;
    bool standaloneHeader_ = false;
    std::size_t remaining_;
    std::set<std::string> activeMethods_;
    std::set<std::string> activeValueDeclarations_;
    std::set<std::string> activeRecordDefaults_;
    Projection projection_;
    const Method* currentMethod_ = nullptr;
    std::optional<std::size_t> currentArrayReturn_;
    std::optional<IntegerRange> currentIntegerReturn_;
};
}
