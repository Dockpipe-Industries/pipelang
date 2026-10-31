#include "Example.h"

namespace pipeview_settings {
namespace {
class Storage final : public settings_models::SettingsStorage {
public:
    explicit Storage(Application::Persist persist) : m_persist(std::move(persist)) {}

    SaveResult Save(SettingsValue value, model::CancellationToken cancellation) override
    {
        return m_persist(std::move(value), std::move(cancellation));
    }

private:
    Application::Persist m_persist;
};
}

Application::Application(std::shared_ptr<settings_models::SettingsStorage> storage, SettingsValue initial)
    : m_storage(std::move(storage)),
      m_model(std::make_unique<settings_models::SettingsModel>(m_storage, std::move(initial)))
{
}

Application::~Application() = default;

std::unique_ptr<Application> Application::create(const std::filesystem::path &path,
    std::string &error, Persist persist)
{
    SettingsStore store(path);
    std::optional<SettingsValue> saved;
    if (!store.load(saved, error))
        return {};
    if (!persist) {
        persist = [store](SettingsValue value, model::CancellationToken cancellation) {
            if (cancellation.is_cancelled())
                return SaveResult::Cancelled("Save cancelled before filesystem commit");
            std::string failure;
            if (!store.save(value, failure))
                return SaveResult::Failure(std::move(failure));
            // A completed atomic replacement is a successful effect even if a
            // cancellation request arrived while the filesystem was committing.
            return SaveResult::Success();
        };
    }
    auto storage = std::make_shared<Storage>(std::move(persist));
    std::unique_ptr<Application> application;
    try {
        application.reset(new Application(storage, saved.value_or(SettingsValue{})));
    } catch (const pipelang::model::ExecutionFailure &failure) {
        error = failure.what();
        return {};
    }
    if (!application->m_model->Inspect().valid) {
        error = "Saved settings failed model validation";
        return {};
    }
    application->m_view = settings_models::SettingsViewAdapter::create(*application->m_model, error);
    if (!application->m_view)
        return {};
    error.clear();
    return application;
}

std::shared_ptr<forgepipe::ui::Session> Application::session() const noexcept
{
    return m_view->session();
}

settings_models::SettingsSnapshot Application::snapshot() const
{
    return m_model->Inspect();
}

bool Application::dispatch(const std::vector<forgepipe::ui::Event> &events, std::string &error)
{
    if (!isOwnerThread()) {
        error = "Settings actions require the owner thread";
        return false;
    }
    for (const auto &report : m_view->dispatch(events)) {
        if (!report.projection.is_success()) {
            error = report.projection.error();
            return false;
        }
        if (report.outcome.is_failure()) {
            error = report.outcome.error();
            return false;
        }
    }
    error.clear();
    return true;
}

bool Application::pumpSave(std::string &error)
{
    if (!isOwnerThread()) {
        error = "Settings actions require the owner thread";
        return false;
    }
    const auto report = m_view->poll();
    if (!report.projection.is_success()) {
        error = report.projection.error();
        return false;
    }
    // Expected storage Failure/Cancelled outcomes are observable in each
    // completion and the source model's status/error bindings.
    error.clear();
    return true;
}

std::string Application::storageError() const
{
    return snapshot().error;
}

void Application::cancelPending()
{
    m_view->cancelPending();
}
}
