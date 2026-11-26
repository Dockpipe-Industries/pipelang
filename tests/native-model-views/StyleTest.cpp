#include "PlainView.hpp"
#include "StyledView.hpp"
#include <stdexcept>

using namespace forgepipe::ui;
using namespace pipelang_models;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class Adapter>
Event event(const Adapter &adapter, const std::string &id, Value value)
{
    for (const auto &control : adapter.inspection()) {
        if (control.id == id)
            return {EventType::ValueChanged, id, control.action, control.binding, std::move(value)};
    }
    throw std::runtime_error("missing control");
}

bool hasColor(const DrawList &draw, DrawKind kind, std::uint32_t color)
{
    for (const auto &command : draw.commands) {
        if (command.kind == kind && command.colorRGBA == color)
            return true;
    }
    return false;
}

int main()
{
    FixtureModel plainModel;
    FixtureModel styledModel;
    std::string error;
    auto plain = PlainViewAdapter::create(plainModel, error);
    auto styled = StyledViewAdapter::create(styledModel, error);
    require(plain && styled, "both generated adapters create against installed SDKs");
    auto plainSession = plain->session();
    auto styledSession = styled->session();
    const auto &style = styledSession->style();
    require(style.padding == 10.5f && style.gap == 4 && style.controlHeight == 42,
        "explicit overrides and inherited token rebinding reach renderer");
    require(style.scale == 1 && style.field == plainSession->style().field,
        "host scale and unspecified adapter defaults are preserved");
    require(plainSession->layout(1000, 700, error) && styledSession->layout(1000, 700, error),
        "both styles have valid bounded layout");
    require(styledSession->bounds("name").height == 42
        && styledSession->bounds("name").x != plainSession->bounds("name").x,
        "authored metrics change measured control geometry");
    const auto draw = styledSession->draw();
    require(draw.commands.front().colorRGBA == 0x102030ff
        && hasColor(draw, DrawKind::Text, 0xddeeffff), "typed palette reaches draw commands");
    require(styledSession->key(Key::Tab, false, error), "focus uses normal input path");
    require(hasColor(styledSession->draw(), DrawKind::StrokeRect, 0xaabbccff),
        "existing focus interaction uses authored focus color");
    for (const auto &name : {std::string("Edited"), std::string("")}) {
        const auto plainResult = plain->dispatch(event(*plain, "name", name));
        const auto styledResult = styled->dispatch(event(*styled, "name", name));
        require(plainResult.outcome.is_success() && styledResult.outcome.is_success()
            && plain->projection().state == styled->projection().state
            && plain->projection().enabled == styled->projection().enabled,
            "visual changes preserve typed action results and model projections");
        require(plainModel.snapshot().Draft.Name == styledModel.snapshot().Draft.Name,
            "visual changes preserve authoritative model state");
    }
    require(hasColor(styledSession->draw(), DrawKind::Text, 0x667788ff),
        "model-disabled control uses authored disabled color");
    require(styled->inspection().front().id == plain->inspection().front().id,
        "style does not change semantic control identity");
    return 0;
}
