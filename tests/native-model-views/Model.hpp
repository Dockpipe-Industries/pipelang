// Independent generated-view oracle: this model deliberately uses ordinary
// C++ to test view compilation independently from the model compiler.
#pragma once
#include <pipelang/model/runtime.hpp>
#include <condition_variable>
#include <mutex>

namespace pipelang_models {
struct Values {
    std::string Name = "Before";
    bool Enabled = false;
    std::int64_t Volume = 50;
};
class FixtureModel {
public:
    struct State {
        Values Draft;
        bool Saving = false;
        std::string Error;
    };
    State snapshot() const { return state_; }
    bool CanSave() const { return !state_.Saving && !state_.Draft.Name.empty(); }
    pipelang::model::InvocationResult SetName(std::string value)
    {
        state_.Draft.Name = std::move(value);
        return pipelang::model::InvocationResult::Success();
    }
    pipelang::model::InvocationResult SetEnabled(bool value)
    {
        state_.Draft.Enabled = value;
        return pipelang::model::InvocationResult::Success();
    }
    pipelang::model::InvocationResult SetVolume(std::int64_t value)
    {
        state_.Draft.Volume = value;
        return pipelang::model::InvocationResult::Success();
    }
    pipelang::model::InvocationResult Fail()
    {
        state_.Error = "Expected failure";
        return pipelang::model::InvocationResult::Failure(state_.Error);
    }
    pipelang::model::InvocationResult Cancel()
    {
        return pipelang::model::InvocationResult::Cancelled("Expected cancellation");
    }
    auto Save(pipelang::model::CancellationToken cancellation)
    {
        return tasks_.start<pipelang::model::InvocationResult>([this, cancellation](auto ownedCancellation) {
            return save(pipelang::model::CancellationToken::combine(cancellation, ownedCancellation));
        });
    }
    std::size_t poll() { return tasks_.poll(); }
    void waitUntilEntered()
    {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return entered_; });
    }
    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        wake_.notify_all();
    }
private:
    pipelang::model::Coroutine<pipelang::model::InvocationResult> save(pipelang::model::CancellationToken cancellation)
    {
        state_.Saving = true;
        auto result = co_await tasks_.host<pipelang::model::InvocationResult>([this, cancellation] {
            std::unique_lock lock(mutex_);
            entered_ = true;
            wake_.notify_all();
            wake_.wait(lock, [this] { return released_; });
            if (cancellation.is_cancelled())
                return pipelang::model::InvocationResult::Cancelled("Save cancelled");
            return pipelang::model::InvocationResult::Success();
        }, cancellation);
        state_.Saving = false;
        co_return result;
    }
    State state_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool entered_ = false;
    bool released_ = false;
    pipelang::model::TaskScope tasks_;
};
}
