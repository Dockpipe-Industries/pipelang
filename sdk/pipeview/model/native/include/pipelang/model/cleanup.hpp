// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "collection.hpp"

#include <exception>
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

template<class T, class Abandoning>
class CommitOnReturn {
public:
    CommitOnReturn(Transaction<T>& transaction, Abandoning abandoning)
        : transaction_(transaction), abandoning_(std::move(abandoning)),
          exceptions_(std::uncaught_exceptions()) {}
    CommitOnReturn(const CommitOnReturn&) = delete;
    CommitOnReturn& operator=(const CommitOnReturn&) = delete;
    ~CommitOnReturn() noexcept {
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
