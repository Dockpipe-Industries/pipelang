// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "result.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace pipelang::model {

class CancellationToken {
public:
    bool is_cancelled() const {
        for (const auto& flag : flags_) {
            if (flag->load(std::memory_order_acquire))
                return true;
        }
        return false;
    }
    static CancellationToken combine(CancellationToken left, const CancellationToken& right) {
        for (const auto& flag : right.flags_) {
            if (std::find(left.flags_.begin(), left.flags_.end(), flag) == left.flags_.end())
                left.flags_.push_back(flag);
        }
        if (left.flags_.size() > 64)
            throw ExecutionFailure("cancellation ancestry limit exceeded");
        return left;
    }
private:
    friend class CancellationSource;
    std::vector<std::shared_ptr<std::atomic<bool>>> flags_;
};

class CancellationSource {
public:
    CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    CancellationToken token() const {
        CancellationToken result;
        result.flags_.push_back(flag_);
        return result;
    }
    void cancel() const { flag_->store(true, std::memory_order_release); }
private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

}
