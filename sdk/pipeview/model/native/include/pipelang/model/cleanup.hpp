// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "collection.hpp"

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace pipelang::model {

// Source cleanup runs once after its protected scope exits. Failure on a normal
// exit replaces that exit; failure during unwinding preserves the primary fault.
// Forced frame destruction must never propagate an exception through destroy().
template<class Function, class Abandoning>
class Finally {
public:
    Finally(Function function, Abandoning abandoning)
        : function_(std::move(function)), abandoning_(std::move(abandoning)),
          exceptions_(std::uncaught_exceptions()) {}
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;
    ~Finally() noexcept(false) {
        try {
            function_();
        } catch (...) {
            if (std::uncaught_exceptions() > exceptions_ || abandoning_())
                return;
            throw;
        }
    }
private:
    Function function_;
    Abandoning abandoning_;
    int exceptions_;
};

class AsyncFinallyFailure {
public:
    void captureFailure(std::exception_ptr failure) noexcept { failure_ = std::move(failure); }
    void captureCleanupFailure(std::exception_ptr failure) noexcept {
        if (!failure_)
            failure_ = std::move(failure);
    }
    void rethrowFailure() const {
        if (failure_)
            std::rethrow_exception(failure_);
    }

private:
    std::exception_ptr failure_;
};

// Owned pending exit for a resumable source finally. Native handlers capture
// only; generated coroutine code executes cleanup outside handlers/destructors.
template<class T, bool StoresReturn = true>
class AsyncFinallyExit {
public:
    void returning(T value) requires (StoresReturn) {
        returned_.emplace(std::move(value));
        kind_ = Kind::Return;
    }
    void breaking() noexcept { kind_ = Kind::Break; }
    void continuing() noexcept { kind_ = Kind::Continue; }
    bool isReturn() const noexcept requires (StoresReturn) { return kind_ == Kind::Return; }
    bool isBreak() const noexcept { return kind_ == Kind::Break; }
    bool isContinue() const noexcept { return kind_ == Kind::Continue; }
    T& returnValue() requires (StoresReturn) { return *returned_; }
    void captureFailure() noexcept { failure_.captureFailure(std::current_exception()); }
    void captureCleanupFailure() noexcept { failure_.captureCleanupFailure(std::current_exception()); }
    void rethrowFailure() const { failure_.rethrowFailure(); }

private:
    enum class Kind { Normal, Return, Break, Continue };
    struct EmptyReturnStorage {};
    using ReturnStorage = std::conditional_t<StoresReturn, std::optional<T>, EmptyReturnStorage>;

    // Keep this order: captured failure is released before a stored result.
    // A region without a return owns no T and has no return operations.
    Kind kind_ = Kind::Normal;
    ReturnStorage returned_;
    AsyncFinallyFailure failure_;
};

template<class T, class Abandoning>
class CommitOnReturn {
public:
    CommitOnReturn(Transaction<T>& transaction, Abandoning abandoning)
        : transaction_(transaction), abandoning_(std::move(abandoning)),
          exceptions_(std::uncaught_exceptions()) {}
    CommitOnReturn(const CommitOnReturn&) = delete;
    CommitOnReturn& operator=(const CommitOnReturn&) = delete;
    ~CommitOnReturn() noexcept(false) {
        if (armed_ && !abandoning_() && std::uncaught_exceptions() == exceptions_)
            transaction_.commit();
    }
    void arm() noexcept { armed_ = true; }
private:
    Transaction<T>& transaction_;
    Abandoning abandoning_;
    int exceptions_;
    bool armed_ = false;
};

}
