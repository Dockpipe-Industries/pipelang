#include "runtime.hpp"

#include <charconv>
#include <iostream>
#include <sstream>
#include <string>

namespace state = pipelang::pipeview::state;

namespace {

std::string hexadecimal(const std::string& bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

int hexadecimalDigit(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

std::optional<std::string> decodeHexadecimal(const std::string& encoded)
{
    if (encoded.size() % 2 != 0)
        return std::nullopt;
    std::string result;
    result.reserve(encoded.size() / 2);
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        const int high = hexadecimalDigit(encoded[index]);
        const int low = hexadecimalDigit(encoded[index + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return result;
}

bool applyCommand(
    state::Model& model,
    const std::string& command,
    std::optional<state::SaveRequest>& request)
{
    std::string error;
    request.reset();
    if (command == "name" || command.compare(0, 5, "name ") == 0) {
        const auto decoded = decodeHexadecimal(command.size() > 4 ? command.substr(5) : "");
        if (!decoded)
            return false;
        return model.dispatch("nameEdited", state::Value{*decoded}, request, error);
    }
    if (command == "enabled 0" || command == "enabled 1")
        return model.dispatch("enabledEdited", state::Value{command.back() == '1'}, request, error);
    if (command == "save" || command == "revert")
        return model.dispatch(command, std::nullopt, request, error);
    if (command == "unknown")
        return model.dispatch("unknown", std::nullopt, request, error);
    if (command == "wrong-type")
        return model.dispatch("nameEdited", state::Value{true}, request, error);
    if (command.compare(0, 9, "complete ") == 0) {
        std::istringstream input(command.substr(9));
        std::string identity;
        std::string result;
        std::string extra;
        if (!(input >> identity >> result) || (input >> extra))
            return false;
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(identity.data(), identity.data() + identity.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != identity.data() + identity.size())
            return false;
        state::Outcome outcome;
        if (result == "success") {
            outcome = state::Outcome::Success;
        } else if (result == "failure") {
            outcome = state::Outcome::Failure;
        } else if (result == "cancelled") {
            outcome = state::Outcome::Cancelled;
        } else {
            outcome = static_cast<state::Outcome>(-1);
        }
        return model.complete(id, outcome, error);
    }
    return false;
}

} // namespace

// Line protocol: name HEXBYTES (empty bytes allowed), enabled 0|1, save,
// revert, complete ID success|failure|cancelled, unknown, or wrong-type.
// Every command emits one tab-separated row, with lowercase hex text:
// accepted dirty valid saving canSave hexStatus hexDraftName draftEnabled
// hexSavedName savedEnabled emittedRequestID. Boolean fields are 0 or 1;
// emittedRequestID is zero unless this command started a save.
int main()
{
    state::Definition definition{
        {{"name", std::string{"Rider"}}, {"enabled", true}},
        {
            {"nameEdited", state::ValueKind::Text, state::Operation::Set, "name"},
            {"enabledEdited", state::ValueKind::Boolean, state::Operation::Set, "enabled"},
            {"save", state::ValueKind::Invocation, state::Operation::Save, ""},
            {"revert", state::ValueKind::Invocation, state::Operation::Revert, ""},
        },
    };
    std::string error;
    auto model = state::Model::create(
        std::move(definition), std::nullopt,
        [](const state::Values& values) { return !std::get<std::string>(values.at("name")).empty(); },
        error);
    if (!model) {
        std::cerr << error << '\n';
        return 1;
    }

    std::string command;
    while (std::getline(std::cin, command)) {
        std::optional<state::SaveRequest> request;
        const bool accepted = applyCommand(*model, command, request);
        const state::Snapshot snapshot = model->snapshot();
        std::cout << accepted << '\t' << snapshot.dirty << '\t' << snapshot.valid << '\t'
                  << snapshot.saving << '\t' << snapshot.canSave << '\t'
                  << hexadecimal(snapshot.status) << '\t'
                  << hexadecimal(std::get<std::string>(snapshot.draft.at("name"))) << '\t'
                  << std::get<bool>(snapshot.draft.at("enabled")) << '\t'
                  << hexadecimal(std::get<std::string>(snapshot.saved.at("name"))) << '\t'
                  << std::get<bool>(snapshot.saved.at("enabled")) << '\t'
                  << (request ? request->id : 0) << '\n';
    }
    return std::cin.bad() ? 1 : 0;
}
