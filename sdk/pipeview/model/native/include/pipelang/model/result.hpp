// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <charconv>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace pipelang::model {

inline constexpr std::size_t maximumStringBytes = 1024 * 1024;

class ExecutionFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class OperationCancelled : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class EvaluationLimitFailure : public ExecutionFailure {
public:
    using ExecutionFailure::ExecutionFailure;
};

struct Error {
    std::string message;
    bool operator==(const Error&) const = default;
};

enum class Outcome { Success, Cancelled, Failure };

template<class T, class E = std::string>
class Result {
public:
    constexpr Result() : outcome_(Outcome::Cancelled), error_{} {}
    constexpr static Result Success(T value) {
        return Result(Outcome::Success, std::move(value), E{});
    }
    constexpr static Result Failure(E error) {
        return Result(Outcome::Failure, std::nullopt, std::move(error));
    }
    constexpr static Result Cancelled(E error = E{}) {
        return Result(Outcome::Cancelled, std::nullopt, std::move(error));
    }
    constexpr bool is_success() const { return outcome_ == Outcome::Success; }
    constexpr bool is_cancelled() const { return outcome_ == Outcome::Cancelled; }
    constexpr bool is_failure() const { return outcome_ == Outcome::Failure; }
    constexpr Outcome outcome() const { return outcome_; }
    constexpr const E& error() const { return error_; }
    constexpr const T& value() const {
        if (!value_)
            throw ExecutionFailure("Result.Value requires Success");
        return *value_;
    }
    bool operator==(const Result&) const = default;
private:
    constexpr Result(Outcome outcome, std::optional<T> value, E error)
        : outcome_(outcome), value_(std::move(value)), error_(std::move(error)) {}
    Outcome outcome_;
    std::optional<T> value_;
    E error_;
};

template<class E>
class Result<void, E> {
public:
    constexpr Result() : outcome_(Outcome::Cancelled), error_{} {}
    constexpr static Result Success() { return Result(Outcome::Success, E{}); }
    constexpr static Result Failure(E error) { return Result(Outcome::Failure, std::move(error)); }
    constexpr static Result Cancelled(E error = E{}) { return Result(Outcome::Cancelled, std::move(error)); }
    constexpr bool is_success() const { return outcome_ == Outcome::Success; }
    constexpr bool is_cancelled() const { return outcome_ == Outcome::Cancelled; }
    constexpr bool is_failure() const { return outcome_ == Outcome::Failure; }
    constexpr Outcome outcome() const { return outcome_; }
    constexpr const E& error() const { return error_; }
    bool operator==(const Result&) const = default;
private:
    constexpr Result(Outcome outcome, E error) : outcome_(outcome), error_(std::move(error)) {}
    Outcome outcome_;
    E error_;
};

using InvocationResult = Result<void, std::string>;

template<class T>
class Optional {
public:
    constexpr Optional() = default;
    constexpr static Optional None() { return {}; }
    constexpr static Optional Some(T value) { return Optional(std::move(value)); }
    constexpr bool has_value() const { return value_.has_value(); }
    constexpr const T& value() const {
        if (!value_)
            throw ExecutionFailure("Optional.Value requires Some");
        return *value_;
    }
    bool operator==(const Optional&) const = default;
private:
    constexpr explicit Optional(T value) : value_(std::move(value)) {}
    std::optional<T> value_;
};

template<class E>
class IResult;
template<class E>
void validate(const IResult<E>& value);
template<class T>
class List;
template<class T, std::size_t N>
class FixedArray;
template<class T>
void validate(const List<T>& value);
template<class T, std::size_t N>
constexpr void validate(const FixedArray<T, N>& value);
template<class T>
constexpr void validate(const Optional<T>& value);
template<class T, class E>
constexpr void validate(const Result<T, E>& result);

template<class T>
constexpr void validate(const T&) {}

inline void validate(const std::string& value) {
    if (value.size() > maximumStringBytes)
        throw ExecutionFailure("string byte limit exceeded");
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset++]);
        if (first <= 0x7f)
            continue;
        unsigned remaining = 0;
        std::uint32_t scalar = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1;
            scalar = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            scalar = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            scalar = first & 0x07;
            minimum = 0x10000;
        } else {
            throw ExecutionFailure("string contains invalid UTF-8");
        }
        if (remaining > value.size() - offset)
            throw ExecutionFailure("string contains invalid UTF-8");
        for (unsigned index = 0; index < remaining; ++index) {
            const auto next = static_cast<unsigned char>(value[offset++]);
            if ((next & 0xc0) != 0x80)
                throw ExecutionFailure("string contains invalid UTF-8");
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            throw ExecutionFailure("string contains invalid UTF-8");
    }
}

inline void validate(const Error& error) { validate(error.message); }

inline Error makeError(std::string message) {
    validate(message);
    return Error{std::move(message)};
}

