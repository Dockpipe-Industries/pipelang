#include "View.hpp"
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace pipelang_models;
using namespace forgepipe::ui;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
const FixtureViewAdapter::ControlInspection &control(const FixtureViewAdapter &adapter, const std::string &id)
{
    for (const auto &item : adapter.inspection()) {
        if (item.id == id)
            return item;
    }
    throw std::runtime_error("missing inspection control");
}
Event event(FixtureViewAdapter &adapter, const std::string &id, Value value = false)
{
    const auto find = [&](const auto &self, const Node &node) -> const Node * {
        if (node.id == id)
            return &node;
        for (const auto &child : node.children) {
            if (const auto *found = self(self, child))
                return found;
        }
        return nullptr;
    };
    const auto *node = find(find, adapter.session()->document().root);
    require(node != nullptr, "declared control exists");
    return {node->type == ControlType::Button ? EventType::Activated : EventType::ValueChanged,
        node->id, node->action, node->binding, std::move(value)};
}
int main()
{
    FixtureModel model;
    std::string error;
    auto adapter = FixtureViewAdapter::create(model, error, 1);
    require(bool(adapter), "generated adapter creates");
    require(adapter->session()->setValue("model.Draft.Name", std::string("Presentation-only edit"), error),
        "Presentation value can be staged independently");
    const auto restored = adapter->dispatch(std::vector<Event>{});
    require(restored.size() == 1 && restored.front().outcome.is_success()
        && restored.front().projection.is_success() && !restored.front().pending
        && std::get<std::string>(*adapter->session()->value("model.Draft.Name")) == "Before"
        && model.snapshot().Draft.Name == "Before",
        "Empty batch restores authoritative model projection without an action outcome change");
    require(control(*adapter, "name").redacted && !control(*adapter, "name").value,
        "sensitive inspection omits value");
    require(adapter->dispatch(event(*adapter, "name", std::string(""))).outcome.is_success()
        && !control(*adapter, "save").enabled, "model getter disables rendered Save");
    require(adapter->dispatch(event(*adapter, "save")).outcome.is_cancelled(), "disabled action cancelled");
    auto forged = event(*adapter, "name", std::string("Forged"));
    forged.binding = "wrong";
    require(adapter->dispatch(forged).outcome.is_failure() && model.snapshot().Draft.Name.empty(),
        "forged event cannot invoke model");
    require(adapter->dispatch(event(*adapter, "name", true)).outcome.is_failure(), "wrong payload fails");
    require(adapter->dispatch(event(*adapter, "name", std::string("After"))).outcome.is_success(), "typed edit invokes model");
    require(adapter->dispatch(event(*adapter, "volume", std::int64_t(-100))).outcome.is_success()
        && model.snapshot().Draft.Volume == -100, "raw integer remains model-owned");
    require(adapter->dispatch(event(*adapter, "fail")).outcome.is_failure()
        && std::get<std::string>(*control(*adapter, "error").value) == "Expected failure", "failure and feedback preserved");
    require(adapter->dispatch(event(*adapter, "cancel")).outcome.is_cancelled(), "sync cancellation preserved");
    const auto started = adapter->dispatch(event(*adapter, "save"));
    require(started.pending && started.outcome.is_success() && started.projection.is_success()
        && adapter->pending() == 1 && !control(*adapter, "save").enabled, "async handle owned and projection applied");
    model.waitUntilEntered();
    adapter->cancelPending();
    model.release();
    bool completed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!completed && std::chrono::steady_clock::now() < deadline) {
        const auto polled = adapter->poll();
        for (const auto &report : polled.completed) {
            require(report.outcome.is_cancelled(), "async cancellation remains observable");
            completed = true;
        }
        std::this_thread::yield();
    }
    require(completed && adapter->pending() == 0 && control(*adapter, "save").enabled,
        "owner poll publishes completion and availability");
    const auto before = adapter->projection().state;
    require(model.SetName(std::string(4097, 'x')).is_success(), "model can produce adapter-invalid text");
    require(adapter->project().is_failure() && adapter->projection().state == before,
        "invalid projection preserves authoritative inspection and rendering state");
    return 0;
}
