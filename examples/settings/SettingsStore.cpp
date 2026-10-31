#include "SettingsStore.h"
#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>

namespace pipeview_settings {
namespace {
bool regularOrAbsent(const std::filesystem::path &path, bool &exists, std::string &error)
{
    if (!path.is_absolute()) {
        error = "Settings path must be absolute";
        return false;
    }
    std::error_code failure;
    const auto status = std::filesystem::symlink_status(path, failure);
    if (failure == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) {
        exists = false;
        return true;
    }
    if (failure || status.type() != std::filesystem::file_type::regular) {
        error = "Settings path must be a regular file or absent";
        return false;
    }
    exists = true;
    return true;
}
}

bool SettingsStore::load(std::optional<SettingsValue> &value, std::string &error) const
{
    value.reset();
    if (!m_path.is_absolute()) {
        error = "Settings path must be absolute";
        return false;
    }
    const int file = ::open(m_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (file < 0) {
        if (errno == ENOENT) {
            error.clear();
            return true;
        }
        error = "Cannot open a regular settings file";
        return false;
    }
    struct stat status {};
    if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size >= 4140) {
        ::close(file);
        error = "Settings source must be a bounded regular file";
        return false;
    }
    std::string data(4140, '\0');
    std::size_t count = 0;
    while (count < data.size()) {
        const auto received = ::read(file, data.data() + count, data.size() - count);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0) {
            ::close(file);
            error = "Cannot read settings file";
            return false;
        }
        if (received == 0)
            break;
        count += static_cast<std::size_t>(received);
    }
    const int closed = ::close(file);
    if (closed != 0 || count == data.size()) {
        error = "Settings read failed or exceeded capacity";
        return false;
    }
    data.resize(count);
    constexpr std::string_view prefix = "pipeview.settings.v1\n";
    constexpr std::string_view currentPrefix = "pipeview.settings.v2\n";
    const bool current = data.compare(0, currentPrefix.size(), currentPrefix) == 0;
    if (!current && data.compare(0, prefix.size(), prefix) != 0) {
        error = "Unsupported settings file contract";
        return false;
    }
    const auto separator = data.find('\n', prefix.size());
    if (separator == std::string::npos) {
        error = "Settings file is truncated";
        return false;
    }
    std::size_t size = 0;
    const auto first = data.data() + prefix.size();
    const auto last = data.data() + separator;
    const auto parsed = std::from_chars(first, last, size);
    if (parsed.ec != std::errc{} || parsed.ptr != last || size > 4096
        || (current && last - first > 1 && *first == '0')
        || (!current && data.size() != separator + 1 + size + 3)
        || (current && data.size() < separator + 1 + size + 5)) {
        error = "Invalid settings file shape";
        return false;
    }
    const auto tail = separator + 1 + size;
    if (data[tail] != '\n' || (data[tail + 1] != '0' && data[tail + 1] != '1') || data[tail + 2] != '\n') {
        error = "Invalid saved VSync value";
        return false;
    }
    std::int64_t volume = 100;
    if (current) {
        const auto digits = std::string_view(data).substr(tail + 3);
        if (digits.size() < 2 || digits.size() > 4 || digits.back() != '\n') {
            error = "Invalid saved volume";
            return false;
        }
        const auto decimal = digits.substr(0, digits.size() - 1);
        if ((decimal.size() > 1 && decimal.front() == '0')
            || decimal.find_first_not_of("0123456789") != std::string_view::npos) {
            error = "Invalid saved volume";
            return false;
        }
        const auto parsedVolume = std::from_chars(decimal.data(), decimal.data() + decimal.size(), volume);
        if (parsedVolume.ec != std::errc{} || parsedVolume.ptr != decimal.data() + decimal.size()
            || volume < 0 || volume > 100) {
            error = "Invalid saved volume";
            return false;
        }
    }
    auto playerName = data.substr(separator + 1, size);
    try {
        pipelang::model::validate(playerName);
    } catch (const pipelang::model::ExecutionFailure &failure) {
        error = "Invalid saved player name: " + std::string(failure.what());
        return false;
    }
    value = SettingsValue{std::move(playerName), data[tail + 1] == '1', volume};
    error.clear();
    return true;
}

bool SettingsStore::save(const SettingsValue &value, std::string &error) const
{
    bool exists = false;
    if (!regularOrAbsent(m_path, exists, error))
        return false;
    if (value.playerName.size() > 4096) {
        error = "Player name exceeds settings capacity";
        return false;
    }
    try {
        pipelang::model::validate(value.playerName);
    } catch (const pipelang::model::ExecutionFailure &failure) {
        error = "Invalid saved player name: " + std::string(failure.what());
        return false;
    }
    if (value.volume < 0 || value.volume > 100) {
        error = "Volume must be within 0..100";
        return false;
    }
    const auto data = "pipeview.settings.v2\n" + std::to_string(value.playerName.size()) + "\n"
        + value.playerName + "\n" + (value.vsync ? "1\n" : "0\n") + std::to_string(value.volume) + "\n";
    std::string temporary = m_path.string() + ".tmp.XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end());
    name.push_back('\0');
    int file = ::mkstemp(name.data());
    if (file < 0) {
        error = "Cannot create temporary settings file";
        return false;
    }
    const auto fail = [&](const std::string &message) {
        if (file >= 0)
            ::close(file);
        ::unlink(name.data());
        error = message;
        return false;
    };
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::write(file, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return fail("Cannot write settings file");
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(file) != 0)
        return fail("Cannot flush settings file");
    const int closed = ::close(file);
    file = -1;
    if (closed != 0)
        return fail("Cannot close settings file");
    if (::rename(name.data(), m_path.c_str()) != 0)
        return fail("Cannot replace settings file");
    error.clear();
    return true;
}
}
