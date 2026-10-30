#include "Example.h"
#include <algorithm>
#include <chrono>

namespace pipeview_settings {
namespace {
const forgepipe::ui::Node *findControl(const forgepipe::ui::Node &node, const std::string &identity)
{
    if (node.id == identity)
        return &node;
    for (const auto &child : node.children) {
        if (const auto *found = findControl(child, identity))
            return found;
    }
    return nullptr;
}
}
Application::Application(std::shared_ptr<forgepipe::ui::Session> session,
    std::unique_ptr<model::Model> state, Persist persist)
    : m_session(std::move(session)), m_model(std::move(state)), m_persist(std::move(persist))
{
}

std::unique_ptr<Application> Application::create(const std::filesystem::path &path, std::string &error, Persist persist)
{
    SettingsStore store(path);
    std::optional<SettingsValue> saved;
    if (!store.load(saved, error))
        return {};
    std::optional<model::Values> baseline;
    if (saved)
        baseline = model::Values{{"playerName", saved->playerName}, {"vsync", saved->vsync}, {"volume", saved->volume}};
    const auto valid = [](const model::Values &values) {
        return compiled_ui::IsValidName(std::get<std::string>(values.at("playerName")))
            && compiled_ui::IsValidVolume(std::get<std::int64_t>(values.at("volume")));
    };
    auto definition = compiled_ui::model_definition();
    if (definition.initial.size() != 3 || !definition.initial.count("playerName") || !definition.initial.count("vsync") || !definition.initial.count("volume")
        || !std::holds_alternative<std::string>(definition.initial.at("playerName"))
        || !std::holds_alternative<bool>(definition.initial.at("vsync"))
        || !std::holds_alternative<std::int64_t>(definition.initial.at("volume"))) {
        error = "Settings model does not match the storage schema";
        return {};
    }
    auto state = model::Model::create(std::move(definition), std::move(baseline), valid, error);
    if (!state)
        return {};
    forgepipe::ui::Document document;
    forgepipe::ui::State initial;
    if (!forgepipe::ui::Document::decode(compiled_ui::view(), document, initial, error))
        return {};
    auto session = forgepipe::ui::Session::create(std::move(document), std::move(initial), error);
    if (!session)
        return {};
    if (!persist) {
        persist = [store = std::move(store)](SettingsValue value) {
            std::string failure;
            const bool saved = store.save(value, failure);
            return SaveResult{saved ? model::Outcome::Success : model::Outcome::Failure, std::move(failure)};
        };
    }
    auto application = std::unique_ptr<Application>(new Application(
        std::shared_ptr<forgepipe::ui::Session>(std::move(session)), std::move(state), std::move(persist)));
    if (!application->project(error))
        return {};
    return application;
}

Application::~Application()
{
    // Persistence captures owned values, never this, Model or Session. Closing
    // finishes an already-started write; it does not promise rollback/cancellation.
    if (m_save.valid())
        m_save.wait();
}

bool Application::project(std::string &error)
{
    const auto state = m_model->snapshot();
    for (const auto &[identity, value] : state.draft) {
        if (!m_session->setValue(identity, value, error))
            return false;
    }
    for (const auto &[binding, action] : compiled_ui::status_bindings()) {
        (void)action;
        if (!m_session->setValue(binding, state.status, error))
            return false;
    }
    return true;
}

bool Application::dispatch(const std::vector<forgepipe::ui::Event> &events, std::string &error)
{
    if (std::this_thread::get_id() != m_owner) {
        error = "Settings actions require the owner thread";
        return false;
    }
    const auto definition = compiled_ui::model_definition();
    for (const auto &event : events) {
        const auto action = std::find_if(definition.actions.begin(), definition.actions.end(),
            [&](const model::Action &candidate) { return candidate.name == event.action; });
        if (action == definition.actions.end()) {
            project(error);
            error = "Unknown typed settings action";
            return false;
        }
        const auto *control = findControl(m_session->document().root, event.control);
        if (!control || !control->enabled || control->action != event.action || control->binding != event.binding) {
            project(error);
            error = "Settings event does not match its declared control";
            return false;
        }
        std::optional<model::Value> payload;
        if (action->operation == model::Operation::Set) {
            // ForgePipe input is presentation staging. Only this typed action
            // may commit the authoritative draft; refusals restore the view.
            if (event.type != forgepipe::ui::EventType::ValueChanged || event.binding != action->target) {
                project(error);
                error = "Settings event does not match its typed target";
                return false;
            }
            const bool controlMatches = (action->payloadKind == model::ValueKind::Text
                    && control->type == forgepipe::ui::ControlType::TextField)
                || (action->payloadKind == model::ValueKind::Boolean
                    && control->type == forgepipe::ui::ControlType::Toggle)
                || (action->payloadKind == model::ValueKind::Integer
                    && control->type == forgepipe::ui::ControlType::PercentStepper);
            if (!controlMatches) {
                project(error);
                error = "Settings event control has the wrong kind";
                return false;
            }
            const bool payloadMatches = (action->payloadKind == model::ValueKind::Text && std::holds_alternative<std::string>(event.value))
                || (action->payloadKind == model::ValueKind::Boolean && std::holds_alternative<bool>(event.value))
                || (action->payloadKind == model::ValueKind::Integer && std::holds_alternative<std::int64_t>(event.value));
            if (!payloadMatches) {
                project(error);
                error = "Settings event payload has the wrong type";
                return false;
            }
            payload = event.value;
        } else if (control->type != forgepipe::ui::ControlType::Button
            || event.type != forgepipe::ui::EventType::Activated || !event.binding.empty()
            || !std::holds_alternative<bool>(event.value) || std::get<bool>(event.value)) {
            project(error);
            error = "Settings command requires activation";
            return false;
        }
        std::optional<model::SaveRequest> request;
        std::string refusal;
        if (!m_model->dispatch(event.action, std::move(payload), request, refusal)) {
            // Busy, invalid and clean-save refusals are expected UI outcomes.
            if (!project(error))
                return false;
            continue;
        }
        if (request) {
            m_pending = std::move(request);
            m_storageError.clear();
            SettingsValue value{std::get<std::string>(m_pending->snapshot.at("playerName")),
                std::get<bool>(m_pending->snapshot.at("vsync")),
                std::get<std::int64_t>(m_pending->snapshot.at("volume"))};
            try {
                m_save = std::async(std::launch::async,
                    [persist = m_persist, value = std::move(value)]() mutable {
                        return persist(std::move(value));
                    });
            } catch (const std::exception &) {
                m_storageError = "Cannot start settings persistence";
                const auto id = m_pending->id;
                m_pending.reset();
                if (!m_model->complete(id, model::Outcome::Failure, error))
                    return false;
            }
        }
    }
    return project(error);
}

bool Application::pumpSave(std::string &error)
{
    if (std::this_thread::get_id() != m_owner) {
        error = "Settings completion requires the owner thread";
        return false;
    }
    error.clear();
    if (!m_pending)
        return true;
    if (!m_save.valid()) {
        error = "Pending settings request has no owned worker";
        return false;
    }
    if (m_save.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return true;
    SaveResult result;
    try {
        result = m_save.get();
    } catch (const std::exception &) {
        result = {model::Outcome::Failure, "Unexpected settings persistence failure"};
    }
    catch (...) {
        result = {model::Outcome::Failure, "Nonstandard settings persistence failure"};
    }
    if (result.outcome != model::Outcome::Success && result.outcome != model::Outcome::Failure
        && result.outcome != model::Outcome::Cancelled)
        result = {model::Outcome::Failure, "Invalid settings persistence completion"};
    const auto id = m_pending->id;
    m_pending.reset();
    m_storageError = std::move(result.error);
    if (!m_model->complete(id, result.outcome, error))
        return false;
    return project(error);
}
}
