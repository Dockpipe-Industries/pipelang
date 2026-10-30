#pragma once
#include <string>
#include <cstdint>

namespace pipeview_settings {
struct SettingsValue {
    std::string playerName;
    bool vsync = true;
    std::int64_t volume = 100;
};
}
