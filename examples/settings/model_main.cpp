#include "Example.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

namespace {
class StorageStub final : public settings_models::SettingsStorage {
public:
    pipeview_settings::SaveResult Save(settings_models::SettingsValues,
        pipelang::model::CancellationToken) override
    {
        return pipeview_settings::SaveResult::Success();
    }
};
void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string playerName(const settings_models::SettingsSnapshot &snapshot)
{
    return snapshot.draft.playerName;
}

void click(pipeview_settings::Application &application, const std::string &identity)
{
    std::string error;
    const auto session = application.session();
    require(session->layout(600, 440, error), error);
    const auto bounds = session->bounds(identity);
    require(session->pointerDown(bounds.x + 2, bounds.y + 2, error), error);
    require(application.dispatch(session->takeEvents(), error), error);
}

void finishSave(pipeview_settings::Application &application)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string error;
    while (application.snapshot().saving) {
        require(application.pumpSave(error), error);
        require(std::chrono::steady_clock::now() < deadline, "Save completion deadline exceeded");
        std::this_thread::yield();
    }
}

void editName(pipeview_settings::Application &application, const std::string &text)
{
    std::string error;
    click(application, "name-field");
    const bool accepted = application.session()->textInput(text, error);
    if (application.snapshot().saving) {
        require(!accepted && application.session()->takeEvents().empty(),
            "Disabled pending field accepted input");
        return;
    }
    require(accepted, error);
    require(application.dispatch(application.session()->takeEvents(), error), error);
}
}

int main(int argc, char **argv)
{
    try {
        require(argc == 2 && std::filesystem::path(argv[1]).is_absolute(), "Absolute fixture root required");
        std::filesystem::create_directories(argv[1]);
        std::string pattern = std::string(argv[1]) + "/run-XXXXXX";
        std::vector<char> directory(pattern.begin(), pattern.end());
        directory.push_back('\0');
        require(::mkdtemp(directory.data()) != nullptr, "Cannot create owned fixture");
        const std::filesystem::path root(directory.data());
        const auto path = root / "settings.data";
        std::string error;
        settings_models::SettingsModel domain(std::make_shared<StorageStub>(), settings_models::SettingsValues{});
        require(domain.ChangeVolume(101).is_success()
            && domain.snapshot().status == settings_models::SaveStatus::Invalid,
            "Typed model status did not reflect an invalid draft");
        require(domain.ChangeVolume(100).is_success()
            && domain.snapshot().status == settings_models::SaveStatus::Saved,
            "Editing back to baseline left a stale unsaved status");
        auto application = pipeview_settings::Application::create(path, error);
        require(bool(application), error);
        require(!application->snapshot().dirty && !application->snapshot().canSave, "Initial flags differ");
        click(*application, "save-button");
        require(!std::filesystem::exists(path), "Clean Save wrote a file");
        editName(*application, "X");
        click(*application, "vsync-toggle");
        require(playerName(application->snapshot()) == "RiderX" && application->snapshot().canSave, "Typed edits failed");
        click(*application, "save-button");
        require(application->snapshot().saving && application->snapshot().status == "Saving", "Save was not pending");
        editName(*application, "Y");
        click(*application, "revert-button");
        require(playerName(application->snapshot()) == "RiderX", "Busy edit/revert changed the draft");
        finishSave(*application);
        require(std::filesystem::is_regular_file(path) && !application->snapshot().dirty, "Save did not persist");
        auto reopened = pipeview_settings::Application::create(path, error);
        require(bool(reopened), error);
        require(playerName(reopened->snapshot()) == "RiderX"
            && !reopened->snapshot().draft.vsync, "Reload did not preserve typed values");
        editName(*reopened, "Z");
        click(*reopened, "revert-button");
        require(playerName(reopened->snapshot()) == "RiderX", "Revert used defaults instead of saved baseline");
        // Presentation staging cannot bypass the authoritative model.
        require(reopened->session()->setValue("model.draft.playerName", std::string("Injected"), error), error);
        click(*reopened, "save-button");
        require(playerName(reopened->snapshot()) == "RiderX"
            && std::get<std::string>(*reopened->session()->value("model.draft.playerName")) == "RiderX", "View staging bypassed actions");
        click(*reopened, "name-field");
        for (int index = 0; index < 6; ++index) {
            require(reopened->session()->key(forgepipe::ui::Key::Backspace, false, error), error);
            require(reopened->dispatch(reopened->session()->takeEvents(), error), error);
        }
        require(!reopened->snapshot().valid && reopened->snapshot().dirty, "Compiled validator did not reject empty name");
        click(*reopened, "save-button");
        require(!reopened->snapshot().saving, "Invalid Save began persistence");
        click(*reopened, "revert-button");
        // An unavailable explicit destination fails without losing the draft.
        const auto failurePath = root / "missing-parent" / "settings.data";
        auto failure = pipeview_settings::Application::create(failurePath, error);
        require(bool(failure), error);
        editName(*failure, "X");
        click(*failure, "save-button");
        finishSave(*failure);
        require(failure->snapshot().dirty && !failure->snapshot().saving
            && failure->snapshot().status == "Save failed" && !failure->storageError().empty(), "Failed Save lost retry state");
        std::filesystem::create_directory(failurePath.parent_path());
        click(*failure, "save-button");
        finishSave(*failure);
        require(!failure->snapshot().dirty, "Retry failed after destination became available");
        const auto corrupt = root / "corrupt.data";
        { std::ofstream file(corrupt); file << "invalid"; }
        require(!pipeview_settings::Application::create(corrupt, error), "Malformed saved data silently became defaults");
        const auto malformedText = root / "malformed-text.data";
        {
            std::ofstream file(malformedText, std::ios::binary);
            file << "pipeview.settings.v1\n2\n" << std::string("\xc0\xaf", 2) << "\n1\n";
        }
        require(!pipeview_settings::Application::create(malformedText, error) && !error.empty(),
            "Malformed stored Unicode bypassed codec admission");
        const auto link = root / "link.data";
        std::filesystem::create_symlink(path, link);
        require(!pipeview_settings::Application::create(link, error), "Symlink store was accepted");
        const auto fifo = root / "fifo.data";
        require(::mkfifo(fifo.c_str(), 0600) == 0, "Cannot create owned FIFO refusal fixture");
        require(!pipeview_settings::Application::create(fifo, error), "Nonregular store was accepted");
        std::puts("pipeview_settings: typed_edit_validation_save_busy_failure_retry_reload_revert=passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Settings model: %s\n", error.what());
        return 1;
    }
}
