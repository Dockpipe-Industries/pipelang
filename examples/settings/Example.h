#pragma once

#include "ModelView.hpp"
#include "SaveResult.h"
#include "SettingsStore.h"
#include <functional>
#include <memory>
#include <thread>

namespace pipeview_settings {
namespace model = pipelang::model;

// Owns the typed model, its generated view bridge and a storage service. Model
// methods own application transitions; this shell supplies filesystem I/O only.
class Application final {
public:
    using Persist = std::function<SaveResult(SettingsValue, model::CancellationToken)>;
    static std::unique_ptr<Application> create(const std::filesystem::path &path,
        std::string &error, Persist persist = {});
    ~Application();
    std::shared_ptr<forgepipe::ui::Session> session() const noexcept;
    bool isOwnerThread() const noexcept { return std::this_thread::get_id() == m_owner; }
    settings_models::SettingsSnapshot snapshot() const;
    bool dispatch(const std::vector<forgepipe::ui::Event> &events, std::string &error);
    bool pumpSave(std::string &error);
    std::string storageError() const;
    void cancelPending();
    const settings_models::SettingsViewAdapter &view() const noexcept { return *m_view; }

private:
    Application(std::shared_ptr<settings_models::SettingsStorage> storage, SettingsValue initial);
    std::shared_ptr<settings_models::SettingsStorage> m_storage;
    std::unique_ptr<settings_models::SettingsModel> m_model;
    // The bridge is destroyed before the model; model teardown cancels and joins
    // its owned work before the service is released.
    std::unique_ptr<settings_models::SettingsViewAdapter> m_view;
    const std::thread::id m_owner = std::this_thread::get_id();
};
}
