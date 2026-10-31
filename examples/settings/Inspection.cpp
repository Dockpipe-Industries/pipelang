#include "Inspection.h"

#include <stdexcept>

namespace pipeview_settings {
namespace {
std::string quote(const std::string &value)
{
    if (!forgepipe::ui::validUtf8(value))
        throw std::runtime_error("Inspection text is not valid UTF-8");
    static constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(character));
        } else if (character < 0x20) {
            result += "\\u00";
            result.push_back(hexadecimal[character >> 4]);
            result.push_back(hexadecimal[character & 15]);
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    return result + '"';
}

std::string role(forgepipe::ui::ControlType type)
{
    using Type = forgepipe::ui::ControlType;
    switch (type) {
    case Type::Column:
        return "column";
    case Type::Row:
        return "row";
    case Type::Label:
        return "label";
    case Type::Button:
        return "button";
    case Type::Toggle:
        return "toggle";
    case Type::TextField:
        return "text-field";
    case Type::PercentStepper:
        return "percent-stepper";
    }
    throw std::runtime_error("Unknown inspected control role");
}

std::string valueJson(const forgepipe::ui::Value &value)
{
    if (const auto *boolean = std::get_if<bool>(&value))
        return *boolean ? "true" : "false";
    if (const auto *integer = std::get_if<std::int64_t>(&value))
        return quote(std::to_string(*integer));
    return quote(std::get<std::string>(value));
}
}

std::string inspect(const Application &application, const std::set<std::string> &exposeBindings)
{
    if (!application.isOwnerThread())
        throw std::runtime_error("Inspection requires the model owner thread");
    const auto &view = application.view();
    const auto &projection = view.projection();
    for (const auto &binding : exposeBindings) {
        if (!projection.state.count(binding))
            throw std::runtime_error("Inspection exposure references an unknown binding");
    }
    std::string result = "{\"schema\":\"pipelang.pipeview.inspect.v3\",\"controls\":[";
    bool first = true;
    for (const auto &control : view.inspection()) {
        if (!first)
            result += ',';
        first = false;
        const bool exposed = !control.binding.empty() && exposeBindings.count(control.binding);
        const bool redacted = control.redacted && !exposed;
        result += "{\"id\":" + quote(control.id) + ",\"role\":" + quote(role(control.role));
        result += ",\"enabled\":" + std::string(control.enabled ? "true" : "false");
        result += ",\"action\":" + quote(control.action) + ",\"binding\":" + quote(control.binding);
        result += ",\"type\":" + quote(control.valueType);
        result += ",\"redacted\":" + std::string(redacted ? "true" : "false");
        if (exposed) {
            result += ",\"value\":" + valueJson(projection.state.at(control.binding));
        } else if (control.value && !redacted) {
            result += ",\"value\":" + valueJson(*control.value);
        }
        result += '}';
    }
    return result + "]}";
}
}
