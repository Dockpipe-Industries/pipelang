// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "support.hpp"
#include <algorithm>

namespace pipelang::tools {
Json parseJson(std::string_view bytes, std::size_t maximum) {
    require(bytes.size() <= maximum, "JSON exceeds its byte limit");
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 64, "JSON exceeds its nesting limit");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        else if (event == Json::parse_event_t::key)
            require(keys.back().insert(value.get<std::string>()).second, "duplicate JSON field");
        return true;
    };
    return Json::parse(bytes.begin(), bytes.end(), callback, true, false);
}
std::string canonical(const Json& value, bool ascii) {
    return value.dump(2, ' ', ascii, Json::error_handler_t::strict) + '\n';
}
void exactObject(const Json& value, const std::set<std::string>& required,
                 const std::set<std::string>& optional, const std::string& label) {
    require(value.is_object(), label + " must be an object");
    for (const auto& name : required)
        require(value.contains(name), label + " is missing " + name);
    for (auto entry = value.begin(); entry != value.end(); ++entry)
        require(required.contains(entry.key()) || optional.contains(entry.key()), label + " contains unexpected field " + entry.key());
}
std::string text(const Json& value, const std::string& label, std::size_t maximum) {
    require(value.is_string(), label + " must be text");
    const auto result = value.get<std::string>();
    require(!result.empty() && result.size() <= maximum && result.find('\0') == std::string::npos,
            label + " must be bounded nonempty text");
    return result;
}
Arguments::Arguments(int argc, char** argv, int first) {
    for (int index = first; index < argc; ++index)
        values_.emplace_back(argv[index]);
}
std::string Arguments::optional(const std::string& name, const std::string& fallback) {
    const auto position = std::find(values_.begin(), values_.end(), name);
    if (position == values_.end())
        return fallback;
    require(position + 1 != values_.end() && !(position + 1)->starts_with("--"), "missing value for " + name);
    const auto result = *(position + 1);
    values_.erase(position, position + 2);
    require(std::find(values_.begin(), values_.end(), name) == values_.end(), "duplicate argument " + name);
    return result;
}
std::string Arguments::take(const std::string& name) {
    const auto result = optional(name);
    require(!result.empty(), "required argument " + name);
    return result;
}
bool Arguments::flag(const std::string& name) {
    const auto position = std::find(values_.begin(), values_.end(), name);
    if (position == values_.end())
        return false;
    values_.erase(position);
    require(std::find(values_.begin(), values_.end(), name) == values_.end(), "duplicate argument " + name);
    return true;
}
std::vector<std::string> Arguments::rest() {
    auto result = std::move(values_);
    values_.clear();
    return result;
}
void Arguments::finish() const {
    require(values_.empty(), "unexpected arguments remain");
}
}
