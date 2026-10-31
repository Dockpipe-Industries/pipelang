#include "Example.h"
#include "Inspection.h"
#include <forgepipe/NativeVulkanHost.h>
#include <forgepipe/ui/BasicFont.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
using Host = forgepipe::NativeVulkanHost;
using Application = pipeview_settings::Application;
using Clock = std::chrono::steady_clock;

bool input(Application &application, const Host::Event &event, std::string &error)
{
    auto session = application.session();
    bool accepted = true;
    if (event.kind == Host::EventKind::PointerDown && event.button == 1) {
        accepted = session->pointerDown(event.x, event.y, error);
    } else if (event.kind == Host::EventKind::Text && session->textFocused()) {
        if (!forgepipe::ui::supportsBasicText(event.text)) {
            error = "Native text backend supports printable ASCII";
            return false;
        }
        accepted = session->textInput(event.text, error);
    } else if (event.kind == Host::EventKind::KeyDown) {
        using Key = forgepipe::ui::Key;
        if (event.key == Host::Key::Tab)
            accepted = session->key(Key::Tab, event.shift, error);
        else if (event.key == Host::Key::Left)
            accepted = session->key(Key::Left, event.shift, error);
        else if (event.key == Host::Key::Right)
            accepted = session->key(Key::Right, event.shift, error);
        else if (event.key == Host::Key::Backspace)
            accepted = session->key(Key::Backspace, event.shift, error);
        else if (event.key == Host::Key::Enter)
            accepted = session->key(Key::Enter, event.shift, error);
        else if (event.key == Host::Key::Space && !session->textFocused())
            accepted = session->key(Key::Space, event.shift, error);
    }
    return accepted && application.dispatch(session->takeEvents(), error);
}

bool exercise(Application &application, std::string &error)
{
    const auto click = [&](const std::string &id) {
        const auto bounds = application.session()->bounds(id);
        Host::Event event;
        event.kind = Host::EventKind::PointerDown;
        event.button = 1;
        event.x = bounds.x + 2;
        event.y = bounds.y + 2;
        return input(application, event, error);
    };
    if (!click("name-field"))
        return false;
    Host::Event text;
    text.kind = Host::EventKind::Text;
    text.text = "X";
    return input(application, text, error) && click("vsync-toggle") && click("volume-stepper") && click("save-button")
        && application.snapshot().saving;
}

bool expected(const Application &application, const std::string &mode)
{
    const auto state = application.snapshot();
    return state.draft.playerName == "RiderX"
        && !state.draft.vsync
        && state.draft.volume == 99 && !state.saving
        && (mode == "failure" ? state.dirty && state.status == "Save failed"
                              : !state.dirty && state.status == "Saved");
}
}

int main(int argc, char **argv)
{
    try {
        std::filesystem::path settings;
        std::filesystem::path proof;
        std::string mode = "save";
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc)
                return 2;
            const std::string option = argv[index];
            if (option == "--settings-file")
                settings = argv[index + 1];
            else if (option == "--proof-dir")
                proof = argv[index + 1];
            else if (option == "--proof-mode")
                mode = argv[index + 1];
            else
                return 2;
        }
        if (!settings.is_absolute() || (!proof.empty() && !proof.is_absolute())
            || (mode != "save" && mode != "reopen" && mode != "failure"))
            return 2;
        std::string error;
        auto application = Application::create(settings, error);
        if (!application)
            throw std::runtime_error(error);
        Host::Options options;
        options.title = "PipeView settings / compiled PipeLang / ForgePipe SDK";
        options.extent = {600, 440};
        options.shaderDirectory = FORGEPIPE_UI_SHADER_DIRECTORY;
        options.renderer.shadowMapSize = 0;
        auto host = Host::create(options, error);
        if (!host)
            throw std::runtime_error(error);
        const auto start = Clock::now();
        std::uint64_t frames = 0;
        bool exercised = false;
        bool capturedPending = false;
        while (!host->closed()) {
            // Poll owned storage work without blocking. Only this owner thread
            // applies completed results to the model and presentation.
            if (!application->pumpSave(error))
                throw std::runtime_error(error);
            const auto events = host->pollEvents();
            const auto extent = host->extent();
            if (extent.width == 0 || extent.height == 0) {
                if (!proof.empty() && Clock::now() - start > std::chrono::seconds(30))
                    throw std::runtime_error("Native proof remained minimized");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (!application->session()->layout(float(extent.width), float(extent.height), error))
                throw std::runtime_error(error);
            for (const auto &event : events) {
                if (!input(*application, event, error))
                    throw std::runtime_error(error);
            }
            if (!proof.empty() && frames == 2 && !exercised) {
                if (mode != "reopen" && !exercise(*application, error))
                    throw std::runtime_error("Typed Save input proof failed: " + error);
                exercised = true;
            }
            SpatialVulkanRenderer::Frame frame;
            frame.camera.right = {1, 0, 0};
            frame.camera.up = {0, 1, 0};
            frame.camera.forward = {0, 0, 1};
            frame.camera.viewport = extent;
            frame.camera.focalLength = 400;
            frame.camera.sky = forgepipe::Color::rgba8(12, 16, 23);
            frame.ui = application->session()->draw();
            if (!host->draw(frame))
                throw std::runtime_error(host->error());
            ++frames;
            if (!proof.empty() && application->snapshot().saving && !capturedPending) {
                std::filesystem::create_directories(proof);
                if (!host->renderer().captureOutput().savePng(proof / "pending-settings.png", &error))
                    throw std::runtime_error(error);
                std::ofstream pending(proof / "pending-inspection.json");
                pending << pipeview_settings::inspect(*application) << '\n';
                if (!pending)
                    throw std::runtime_error("Cannot write pending semantic inspection");
                capturedPending = true;
            }
            if (!proof.empty() && frames >= 8 && !application->snapshot().saving) {
                if (!exercised || (mode != "reopen" && !capturedPending) || !expected(*application, mode) || !host->renderer().error().empty())
                    throw std::runtime_error("Native settings outcome differs from expected model");
                std::filesystem::create_directories(proof);
                if (!host->renderer().captureOutput().savePng(proof / "native-settings.png", &error))
                    throw std::runtime_error(error);
                std::ofstream semantic(proof / "inspection.json");
                semantic << pipeview_settings::inspect(*application) << '\n';
                if (!semantic)
                    throw std::runtime_error("Cannot write semantic inspection");
                std::ofstream report(proof / "native-settings.txt");
                report << "success=true\nmode=" << mode << "\nframes=" << frames
                    << "\nstatus=" << application->snapshot().status
                    << "\nsynthetic_input=true\nphysical_input_tested=false\n";
                return report ? 0 : 1;
            }
            if (!proof.empty() && Clock::now() - start > std::chrono::seconds(30))
                throw std::runtime_error("Native settings proof deadline exceeded");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return proof.empty() ? 0 : 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "PipeView settings: %s\n", error.what());
        return 1;
    }
}
