// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "native_app_abi.hpp"

#include <pipelang/model/result.hpp>

#include <memory_resource>
#include <set>
#include <string>
#include <utility>

namespace pipelang::native_app_v19 {

class NativeTextPool {
    class StorageBudget final : public std::pmr::memory_resource {
    public:
        StorageBudget(std::size_t limit, const char* diagnostic) : limit_(limit), diagnostic_(diagnostic) {}

        void charge(std::size_t bytes)
        {
            if (bytes > limit_ - bytes_)
                throw model::EvaluationLimitFailure(diagnostic_);
            bytes_ += bytes;
        }

        void release(std::size_t bytes) noexcept { bytes_ -= bytes; }

    private:
        void* do_allocate(std::size_t bytes, std::size_t alignment) override
        {
            charge(bytes);
            try {
                return std::pmr::new_delete_resource()->allocate(bytes, alignment);
            } catch (...) {
                release(bytes);
                throw;
            }
        }

        void do_deallocate(void* storage, std::size_t bytes, std::size_t alignment) override
        {
            std::pmr::new_delete_resource()->deallocate(storage, bytes, alignment);
            release(bytes);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
        {
            return this == &other;
        }

        std::size_t limit_;
        const char* diagnostic_;
        std::size_t bytes_ = 0;
    };

public:
    NativeTextPool(std::size_t limit, const char* diagnostic) : budget_(limit, diagnostic), values_(&budget_) {}

    NativeTextPool(const NativeTextPool&) = delete;
    NativeTextPool& operator=(const NativeTextPool&) = delete;

    TextView retain(std::string value)
    {
        const auto existing = values_.find(value);
        if (existing != values_.end())
            return {existing->data(), existing->size()};

        // Account for retained string capacity in addition to the exact node
        // allocation. Counting inline capacity keeps the bound conservative.
        const auto storage = value.capacity() + 1;
        budget_.charge(storage);
        try {
            const auto stored = values_.insert(std::move(value)).first;
            return {stored->data(), stored->size()};
        } catch (...) {
            budget_.release(storage);
            throw;
        }
    }

private:
    StorageBudget budget_;
    std::pmr::set<std::string> values_;
};

}
