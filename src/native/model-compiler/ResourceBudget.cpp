// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ResourceBudget.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>

namespace pipelang::models {
namespace {
using Field = std::pair<const char*, std::size_t ResourceBudget::*>;
constexpr std::array<Field, 13> fields{{
    {"input_bytes", &ResourceBudget::inputBytes},
    {"output_bytes", &ResourceBudget::outputBytes},
    {"indexed_bytes", &ResourceBudget::indexedBytes},
    {"indexed_files", &ResourceBudget::indexedFiles},
    {"directory_entries", &ResourceBudget::directoryEntries},
    {"directory_depth", &ResourceBudget::directoryDepth},
    {"source_fragments", &ResourceBudget::sourceFragments},
    {"modules", &ResourceBudget::modules},
    {"declarations", &ResourceBudget::declarations},
    {"tokens", &ResourceBudget::tokens},
    {"expressions", &ResourceBudget::expressions},
    {"generic_instances", &ResourceBudget::genericInstances},
    {"implementation_units", &ResourceBudget::implementationUnits}
}};
}
ResourceBudget ResourceBudget::fromJson(const nlohmann::json& value) {
    if (!value.is_object())
        throw std::runtime_error("compiler resource budget must be an object");
    ResourceBudget result;
    for (auto item = value.begin(); item != value.end(); ++item) {
        auto field = std::find_if(fields.begin(), fields.end(), [&](const Field& candidate) {
            return item.key() == candidate.first;
        });
        if (field == fields.end())
            throw std::runtime_error("unknown compiler resource budget field: " + item.key());
        if ((!item->is_number_unsigned() && !item->is_number_integer()) ||
            (item->is_number_integer() && !item->is_number_unsigned() && item->get<std::int64_t>() <= 0))
            throw std::runtime_error("compiler resource budget field must be a positive size_t integer: " + item.key());
        const auto number = item->get<std::uint64_t>();
        if (number == 0 || number > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("compiler resource budget field is outside positive size_t range: " + item.key());
        result.*(field->second) = static_cast<std::size_t>(number);
    }
    return result;
}
ResourceBudget ResourceBudget::fromText(std::string_view value) {
    if (value.size() > 65536)
        throw std::runtime_error("compiler resource budget profile exceeds 65536 metadata bytes");
    std::vector<std::set<std::string>> keys;
    const auto profile = nlohmann::json::parse(value, [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& item) {
        if (depth > 16)
            throw std::runtime_error("compiler resource budget JSON nesting limit exceeded");
        if (event == nlohmann::json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == nlohmann::json::parse_event_t::object_end) {
            keys.pop_back();
        } else if (event == nlohmann::json::parse_event_t::key && !keys.back().insert(item.get<std::string>()).second) {
            throw std::runtime_error("duplicate compiler resource budget field");
        }
        return true;
    });
    return fromJson(profile);
}
nlohmann::json ResourceBudget::json() const {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [name, member] : fields)
        result[name] = this->*member;
    return result;
}
bool budgetExceeded(std::size_t used, std::size_t additional, std::size_t limit) noexcept {
    return used > limit || additional > limit - used;
}
}