[[noreturn]] inline void raiseError(const Error& error) {
    validate(error);
    throw ExecutionFailure(error.message);
}

inline std::string safeErrorMessage(const std::exception& error) {
    std::string message = error.what();
    try {
        validate(message);
        return message;
    } catch (const ExecutionFailure&) {
        return "host exception contained invalid error text";
    }
}

template<class T, class E>
constexpr void validate(const Result<T, E>& result) {
    validate(result.error());
    if constexpr (!std::is_void_v<T>) {
        if (result.is_success())
            validate(result.value());
    }
}

template<class T>
constexpr void validate(const Optional<T>& value) {
    if (value.has_value())
        validate(value.value());
}

inline constexpr std::int64_t add(std::int64_t left, std::int64_t right) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
        throw ExecutionFailure("integer addition overflow");
    return left + right;
}

inline constexpr std::int64_t subtract(std::int64_t left, std::int64_t right) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right))
        throw ExecutionFailure("integer subtraction overflow");
    return left - right;
}

inline constexpr std::int64_t multiply(std::int64_t left, std::int64_t right) {
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((left > 0 && ((right > 0 && left > maximum / right) ||
                      (right < 0 && right < minimum / left))) ||
        (left < 0 && ((right > 0 && left < minimum / right) ||
                      (right < 0 && left < maximum / right))))
        throw ExecutionFailure("integer multiplication overflow");
    return left * right;
}

inline constexpr std::int64_t divide(std::int64_t left, std::int64_t right) {
    if (right == 0)
        throw ExecutionFailure("integer division by zero");
    if (left == std::numeric_limits<std::int64_t>::min() && right == -1)
        throw ExecutionFailure("integer division overflow");
    return left / right;
}

inline constexpr std::int64_t remainder(std::int64_t left, std::int64_t right) {
    if (right == 0)
        throw ExecutionFailure("integer remainder by zero");
    // The mathematical truncated remainder is zero even though the associated
    // quotient cannot be represented. Never evaluate C++ MIN % -1, which is UB.
    if (left == std::numeric_limits<std::int64_t>::min() && right == -1)
        return 0;
    return left % right;
}

inline constexpr std::int64_t negate(std::int64_t value) {

    return subtract(0, value);
}

inline std::string concatenate(const std::string& left, const std::string& right) {
    validate(left);
    validate(right);
    if (right.size() > maximumStringBytes - left.size())
        throw ExecutionFailure("string byte limit exceeded");
    return left + right;
}

inline bool isUtf8Boundary(const std::string& value, std::size_t offset) {
    return offset == value.size() ||
        (static_cast<unsigned char>(value[offset]) & 0xc0) != 0x80;
}

inline std::size_t checkedUtf8Offset(const std::string& value, std::int64_t offset) {
    if (offset < 0 || static_cast<std::uint64_t>(offset) > value.size())
        throw ExecutionFailure("UTF-8 byte offset out of range");
    const auto position = static_cast<std::size_t>(offset);
    if (!isUtf8Boundary(value, position))
        throw ExecutionFailure("UTF-8 byte offset splits a scalar");
    return position;
}

inline std::int64_t utf8Length(const std::string& value) {
    validate(value);
    return static_cast<std::int64_t>(value.size());
}

inline std::string stringFromInt(std::int64_t value) {
    char buffer[21];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (converted.ec != std::errc{})
        throw ExecutionFailure("integer text conversion failed");
    return std::string(buffer, converted.ptr);
}

inline std::string utf8Slice(const std::string& value, std::int64_t start, std::int64_t count) {
    validate(value);
    const auto position = checkedUtf8Offset(value, start);
    if (count < 0 || static_cast<std::uint64_t>(count) > value.size() - position)
        throw ExecutionFailure("UTF-8 byte count out of range");
    const auto length = static_cast<std::size_t>(count);
    if (!isUtf8Boundary(value, position + length))
        throw ExecutionFailure("UTF-8 slice end splits a scalar");
    return value.substr(position, length);
}

inline std::int64_t utf8IndexOf(const std::string& value, const std::string& pattern, std::int64_t start) {
    validate(value);
    validate(pattern);
    const auto position = checkedUtf8Offset(value, start);
    const auto found = value.find(pattern, position);
    return found == std::string::npos ? -1 : static_cast<std::int64_t>(found);
}

// A transaction owns its working copy. No authored partial mutation escapes a
// failed synchronous action. Async methods commit at each await and successful
// return, then refresh from live state when resumed by the owner executor.
template<class T>
class Transaction {
public:
    explicit Transaction(T& target) : target_(target), working_(target) {}
    T& state() { return working_; }
    const T& state() const { return working_; }
    void commit() {
        static_assert(std::is_nothrow_swappable_v<T>);
        using std::swap;
        swap(target_, working_);
    }
    void refresh() { working_ = target_; }
private:
    T& target_;
    T working_;
};

}
