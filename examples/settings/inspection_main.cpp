#include "Inspection.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void click(pipeview_settings::Application& application, const std::string& identity)
{
    std::string error;
    const auto session = application.session();
    require(session->layout(600, 440, error), error);
    const auto bounds = session->bounds(identity);
    require(session->pointerDown(bounds.x + 2, bounds.y + 2, error), error);
    require(application.dispatch(session->takeEvents(), error), error);
}

void editName(pipeview_settings::Application& application, const std::string& text)
{
    std::string error;
    click(application, "name-field");
    require(application.session()->textInput(text, error), error);
    require(application.dispatch(application.session()->takeEvents(), error), error);
}

void finishSave(pipeview_settings::Application& application)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string error;
    while (application.snapshot().saving) {
        require(application.pumpSave(error), error);
        require(std::chrono::steady_clock::now() < deadline, "Inspection save completion deadline exceeded");
        std::this_thread::yield();
    }
}

struct SaveGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;

    pipeview_settings::SaveResult persist()
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; }))
            return {pipeview_settings::model::Outcome::Failure, "Inspection save gate deadline exceeded"};
        return {pipeview_settings::model::Outcome::Success, {}};
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

struct ReleaseGate {
    std::shared_ptr<SaveGate> gate;
    ~ReleaseGate() { gate->release(); }
};

}

// Private pure-UI fixture: ROOT OUTPUT must be absolute paths. An owned unique
// directory under ROOT supplies an absent load path; persistence is an injected
// gated function with no I/O. OUTPUT receives six inspection frames.
int main(int argc, char** argv)
{
    try {
        require(argc == 3 && std::filesystem::path(argv[1]).is_absolute()
                && std::filesystem::path(argv[2]).is_absolute(),
            "Absolute inspection fixture ROOT and OUTPUT are required");
        std::filesystem::create_directories(argv[1]);
        std::string pattern = std::string(argv[1]) + "/inspection-XXXXXX";
        std::vector<char> directory(pattern.begin(), pattern.end());
        directory.push_back('\0');
        require(::mkdtemp(directory.data()) != nullptr, "Cannot create owned inspection fixture");
        const auto path = std::filesystem::path(directory.data()) / "settings.data";
        auto gate = std::make_shared<SaveGate>();
        std::string error;
        auto application = pipeview_settings::Application::create(path, error,
            [gate](pipeview_settings::SettingsValue) { return gate->persist(); });
        require(bool(application), error);
        ReleaseGate releaseGate{gate};
        auto foreign = std::async(std::launch::async, [&] {
            try {
                pipeview_settings::inspect(*application);
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        });
        require(foreign.get(), "Foreign inspection accessed owner state");
        const std::set<std::string> exposed{"playerName", "vsync", "volume", "save.status"};
        const auto initial = pipeview_settings::inspect(*application, exposed);
        const auto redacted = pipeview_settings::inspect(*application);
        bool refusedUnknownExposure = false;
        try {
            pipeview_settings::inspect(*application, {"unknown-binding"});
        } catch (const std::runtime_error&) {
            refusedUnknownExposure = true;
        }
        require(refusedUnknownExposure, "Inspection accepted an unknown exposure identity");
        require(application->session()->setValue("playerName", std::string{"Presentation staging"}, error), error);
        require(pipeview_settings::inspect(*application, exposed) == initial,
            "Inspection used presentation staging instead of authoritative state");
        require(application->session()->setValue("playerName", std::string{"Rider"}, error), error);
        editName(*application, "X");
        click(*application, "vsync-toggle");
        click(*application, "volume-stepper");
        const auto edited = pipeview_settings::inspect(*application, exposed);
        click(*application, "save-button");
        require(application->snapshot().saving, "Inspection fixture did not start a save");
        const auto saving = pipeview_settings::inspect(*application, exposed);
        gate->release();
        finishSave(*application);
        require(!application->snapshot().dirty && application->snapshot().status == "Saved",
            "Inspection fixture save did not succeed");
        const auto saved = pipeview_settings::inspect(*application, exposed);
        editName(*application, "Z");
        click(*application, "revert-button");
        const auto reverted = pipeview_settings::inspect(*application, exposed);
        require(!std::filesystem::exists(path), "Injected inspection persistence unexpectedly wrote a file");
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        require(bool(output), "Cannot open inspection report");
        output << "{\"initial\":" << initial << ",\"default\":" << redacted
               << ",\"edited\":" << edited << ",\"saving\":" << saving
               << ",\"saved\":" << saved << ",\"reverted\":" << reverted << "}\n";
        output.close();
        require(bool(output), "Cannot write inspection report");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Settings inspection: %s\n", error.what());
        return 1;
    }
}
