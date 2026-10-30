#pragma once
#include "SettingsValue.h"
#include <filesystem>
#include <optional>
#include <string>

namespace pipeview_settings {
// Linux showcase infrastructure. The path is explicit; no user profile is
// discovered and no application-wide resolver or scheduler is introduced.
class SettingsStore final {
public:
    explicit SettingsStore(std::filesystem::path path) : m_path(std::move(path)) {}
    bool load(std::optional<SettingsValue> &value, std::string &error) const;
    bool save(const SettingsValue &value, std::string &error) const;

private:
    std::filesystem::path m_path;
};
}
