#pragma once

#include "Example.h"
#include <set>
#include <string>

namespace pipeview_settings {

// Capture must run on the application owner thread.
// Binding values require explicit exposure. Static labels and model status are
// visible. Invalid declarations or exposure identities throw std::runtime_error.
std::string inspect(const Application& application, const std::set<std::string>& exposeBindings = {});

}
