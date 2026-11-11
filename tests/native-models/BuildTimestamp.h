// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../src/native/support/support.hpp"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <thread>

namespace pipelang::tests {
// Older build tools compare whole seconds. Before mutating fixture inputs,
// wait past every output's second, including generated headers, configure
// metadata, object files and executables. Never follow build-tree symlinks or
// manufacture future timestamps. The caller must have finished its build.
inline void waitForBuildTimestampTick(const std::filesystem::path& build) {
    namespace fs = std::filesystem;
    using pipelang::tools::require;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    require(fs::is_directory(fs::symlink_status(build)),
            "incremental fixture build root must be a directory without symlink redirection");
    auto latestOutput = fs::file_time_type::min();
    std::size_t entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(build)) {
        require(++entries <= 1000000, "incremental fixture build tree exceeds its entry bound");
        require(std::chrono::steady_clock::now() < deadline,
                "incremental fixture build timestamp scan exceeded five seconds");
        const auto status = entry.symlink_status();
        if (fs::is_regular_file(status)) {
            const auto modified = fs::last_write_time(entry.path());
            if (modified > latestOutput)
                latestOutput = modified;
        }
    }
    require(latestOutput != fs::file_time_type::min(),
            "incremental fixture build tree has no regular output timestamp");
    const auto latestSecond = std::chrono::floor<std::chrono::seconds>(latestOutput);
    while (std::chrono::floor<std::chrono::seconds>(fs::file_time_type::clock::now()) <= latestSecond) {
        require(std::chrono::steady_clock::now() < deadline,
                "incremental fixture did not pass the latest build timestamp within five seconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}
