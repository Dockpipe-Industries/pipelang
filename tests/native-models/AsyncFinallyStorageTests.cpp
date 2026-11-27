// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/cleanup.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pipelang::model;
using IntResult = Result<std::int64_t>;
using ReturningExit = AsyncFinallyExit<IntResult>;
using NonreturningExit = AsyncFinallyExit<IntResult, false>;
std::size_t assertions = 0;

template<class Exit>
concept CanStoreReturn = requires(Exit& exit, IntResult value) {
    exit.returning(std::move(value));
};

template<class Exit>
concept CanReadReturn = requires(Exit& exit) {
    exit.returnValue();
};

template<class Exit>
concept CanTestReturn = requires(const Exit& exit) {
    exit.isReturn();
};

// A nonreturning region must not require its declared return type to be owned.
// Deleted construction/destruction makes accidental storage a compile failure.
struct UnownableValue {
    UnownableValue() = delete;
    ~UnownableValue() = delete;
};

static_assert(CanStoreReturn<ReturningExit> && CanReadReturn<ReturningExit> && CanTestReturn<ReturningExit>);
static_assert(!CanStoreReturn<NonreturningExit>);
static_assert(!CanReadReturn<NonreturningExit>);
static_assert(!CanTestReturn<NonreturningExit>);
static_assert(std::is_default_constructible_v<AsyncFinallyExit<UnownableValue, false>>);
static_assert(std::is_destructible_v<AsyncFinallyExit<UnownableValue, false>>);
static_assert(std::is_move_constructible_v<ReturningExit>);
static_assert(std::is_move_constructible_v<NonreturningExit>);
static_assert(sizeof(NonreturningExit) < sizeof(ReturningExit));

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

template<class Exit>
void requireFailure(const Exit& exit, const std::string& expected) {
    try {
        exit.rethrowFailure();
    } catch (const ExecutionFailure& error) {
        require(error.what() == expected, "pending failure identity changed");
        return;
    }
    throw std::runtime_error("pending failure was not rethrown");
}

void nonreturningControlAndFailures() {
    NonreturningExit exit;
    require(!exit.isBreak() && !exit.isContinue(), "normal exit gained a loop transfer");
    exit.rethrowFailure();
    exit.breaking();
    require(exit.isBreak() && !exit.isContinue(), "break was not retained");
    exit.continuing();
    require(!exit.isBreak() && exit.isContinue(), "continue was not retained");
    try {
        throw ExecutionFailure("primary");
    } catch (...) {
        exit.captureFailure();
    }
    try {
        throw ExecutionFailure("cleanup");
    } catch (...) {
        exit.captureCleanupFailure();
    }
    requireFailure(exit, "primary");
    require(exit.isContinue(), "capturing a failure changed the pending transfer");

    NonreturningExit cleanupOnly;
    try {
        throw ExecutionFailure("cleanup-only");
    } catch (...) {
        cleanupOnly.captureCleanupFailure();
    }
    requireFailure(cleanupOnly, "cleanup-only");

    AsyncFinallyExit<UnownableValue, false> noValue;
    noValue.breaking();
    require(noValue.isBreak(), "return-free state unexpectedly needs an owned value");
}

class Lifetime final {
public:
    Lifetime(std::vector<int>& events, int identity) : events_(events), identity_(identity) {}
    ~Lifetime() { events_.push_back(identity_); }
    Lifetime(const Lifetime&) = delete;
    Lifetime& operator=(const Lifetime&) = delete;
private:
    std::vector<int>& events_;
    int identity_;
};

void returnEvaluationAndOwnership() {
    std::vector<int> events;
    events.reserve(4);
    int evaluations = 0;
    auto create = [&] {
        ++evaluations;
        return std::make_unique<Lifetime>(events, 2);
    };
    std::unique_ptr<Lifetime> result;
    {
        AsyncFinallyExit<std::unique_ptr<Lifetime>> exit;
        exit.returning(create());
        require(evaluations == 1 && exit.isReturn(), "return value was not evaluated once");
        require(events.empty() && exit.returnValue() != nullptr, "pending return lost owned value");
        exit.rethrowFailure();
        result = std::move(exit.returnValue());
    }
    require(events.empty() && result != nullptr, "exit destruction released the transferred result");
    result.reset();
    require(events == std::vector<int>{2}, "owned return was destroyed more than once");
}

void destructionOrder() {
    std::vector<int> events;
    events.reserve(4);
    {
        AsyncFinallyExit<std::unique_ptr<Lifetime>> exit;
        exit.returning(std::make_unique<Lifetime>(events, 2));
        try {
            throw std::make_shared<Lifetime>(events, 1);
        } catch (...) {
            exit.captureFailure();
        }
        require(events.empty(), "captured fault or returned owner died before region destruction");
    }
    require(events == std::vector<int>({1, 2}),
        "region must release its captured fault before its stored return");

    events.clear();
    {
        AsyncFinallyExit<std::unique_ptr<Lifetime>, false> exit;
        try {
            throw std::make_shared<Lifetime>(events, 1);
        } catch (...) {
            exit.captureFailure();
        }
        require(events.empty(), "return-free region released its fault too early");
    }
    require(events == std::vector<int>{1}, "return-free region lost its fault lifetime");
}
}

int main() {
    try {
        nonreturningControlAndFailures();
        returnEvaluationAndOwnership();
        destructionOrder();
        std::cout << "async finally storage: " << assertions << " runtime assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
