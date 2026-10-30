#pragma once
#include "DemoBindings.hpp"
#include "SettingsStore.h"
#include "SaveResult.h"
#include <functional>
#include <future>
#include <thread>
#include <forgepipe/ui/Session.h>
#include <memory>

namespace pipeview_settings {
namespace model = pipelang::pipeview::state;

class Application final {
public:
    using Persist = std::function<SaveResult(SettingsValue)>;
    ~Application();
    static std::unique_ptr<Application> create(const std::filesystem::path &path, std::string &error, Persist persist = {});
    const std::shared_ptr<forgepipe::ui::Session> &session() const noexcept { return m_session; }
    bool isOwnerThread() const noexcept { return std::this_thread::get_id() == m_owner; }
    model::Snapshot snapshot() const { return m_model->snapshot(); }
    bool dispatch(const std::vector<forgepipe::ui::Event> &events, std::string &error);
    bool pumpSave(std::string &error);
    const std::string &storageError() const noexcept { return m_storageError; }

private:
    Application(std::shared_ptr<forgepipe::ui::Session> session, std::unique_ptr<model::Model> state,
        Persist persist);
    bool project(std::string &error);
    std::shared_ptr<forgepipe::ui::Session> m_session;
    std::unique_ptr<model::Model> m_model;
    Persist m_persist;
    const std::thread::id m_owner = std::this_thread::get_id();
    std::optional<model::SaveRequest> m_pending;
    std::string m_storageError;
    // Last member: destruction joins owned work before model/session teardown.
    std::future<SaveResult> m_save;
};
}
