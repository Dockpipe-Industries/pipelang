#include "Example.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Application = pipeview_settings::Application;
using Value = pipeview_settings::SettingsValue;
using Store = pipeview_settings::SettingsStore;

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string read(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path &path, const std::string &data)
{
    std::ofstream file(path, std::ios::binary);
    file << data;
    require(bool(file), "Cannot write owned fixture");
}

void click(Application &application, const std::string &identity)
{
    std::string error;
    const auto session = application.session();
    require(session->layout(600, 520, error), error);
    const auto bounds = session->bounds(identity);
    require(bounds.width > 0 && bounds.height > 0, "Control has no bounds");
    require(session->pointerDown(bounds.x + 2, bounds.y + 2, error), error);
    require(application.dispatch(session->takeEvents(), error), error);
}

void setVolume(Application &application, std::int64_t value)
{
    std::string error;
    forgepipe::ui::Event event;
    event.type = forgepipe::ui::EventType::ValueChanged;
    event.control = "volume-stepper";
    event.action = "volumeEdited";
    event.binding = "volume";
    event.value = value;
    require(application.dispatch({event}, error), error);
}

std::int64_t volume(const Application &application)
{
    return std::get<std::int64_t>(application.snapshot().draft.at("volume"));
}

void finishSave(Application &application)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string error;
    while (application.snapshot().saving) {
        require(application.pumpSave(error), error);
        require(std::chrono::steady_clock::now() < deadline, "Owned save completion deadline exceeded");
        std::this_thread::yield();
    }
}

void storageFixtures(const std::filesystem::path &root)
{
    std::string error;
    std::optional<Value> loaded = Value{"previous", false, 9};
    Store absent(root / "absent");
    require(absent.load(loaded, error) && !loaded, "Absent storage did not clear optional output");
    const auto path = root / "codec";
    Store store(path);
    const std::string legacy = "pipeview.settings.v1\n5\nRider\n1\n";
    write(path, legacy);
    require(store.load(loaded, error) && loaded && loaded->volume == 100, error);
    require(read(path) == legacy, "Legacy load rewrote storage");
    for (const auto value : {0, 1, 37, 99, 100}) {
        require(store.save(Value{"Rider", false, value}, error), error);
        require(read(path) == "pipeview.settings.v2\n5\nRider\n0\n" + std::to_string(value) + "\n", "Noncanonical v2 encoding");
        require(store.load(loaded, error) && loaded && loaded->volume == value && !loaded->vsync, error);
    }
    const auto accepted = read(path);
    require(!store.save(Value{"Rider", true, -1}, error) && read(path) == accepted, "Invalid Save changed storage");
    require(!store.save(Value{"Rider", true, 101}, error) && read(path) == accepted, "Excessive Save changed storage");
    for (const auto &decimal : std::vector<std::string>{
             "", "-1\n", "+1\n", "01\n", "00\n", "101\n", "1 \n", "1.0\n",
             "100", "100\nextra", "18446744073709551616\n"}) {
        write(path, "pipeview.settings.v2\n5\nRider\n1\n" + decimal);
        loaded = Value{"sentinel", false, 17};
        require(!store.load(loaded, error) && !loaded && !error.empty(), "Malformed v2 published a partial candidate");
    }
    for (const auto &data : std::vector<std::string>{
             "invalid", "pipeview.settings.v1\n5\nRider\n2\n",
             "pipeview.settings.v1\n5\nRider\n1\nextra",
             "pipeview.settings.v2\n5\nRider\n2\n100\n",
             "pipeview.settings.v2\n05\nRider\n1\n100\n"}) {
        write(path, data);
        loaded = Value{"sentinel", false, 17};
        require(!store.load(loaded, error) && !loaded, "Malformed version/shape returned data");
    }
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
        storageFixtures(root);
        const auto path = root / "settings";
        const std::string legacy = "pipeview.settings.v1\n5\nRider\n0\n";
        write(path, legacy);
        std::string error;
        auto application = Application::create(path, error);
        require(bool(application) && volume(*application) == 100 && !application->snapshot().dirty, error);
        click(*application, "save-button");
        require(read(path) == legacy, "Clean Save migrated legacy storage");
        for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                 std::int64_t{101}, std::numeric_limits<std::int64_t>::max()}) {
            setVolume(*application, invalid);
            require(volume(*application) == invalid && !application->snapshot().valid
                && !application->snapshot().canSave && application->snapshot().status == "Invalid settings", "Invalid draft was clamped or admitted");
            require(std::get<std::int64_t>(*application->session()->value("volume")) == invalid, "Invalid raw draft was not projected");
            click(*application, "save-button");
            require(!application->snapshot().saving && read(path) == legacy, "Invalid Save changed storage");
            click(*application, "volume-stepper");
            require(volume(*application) == (invalid < 0 ? 0 : 100) && application->snapshot().valid, "Gesture did not recover invalid draft");
        }
        setVolume(*application, 37);
        click(*application, "save-button");
        require(application->snapshot().saving, "Valid volume Save did not start");
        setVolume(*application, 22);
        require(volume(*application) == 37, "Busy volume edit escaped model ownership");
        finishSave(*application);
        require(read(path) == "pipeview.settings.v2\n5\nRider\n0\n37\n", "Explicit Save did not migrate full snapshot");
        auto reopened = Application::create(path, error);
        require(bool(reopened) && volume(*reopened) == 37, error);
        setVolume(*reopened, 72);
        click(*reopened, "revert-button");
        require(volume(*reopened) == 37, "Revert ignored saved volume");
        forgepipe::ui::Event forged;
        forged.type = forgepipe::ui::EventType::ValueChanged;
        forged.control = "name-field";
        forged.action = "volumeEdited";
        forged.binding = "volume";
        forged.value = std::int64_t{55};
        require(!reopened->dispatch({forged}, error) && volume(*reopened) == 37, "Forged control edited volume");
        std::puts("percent_stepper: strict_storage_legacy_no_write_invalid_draft_recovery_save_reload_revert=passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Percent settings: %s\n", error.what());
        return 1;
    }
}
