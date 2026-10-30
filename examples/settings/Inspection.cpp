#include "Inspection.h"

#include <map>
#include <stdexcept>

namespace pipeview_settings {
namespace {

using Actions = std::map<std::string, model::Action>;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string boolean(bool value)
{
    return value ? "true" : "false";
}

std::string quote(const std::string& value)
{
    require(forgepipe::ui::validUtf8(value), "Inspection text is not valid UTF-8");
    static constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(character));
        } else if (character < 0x20) {
            result += "\\u00";
            result.push_back(hexadecimal[character >> 4]);
            result.push_back(hexadecimal[character & 0x0f]);
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    result.push_back('"');
    return result;
}

std::string valueType(const model::Value& value)
{
    require(!value.valueless_by_exception(), "Inspection value has no scalar type");
    if (std::holds_alternative<bool>(value))
        return "bool";
    if (std::holds_alternative<std::int64_t>(value))
        return "int64";
    return "string";
}

std::string payloadType(model::ValueKind kind)
{
    switch (kind) {
    case model::ValueKind::Boolean:
        return "bool";
    case model::ValueKind::Integer:
        return "int64";
    case model::ValueKind::Text:
        return "string";
    case model::ValueKind::Invocation:
        return "invocation";
    default:
        throw std::runtime_error("Unknown inspection payload type");
    }
}

std::string operationName(model::Operation operation)
{
    switch (operation) {
    case model::Operation::Set:
        return "set";
    case model::Operation::Save:
        return "save";
    case model::Operation::Revert:
        return "revert";
    default:
        throw std::runtime_error("Unknown inspection operation");
    }
}

std::string controlKind(forgepipe::ui::ControlType type)
{
    switch (type) {
    case forgepipe::ui::ControlType::Column:
        return "column";
    case forgepipe::ui::ControlType::Row:
        return "row";
    case forgepipe::ui::ControlType::Label:
        return "label";
    case forgepipe::ui::ControlType::Button:
        return "button";
    case forgepipe::ui::ControlType::Toggle:
        return "toggle";
    case forgepipe::ui::ControlType::PercentStepper:
        return "percent-stepper";
    case forgepipe::ui::ControlType::TextField:
        return "text-field";
    default:
        throw std::runtime_error("Unknown inspection control kind");
    }
}

void validateValues(const model::Values& declared, const model::Values& values)
{
    require(declared.size() == values.size(), "Inspection snapshot has unexpected states");
    for (const auto& [identity, initial] : declared) {
        const auto found = values.find(identity);
        require(found != values.end() && valueType(found->second) == valueType(initial),
            "Inspection snapshot has an unknown state or scalar type");
        if (const auto text = std::get_if<std::string>(&found->second))
            require(text->size() <= 4096 && forgepipe::ui::validUtf8(*text),
                "Inspection snapshot exceeds the UTF-8 text profile");
    }
}

std::string bindingJson(const model::Value& value, bool readOnly, bool expose)
{
    std::string result = "{\"type\":" + quote(valueType(value))
        + ",\"readonly\":" + boolean(readOnly) + ",\"redacted\":" + boolean(!expose);
    if (expose) {
        result += ",\"value\":";
        if (const auto scalar = std::get_if<bool>(&value)) {
            result += boolean(*scalar);
        } else if (const auto scalar = std::get_if<std::int64_t>(&value)) {
            result += quote(std::to_string(*scalar));
        } else {
            result += quote(std::get<std::string>(value));
        }
    }
    return result + "}";
}

std::string nodeJson(const forgepipe::ui::Node& node, const Actions& actions,
    const model::Values& bindings, const model::Snapshot& snapshot, std::set<std::string>& usedActions)
{
    std::string result = "{\"id\":" + quote(node.id) + ",\"kind\":" + quote(controlKind(node.type))
        + ",\"text\":" + quote(node.text) + ",\"enabled\":" + boolean(node.enabled);
    if (!node.binding.empty()) {
        require(bindings.count(node.binding) != 0, "Inspection control references an unknown binding");
        result += ",\"binding\":" + quote(node.binding);
    }
    if (!node.action.empty()) {
        const auto action = actions.find(node.action);
        require(action != actions.end(), "Inspection control references an unknown action");
        const auto& definition = action->second;
        if (definition.operation == model::Operation::Set) {
            require(node.binding == definition.target,
                "Inspection control binding differs from its typed action target");
            const bool matches = (node.type == forgepipe::ui::ControlType::Toggle
                    && definition.payloadKind == model::ValueKind::Boolean)
                || (node.type == forgepipe::ui::ControlType::TextField
                    && definition.payloadKind == model::ValueKind::Text)
                || (node.type == forgepipe::ui::ControlType::PercentStepper
                    && definition.payloadKind == model::ValueKind::Integer);
            require(matches, "Inspection control differs from its typed action payload");
        } else {
            require(node.type == forgepipe::ui::ControlType::Button && node.binding.empty()
                    && definition.payloadKind == model::ValueKind::Invocation,
                "Inspection command control differs from its typed action");
        }
        usedActions.insert(node.action);
        const bool available = node.enabled && (definition.operation == model::Operation::Save
            ? snapshot.canSave : !snapshot.saving);
        result += ",\"action\":" + quote(node.action) + ",\"action_available\":" + boolean(available);
    }
    if (!node.children.empty()) {
        result += ",\"children\":[";
        bool first = true;
        for (const auto& child : node.children) {
            if (!first)
                result += ',';
            first = false;
            result += nodeJson(child, actions, bindings, snapshot, usedActions);
        }
        result += ']';
    }
    return result + "}";
}

}

