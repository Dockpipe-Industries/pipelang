#pragma once
#include "PipeViewDocument.hpp"
#include <string>

namespace pipeview_settings {
struct SaveResult {
    pipelang::pipeview::state::Outcome outcome = pipelang::pipeview::state::Outcome::Failure;
    std::string error;
};
}
