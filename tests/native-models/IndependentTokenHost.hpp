// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Model.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace token_test {
#ifdef PIPELANG_TOKENS_PROJECT
using App = independent_tokens::source::Tokens::App;
using Receipt = independent_tokens::source::Tokens::TokenReceipt;
using TokenHost = independent_tokens::source::Tokens::TokenHost;
using Stamp = independent_tokens::source::Tokens::ScopeStamp;
#else
using App = independent_tokens::App;
using Receipt = independent_tokens::TokenReceipt;
using TokenHost = independent_tokens::TokenHost;
using Stamp = independent_tokens::ScopeStamp;
#endif
using namespace pipelang::model;
using ReceiptTask = Task<Result<Receipt>>;
inline std::size_t assertions = 0;

inline void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

class Host final : public TokenHost {
public:
    Result<std::int64_t> Step(std::int64_t phase, CancellationToken token) override {
        std::unique_lock lock(mutex_);
        phases_.push_back(phase);
        changed_.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (held_[phase] && (authoritative_[phase] || !token.is_cancelled())) {
            if (std::chrono::steady_clock::now() >= deadline)
                return Result<std::int64_t>::Failure("token fixture gate deadline");
            changed_.wait_for(lock, std::chrono::milliseconds(1));
        }
        if (token.is_cancelled() && !authoritative_[phase])
            return Result<std::int64_t>::Cancelled("selected operation token cancelled");
        return Result<std::int64_t>::Success(phase);
    }
    void hold(std::int64_t phase, bool authoritative = false) {
        std::lock_guard lock(mutex_);
        held_[phase] = true;
        authoritative_[phase] = authoritative;
    }
    void release(std::int64_t phase) {
        std::lock_guard lock(mutex_);
        held_[phase] = false;
        changed_.notify_all();
    }
    bool entered(std::int64_t phase) const {
        const auto values = phases();
        return std::find(values.begin(), values.end(), phase) != values.end();
    }
    std::vector<std::int64_t> phases() const {
        std::lock_guard lock(mutex_);
        return phases_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::int64_t, bool> held_;
    std::map<std::int64_t, bool> authoritative_;
    std::vector<std::int64_t> phases_;
};

template<class Pump, class Predicate>
void pumpUntil(Pump pump, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        pump();
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("token fixture owner deadline");
        std::this_thread::yield();
    }
}

template<class T>
void finish(App& app, const Task<T>& task) {
    pumpUntil([&] { app.poll(); }, [&] { return task.completed(); });
}

inline const Receipt& receipt(const ReceiptTask& task) {
    require(task.completed() && task.result().is_success(), "source token method did not complete with its owned receipt");
    return task.result().value();
}
}
