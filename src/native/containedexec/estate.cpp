// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "estate.hpp"
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>

namespace pipelang::contained {
namespace {
constexpr std::size_t maximumAttempts = 8;
constexpr std::size_t maximumEntries = 1000000;

bool disappeared(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory;
}

void requireReadable(const std::error_code& error, const std::filesystem::path& estate) {
    if (error && !disappeared(error))
        throw std::filesystem::filesystem_error("cannot inspect proof estate", estate, error);
}
}

std::uint64_t allocatedEstate(const std::filesystem::path& estate, std::uint64_t limit) {
    using namespace pipelang::tools;
    require(limit > 0, "proof estate byte limit must be positive");
    std::size_t entries = 0;
    std::uint64_t observed = 0;
    for (std::size_t attempt = 0; attempt < maximumAttempts; ++attempt) {
        std::uint64_t total = 0;
        std::error_code error;
        // Default directory options never follow directory symlinks and do not
        // skip permission errors. ENOENT can also arise while increment opens a
        // directory which disappeared after its entry was inspected.
        const auto rootStatus = std::filesystem::symlink_status(estate, error);
        requireReadable(error, estate);
        if (disappeared(error))
            continue;
        require(std::filesystem::is_directory(rootStatus),
            "proof estate root must be a directory without symlink redirection");
        std::filesystem::recursive_directory_iterator iterator(estate, error);
        requireReadable(error, estate);
        bool incomplete = disappeared(error);
        const std::filesystem::recursive_directory_iterator end;
        while (!incomplete && iterator != end) {
            require(++entries <= maximumEntries, "proof estate entry limit exceeded");
            struct stat status{};
            if (::lstat(iterator->path().c_str(), &status) != 0) {
                const std::error_code inspectionError(errno, std::generic_category());
                requireReadable(inspectionError, estate);
                incomplete = disappeared(inspectionError);
                break;
            }
            if (S_ISREG(status.st_mode)) {
                const auto blocks = static_cast<std::uint64_t>(status.st_blocks);
                require(blocks <= (limit - 1) / 512, "proof estate reached its byte limit");
                const auto bytes = blocks * 512;
                require(bytes < limit - total, "proof estate reached its byte limit");
                total += bytes;
            }
            iterator.increment(error);
            requireReadable(error, estate);
            incomplete = disappeared(error);
        }
        observed = std::max(observed, total);
        if (!incomplete)
            return observed;
        // Never accept the iterator's terminal state after an ENOENT: libstdc++
        // may have discarded siblings. Restart at the estate root instead.
    }
    throw std::runtime_error("proof estate remained unstable after 8 allocation scans");
}
}
