#include "Example.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Application = pipeview_settings::Application;
using Outcome = pipeview_settings::model::Outcome;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    bool finished = false;
    int calls = 0;
    std::thread::id worker;
    pipeview_settings::SettingsValue captured;

    pipeview_settings::SaveResult persist(pipeview_settings::SettingsValue value)
    {
        std::unique_lock lock(mutex);
        worker = std::this_thread::get_id();
        captured = std::move(value);
        ++calls;
        entered = true;
        condition.notify_all();
        if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return released; }))
            return {Outcome::Failure, "Fixture gate deadline exceeded"};
        finished = true;
        condition.notify_all();
        return {Outcome::Success, {}};
    }

    void wait(bool Gate::*flag)
    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(5), [&] { return this->*flag; }),
            "Fixture handshake deadline exceeded");
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
};

struct Release {
    std::shared_ptr<Gate> gate;
    ~Release() { gate->release(); }
};

void click(Application &application, const std::string &identity)
{
    std::string error;
    const auto session = application.session();
    require(session->layout(600, 440, error), error);
    const auto bounds = session->bounds(identity);
    require(session->pointerDown(bounds.x + 2, bounds.y + 2, error), error);
    require(application.dispatch(session->takeEvents(), error), error);
}

void edit(Application &application, const std::string &text)
{
    click(application, "name-field");
    std::string error;
    require(application.session()->textInput(text, error), error);
    require(application.dispatch(application.session()->takeEvents(), error), error);
}

void finish(Application &application)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    std::string error;
    while (application.snapshot().saving) {
        require(application.pumpSave(error), error);
        require(Clock::now() < deadline, "Owned completion deadline exceeded");
        std::this_thread::yield();
    }
}
}

int main(int argc, char **argv)
{
    try {
        require(argc == 2 && std::filesystem::path(argv[1]).is_absolute(), "Absolute owned fixture root required");
        std::filesystem::create_directories(argv[1]);
        std::string pattern = std::string(argv[1]) + "/run-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        require(::mkdtemp(name.data()) != nullptr, "Cannot create owned fixture");
        const std::filesystem::path root(name.data());
        std::string error;
        const auto owner = std::this_thread::get_id();
        auto gate = std::make_shared<Gate>();
        auto application = Application::create(root / "gate.data", error,
            [gate](auto value) { return gate->persist(std::move(value)); });
        require(bool(application), error);
        Release release{gate};
        edit(*application, "X");
        click(*application, "vsync-toggle");
        click(*application, "volume-stepper");
        click(*application, "save-button");
        gate->wait(&Gate::entered);
        {
            std::lock_guard lock(gate->mutex);
            require(gate->worker != owner && gate->calls == 1, "Persistence did not run on one worker");
            require(gate->captured.playerName == "RiderX" && !gate->captured.vsync && gate->captured.volume == 99, "Worker received the wrong snapshot");
        }
        require(application->pumpSave(error), error);
        require(application->snapshot().saving, "Pump waited for blocked storage");
        edit(*application, "Y");
        click(*application, "volume-stepper");
        click(*application, "save-button");
        click(*application, "revert-button");
        require(std::get<std::string>(application->snapshot().draft.at("playerName")) == "RiderX", "Busy input changed capture");
        require(std::get<std::int64_t>(application->snapshot().draft.at("volume")) == 99, "Busy volume input changed capture");
        auto foreign = std::async(std::launch::async, [&] {
            std::string failure;
            return !application->pumpSave(failure) && !application->dispatch({}, failure);
        });
        require(foreign.get(), "Foreign thread reached owner state or completion");
        gate->release();
        gate->wait(&Gate::finished);
        require(application->snapshot().saving, "Worker completed the model from its own thread");
        finish(*application);
        require(!application->snapshot().dirty, "Owner did not apply completion");
        {
            std::lock_guard lock(gate->mutex);
            require(gate->calls == 1, "Busy Save started duplicate persistence");
        }
        for (int mode = 0; mode < 5; ++mode) {
            auto calls = std::make_shared<std::atomic<int>>(0);
            auto retry = Application::create(root / ("retry" + std::to_string(mode)), error,
                [calls, mode](auto) -> pipeview_settings::SaveResult {
                    if (++*calls > 1)
                        return {Outcome::Success, {}};
                    if (mode == 0)
                        return {Outcome::Failure, "Expected storage refusal"};
                    if (mode == 1)
                        throw std::runtime_error("Unexpected fixture failure");
                    if (mode == 2)
                        throw 7;
                    if (mode == 3)
                        return {static_cast<Outcome>(999), "Invalid fixture outcome"};
                    return {Outcome::Cancelled, {}};
                });
            require(bool(retry), error);
            edit(*retry, "X");
            click(*retry, "save-button");
            finish(*retry);
            const auto expectedStatus = mode == 4 ? "Save cancelled" : "Save failed";
            require(retry->snapshot().dirty && retry->snapshot().status == expectedStatus
                && (mode == 4 || !retry->storageError().empty()), "Worker outcome lost retry state");
            click(*retry, "save-button");
            finish(*retry);
            require(!retry->snapshot().dirty && *calls == 2, "Retry did not launch one fresh worker");
        }
        auto closingGate = std::make_shared<Gate>();
        auto closing = std::make_shared<std::promise<void>>();
        auto closed = std::make_shared<std::promise<void>>();
        auto closingSignal = closing->get_future();
        auto closedSignal = closed->get_future();
        std::future<void> closingOwner;
        Release releaseClosing{closingGate};
        closingOwner = std::async(std::launch::async, [=] {
            std::string failure;
            auto owned = Application::create(root / "closing.data", failure,
                [closingGate](auto value) { return closingGate->persist(std::move(value)); });
            require(bool(owned), failure);
            edit(*owned, "X");
            click(*owned, "save-button");
            closingGate->wait(&Gate::entered);
            closing->set_value();
            owned.reset();
            {
                std::lock_guard lock(closingGate->mutex);
                require(closingGate->finished, "Teardown completed before owned work");
            }
            closed->set_value();
        });
        require(closingSignal.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "Owner did not begin teardown");
        require(closedSignal.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready,
            "Teardown escaped owned persistence");
        closingGate->release();
        require(closedSignal.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "Teardown failed to join the worker");
        closingOwner.get();
        std::puts("pipeview_async: worker_capture_nonblocking_owner_completion_failure_retry_join=passed");
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "Owned Save worker: %s\n", failure.what());
        return 1;
    }
}