std::string inspect(const Application& application, const std::set<std::string>& exposeBindings)
{
    require(application.isOwnerThread(), "Inspection requires the model owner thread");
    const auto snapshot = application.snapshot();
    const auto definition = compiled_ui::model_definition();
    validateValues(definition.initial, snapshot.draft);
    validateValues(definition.initial, snapshot.saved);
    const bool dirty = snapshot.draft != snapshot.saved;
    require(snapshot.dirty == dirty && snapshot.canSave == (dirty && snapshot.valid && !snapshot.saving),
        "Inspection snapshot flags are inconsistent");
    require(!snapshot.saving || (snapshot.dirty && snapshot.valid), "Inspection save requires a valid dirty snapshot");
    require(snapshot.status.size() <= 4096 && forgepipe::ui::validUtf8(snapshot.status),
        "Inspection status exceeds the UTF-8 text profile");

    Actions actions;
    for (const auto& action : definition.actions) {
        operationName(action.operation);
        payloadType(action.payloadKind);
        require(actions.emplace(action.name, action).second, "Inspection action identity is duplicated");
    }

    forgepipe::ui::Document declaredDocument;
    forgepipe::ui::State declaredBindings;
    std::string error;
    require(forgepipe::ui::Document::decode(compiled_ui::view(), declaredDocument, declaredBindings, error),
        "Inspection document declaration is invalid");
    const auto statusBindings = compiled_ui::status_bindings();
    require(declaredBindings.size() == definition.initial.size() + statusBindings.size(),
        "Inspection document bindings differ from the model declaration");
    for (const auto& [identity, initial] : definition.initial) {
        const auto found = declaredBindings.find(identity);
        require(found != declaredBindings.end() && found->second == initial,
            "Inspection document state differs from the model declaration");
    }
    for (const auto& [binding, owner] : statusBindings) {
        const auto action = actions.find(owner);
        require(action != actions.end() && action->second.operation == model::Operation::Save
                && binding == owner + ".status" && definition.initial.count(binding) == 0,
            "Inspection derived status requires its declared save owner");
        const auto declared = declaredBindings.find(binding);
        require(declared != declaredBindings.end() && declared->second == model::Value{std::string{"Saved"}},
            "Inspection status binding must declare the initial Saved text");
    }
    for (const auto& binding : exposeBindings)
        require(declaredBindings.count(binding) != 0, "Inspection exposure references an unknown binding");

    const auto& document = application.session()->document();
    require(document.validate(declaredBindings, error), "Inspection live document is invalid");
    std::set<std::string> usedActions;
    const std::string root = nodeJson(document.root, actions, declaredBindings, snapshot, usedActions);
    require(usedActions.size() == actions.size(), "Inspection document actions differ from the model declaration");

    std::map<std::string, std::string> bindings;
    for (const auto& [identity, value] : snapshot.draft)
        bindings.emplace(identity, bindingJson(value, false, exposeBindings.count(identity) != 0));
    for (const auto& [binding, owner] : statusBindings) {
        (void)owner;
        bindings.emplace(binding, bindingJson(model::Value{snapshot.status}, true, exposeBindings.count(binding) != 0));
    }

    std::string result = "{\"schema\":\"pipelang.pipeview.inspect.v2\",\"root\":" + root + ",\"bindings\":{";
    bool first = true;
    for (const auto& [identity, binding] : bindings) {
        if (!first)
            result += ',';
        first = false;
        result += quote(identity) + ':' + binding;
    }
    result += "},\"actions\":{";
    first = true;
    for (const auto& [identity, action] : actions) {
        if (!first)
            result += ',';
        first = false;
        result += quote(identity) + ":{\"operation\":" + quote(operationName(action.operation))
            + ",\"payload\":" + quote(payloadType(action.payloadKind));
        if (!action.target.empty())
            result += ",\"target\":" + quote(action.target);
        result += '}';
    }
    result += "},\"model\":{\"dirty\":" + boolean(snapshot.dirty) + ",\"valid\":" + boolean(snapshot.valid)
        + ",\"saving\":" + boolean(snapshot.saving) + ",\"can_save\":" + boolean(snapshot.canSave)
        + ",\"status\":" + quote(snapshot.status) + "}}";
    return result;
}

}
