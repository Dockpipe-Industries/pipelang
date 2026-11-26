// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NativeCellSlots.hpp"
#include "native_app_abi.hpp"

#include <pipelang/model/context.hpp>
#include <pipelang/model/synchronization.hpp>

#include <memory>

#include <utility>

namespace pipelang::native_app_v19 {

// Bit 63 belongs to continuation-local atomic handles.
inline constexpr std::uint64_t hostedSynchronizationTag = std::uint64_t{1} << 62;

class NativeContextOwners {
public:
    std::uint64_t retain(std::shared_ptr<model::ContextOwner> owner)
    {
        return cells_.retain(std::move(owner), "native context storage limit exceeded") + 1;
    }

    const std::shared_ptr<model::ContextOwner>& at(std::uint64_t handle) const
    {
        const auto* owner = cells_.find(handle - 1);
        if (!owner)
            throw model::ExecutionFailure("native context owner does not belong to the invocation or its binding");
        return *owner;
    }

    void apply(std::uint64_t handle, ContextOwnerOperation operation) const
    {
        auto& owner = *at(handle);
        switch (operation) {
        case ContextOwnerOperation::Replace: (void)owner.replace(); return;
        case ContextOwnerOperation::Invalidate: owner.invalidate(); return;
        case ContextOwnerOperation::Close: owner.close(); return;
        }
        throw model::ExecutionFailure("invalid native context owner operation");
    }

    std::size_t checkpoint() const noexcept { return cells_.checkpoint(); }
    void restore(std::size_t checkpoint) noexcept { cells_.restore(checkpoint); }
    void retire(std::uint64_t handle) noexcept { cells_.retire(handle - 1); }

private:
    NativeCellSlots<std::shared_ptr<model::ContextOwner>> cells_;
};

class InvocationSemaphores {
public:
    static std::int64_t handleFor(std::size_t index) noexcept
    {
        return static_cast<std::int64_t>(maximumSemaphoreSites + index);
    }

    std::size_t checkpoint() const noexcept { return cells_.checkpoint(); }

    void restore(std::size_t checkpoint) noexcept { cells_.restore(checkpoint); }

    void retire(std::uint64_t handle) noexcept { cells_.retire(handle - maximumSemaphoreSites); }

    std::int64_t retain(model::SemaphoreSlim semaphore)
    {
        return handleFor(cells_.retain(std::move(semaphore), "native invocation semaphore storage limit exceeded"));
    }

    const model::SemaphoreSlim& at(std::int64_t handle) const
    {
        const auto index = static_cast<std::uint64_t>(handle) - maximumSemaphoreSites;
        const auto* cell = cells_.find(index);
        if (!cell)
            throw model::ExecutionFailure("native semaphore does not belong to the current invocation");
        return *cell;
    }

private:
    NativeCellSlots<model::SemaphoreSlim> cells_;
};

class InvocationLocks {
public:
    std::size_t checkpoint() const noexcept { return cells_.checkpoint(); }

    void restore(std::size_t checkpoint) noexcept { cells_.restore(checkpoint); }

    void retire(std::uint64_t handle) noexcept { cells_.retire(handle - 1); }

    std::uint64_t create()
    {
        return borrow(model::Lock{});
    }

    std::uint64_t borrow(const model::Lock& lock)
    {
        return cells_.retain(lock, "native invocation lock storage limit exceeded") + 1;
    }

    const model::Lock& at(std::uint64_t handle) const
    {
        const auto* cell = cells_.find(handle - 1);
        if (!cell)
            throw model::ExecutionFailure("native lock does not belong to the current invocation");
        return *cell;
    }

private:
    NativeCellSlots<model::Lock> cells_;
};

}
