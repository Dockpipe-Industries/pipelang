// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "legacy.hpp"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pipelang::legacy {
namespace {
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto fraction = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count();
    const auto raw = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#ifdef _WIN32
    require(gmtime_s(&utc, &raw) == 0, "cannot resolve operation timestamp");
#else
    require(gmtime_r(&raw, &utc) != nullptr, "cannot resolve operation timestamp");
#endif
    std::array<char, 32> buffer{};
    require(std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &utc) != 0, "cannot format operation timestamp");
    std::string result(buffer.data());
    if (fraction != 0) {
        auto digits = std::to_string(1000000000 + fraction).substr(1);
        while (digits.back() == '0')
            digits.pop_back();
        result += '.' + digits;
    }
    return result + 'Z';
}
std::string eventBytes(const nlohmann::ordered_json& value) {
    const auto bytes = value.dump();
    std::string result;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == '<')
            result += "\\u003c";
        else if (bytes[index] == '>')
            result += "\\u003e";
        else if (bytes[index] == '&')
            result += "\\u0026";
        else if (bytes.substr(index, 3) == "\xe2\x80\xa8" || bytes.substr(index, 3) == "\xe2\x80\xa9") {
            result += bytes[index + 2] == '\xa8' ? "\\u2028" : "\\u2029";
            index += 2;
        } else
            result += bytes[index];
    }
    return result + '\n';
}
void append(const fs::path& selected, const std::string& bytes) {
    require(bytes.size() <= 65536, "operation event exceeds its line capacity");
    const auto path = fs::absolute(selected).lexically_normal();
    require(fs::weakly_canonical(path.parent_path()) == path.parent_path(), "operation log has symbolic ancestors");
    fs::create_directories(path.parent_path());
    if (fs::exists(fs::symlink_status(path))) {
        regularFile(path);
        require(fs::file_size(path) <= 64U << 20, "operation log capacity exceeded");
    }
#ifndef _WIN32
    const int file = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(file >= 0, "cannot open operation log");
    try {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(file, bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR)
                continue;
            require(count > 0, "cannot append operation event");
            written += static_cast<std::size_t>(count);
        }
        require(::fsync(file) == 0, "cannot synchronize operation event");
    } catch (...) {
        ::close(file);
        throw;
    }
    require(::close(file) == 0, "cannot close operation log");
#else
    appendWindows(path, bytes, 64U << 20);
#endif
}
}
void operation(const std::string& unit, std::map<std::string, std::string>& ids, const std::function<void()>& action) {
    std::exception_ptr failure;
    std::string message;
    try {
        action();
    } catch (const std::exception& error) {
        failure = std::current_exception();
        message = error.what();
        ids["result"] = "failed";
    }
    nlohmann::ordered_json event{{"schema", "dockpipe.operation_event.v1"}, {"type", "operation_result"},
        {"ts", timestamp()}, {"unit", unit}, {"status", failure ? "fail" : "done"}};
    if (!ids.empty())
        event["ids"] = ids;
    if (failure)
        event["error"] = message;
    std::cerr << unit << ": " << (failure ? "fail" : "done") << '\n';
    const char* path = std::getenv("DOCKPIPE_EVENT_LOG");
    if (path && *path) {
        try {
            append(path, eventBytes(event));
        } catch (const std::exception& error) {
            throw std::runtime_error("record " + unit + " event: " + error.what());
        }
    }
    if (failure)
        std::rethrow_exception(failure);
}
}
