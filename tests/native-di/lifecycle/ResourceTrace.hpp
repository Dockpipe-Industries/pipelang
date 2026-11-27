// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResourceEvent.hpp"
#include <pipelang/model/runtime.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lifecycle_test {

// Test-only external resource ledger. Effects intentionally survive source
// rollback; only authored factory/cleanup calls can release these resources.
class ResourceTrace {
public:
    enum class CloseMode {
        Success,
        FailureAfterRelease,
        CancelledAfterRelease,
        ThrowBeforeRelease
    };

    pipelang::model::Result<std::int64_t> Open(std::string kind, std::int64_t context) {
        std::int64_t id;
        {
            std::lock_guard lock(mutex_);
            id = nextId_++;
            resources_.emplace(id, Resource{kind, context});
            events_.push_back({"open", id, kind, context});
        }
        if (onOpen)
            onOpen(id, kind, context);
        return pipelang::model::Result<std::int64_t>::Success(id);
    }

    pipelang::model::Result<void> Close(std::int64_t id) {
        CloseMode mode;
        {
            std::lock_guard lock(mutex_);
            auto& resource = Checked(id);
            ++resource.closeAttempts;
            events_.push_back({"close-attempt", id, resource.kind, resource.context});
            mode = resource.closeMode;
        }
        if (onClose)
            onClose(id);
        if (mode == CloseMode::ThrowBeforeRelease)
            throw pipelang::model::ExecutionFailure("fixture.close_before_effect");
        {
            std::lock_guard lock(mutex_);
            auto& resource = Checked(id);
            if (!resource.open)
                return pipelang::model::Result<void>::Failure("fixture.double_close");
            resource.open = false;
            events_.push_back({"close-effect", id, resource.kind, resource.context});
        }
        if (mode == CloseMode::FailureAfterRelease)
            return pipelang::model::Result<void>::Failure("fixture.close_after_effect");
        if (mode == CloseMode::CancelledAfterRelease)
            return pipelang::model::Result<void>::Cancelled("fixture.close_after_effect");
        return pipelang::model::Result<void>::Success();
    }

    pipelang::model::Result<void> Note(
        std::string operation, std::int64_t id, std::int64_t dependency) {
        {
            std::lock_guard lock(mutex_);
            const auto& resource = Checked(id);
            events_.push_back({operation, id, resource.kind, resource.context, dependency});
        }
        if (onNote)
            onNote(operation, id, dependency);
        return pipelang::model::Result<void>::Success();
    }

    pipelang::model::Result<std::int64_t> Wait(
        std::int64_t id, pipelang::model::CancellationToken cancellation) {
        std::unique_lock lock(mutex_);
        auto& resource = Checked(id);
        if (!resource.open)
            return pipelang::model::Result<std::int64_t>::Failure("fixture.wait_closed");
        resource.waiting = true;
        events_.push_back({"wait-enter", id, resource.kind, resource.context});
        changed_.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!resource.completeWait && !cancellation.is_cancelled()) {
            if (changed_.wait_until(lock, std::min(deadline,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(2))) == std::cv_status::timeout &&
                std::chrono::steady_clock::now() >= deadline) {
                resource.waiting = false;
                events_.push_back({"wait-timeout", id, resource.kind, resource.context});
                return pipelang::model::Result<std::int64_t>::Failure("fixture.wait_timeout");
            }
        }
        resource.waiting = false;
        if (cancellation.is_cancelled()) {
            events_.push_back({"wait-cancelled", id, resource.kind, resource.context});
            return pipelang::model::Result<std::int64_t>::Cancelled("fixture.wait_cancelled");
        }
        events_.push_back({"wait-success", id, resource.kind, resource.context});
        return pipelang::model::Result<std::int64_t>::Success(id);
    }

    bool WaitEntered(std::int64_t id) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(3), [&] { return Checked(id).waiting; });
    }

    void CompleteWait(std::int64_t id) {
        std::lock_guard lock(mutex_);
        Checked(id).completeWait = true;
        changed_.notify_all();
    }

    void SetCloseMode(std::int64_t id, CloseMode mode) {
        std::lock_guard lock(mutex_);
        Checked(id).closeMode = mode;
    }

    std::vector<ResourceEvent> Events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }

    void Dump(std::ostream& output) const {
        const auto events = Events();
        for (std::size_t index = 0; index < events.size(); ++index) {
            const auto& event = events[index];
            output << index << ": " << event.operation << " resource=" << event.resource
                   << " kind=" << event.kind << " context=" << event.context
                   << " dependency=" << event.dependency << '\n';
        }
    }

    std::size_t OpenCount() const {
        std::lock_guard lock(mutex_);
        std::size_t count = 0;
        for (const auto& [id, resource] : resources_) {
            if (resource.open)
                ++count;
        }
        return count;
    }

    std::size_t CloseAttempts(std::int64_t id) const {
        std::lock_guard lock(mutex_);
        const auto found = resources_.find(id);
        if (found == resources_.end())
            throw std::runtime_error("fixture.unknown_resource");
        return found->second.closeAttempts;
    }

    // Configure before worker admission; callbacks execute outside the ledger
    // lock so the fixture can deliberately challenge owner/reentry admission.
    std::function<void(std::int64_t, const std::string&, std::int64_t)> onOpen;
    std::function<void(std::int64_t)> onClose;
    std::function<void(const std::string&, std::int64_t, std::int64_t)> onNote;

private:
    struct Resource {
        std::string kind;
        std::int64_t context;
        bool open = true;
        bool waiting = false;
        bool completeWait = false;
        std::size_t closeAttempts = 0;
        CloseMode closeMode = CloseMode::Success;
    };

    Resource& Checked(std::int64_t id) {
        const auto found = resources_.find(id);
        if (found == resources_.end())
            throw std::runtime_error("fixture.unknown_resource");
        return found->second;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::int64_t nextId_ = 1;
    std::map<std::int64_t, Resource> resources_;
    std::vector<ResourceEvent> events_;
};
}
