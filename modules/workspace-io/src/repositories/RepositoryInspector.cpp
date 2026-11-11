// SPDX-License-Identifier: Apache-2.0
#include "RepositoryInspector.hpp"
#include "../primitives/OwnedFd.hpp"
#include "../primitives/WorkspacePath.hpp"
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <thread>
#include <vector>

namespace pipelang::workspace {
namespace {
struct ProcessResult {
    int exit = 0;
    std::string output;
};
ProcessResult runGit(int directory, const std::string& executable, const std::vector<std::string>& arguments,
                     const Cancellation& cancelled, std::chrono::steady_clock::time_point deadline) {
    checkCancellation(cancelled);
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0)
        throw WorkspaceError("repository_process_failure");
    OwnedFd reader(descriptors[0]);
    OwnedFd writer(descriptors[1]);
    OwnedFd discard(::open("/dev/null", O_RDWR | O_CLOEXEC));
    std::vector<std::string> values{executable, "--no-pager", "--literal-pathspecs"};
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& value : values)
        argv.push_back(value.data());
    argv.push_back(nullptr);
    std::string home = std::string("HOME=") + (::getenv("HOME") ? ::getenv("HOME") : "/");
    std::array<std::string, 5> environment{"PATH=/usr/bin:/bin", "LANG=C", "GIT_TERMINAL_PROMPT=0", "GIT_OPTIONAL_LOCKS=0", home};
    std::array<char*, 6> env{};
    for (std::size_t i = 0; i < environment.size(); ++i)
        env[i] = environment[i].data();
    pid_t child = ::fork();
    if (child < 0)
        throw WorkspaceError("repository_process_failure");
    if (child == 0) {
        ::setpgid(0, 0);
        if (::fchdir(directory) != 0 || ::dup2(discard.get(), STDIN_FILENO) < 0 ||
            ::dup2(writer.get(), STDOUT_FILENO) < 0 || ::dup2(discard.get(), STDERR_FILENO) < 0)
            ::_exit(126);
        ::execve(executable.c_str(), argv.data(), env.data());
        ::_exit(127);
    }
    ::setpgid(child, child);
    writer = OwnedFd();
    bool reaped = false;
    auto stop = [&] {
        if (!reaped) {
            ::kill(-child, SIGKILL);
            ::kill(child, SIGKILL);
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            reaped = true;
        }
    };
    try {
        ProcessResult result;
        bool ended = false;
        int status = 0;
        std::array<char, 4096> buffer{};
        for (;;) {
            checkCancellation(cancelled);
            if (std::chrono::steady_clock::now() >= deadline)
                throw WorkspaceError("repository_timeout");
            for (;;) {
                auto count = ::read(reader.get(), buffer.data(), buffer.size());
                if (count > 0) {
                    if (result.output.size() + static_cast<std::size_t>(count) > 16384)
                        throw WorkspaceError("repository_output_limit");
                    result.output.append(buffer.data(), static_cast<std::size_t>(count));
                } else if (count < 0 && errno == EINTR) {
                    continue;
                } else if (count < 0 && errno != EAGAIN) {
                    throw WorkspaceError("repository_process_failure");
                } else {
                    break;
                }
            }
            if (ended) {
                if (!WIFEXITED(status))
                    throw WorkspaceError("repository_process_failure");
                result.exit = WEXITSTATUS(status);
                return result;
            }
            auto waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child) {
                ended = true;
                reaped = true;
            } else if (waited < 0 && errno != EINTR) {
                throw WorkspaceError("repository_process_failure");
            }
            struct pollfd ready{reader.get(), POLLIN, 0};
            ::poll(&ready, 1, 10);
        }
    } catch (...) {
        stop();
        throw;
    }
}
std::string line(std::string value) {
    if (!value.empty() && value.back() == '\n')
        value.pop_back();
    if (value.find('\0') != std::string::npos || value.find('\n') != std::string::npos || value.size() > 4096)
        throw WorkspaceError("repository_invalid_signal");
    WorkspacePath::validateText(value);
    return value;
}
}
RepositoryInspector::RepositoryInspector(std::unordered_map<std::string, std::string> admitted, Cancellation cancelled,
                                         std::string executable, std::chrono::milliseconds budget)
    : admitted_(std::move(admitted)), cancelled_(std::move(cancelled)), executable_(std::move(executable)), budget_(budget) {
    if (admitted_.size() > 64 || executable_.empty() || executable_.front() != '/' || budget_.count() < 1 || budget_.count() > 2000)
        throw WorkspaceError("repository_configuration");
    for (const auto& [alias, path] : admitted_) {
        if (alias.empty() || alias.size() > 128 || alias.find('\0') != std::string::npos)
            throw WorkspaceError("repository_configuration");
        (void)WorkspacePath::openAbsoluteDirectory(path);
    }
}
RepositoryData RepositoryInspector::inspect(const std::string& alias) const {
    checkCancellation(cancelled_);
    auto found = admitted_.find(alias);
    if (found == admitted_.end())
        throw WorkspaceError("repository_not_admitted");
    auto directory = WorkspacePath::openAbsoluteDirectory(found->second);
    struct stat checkout{};
    if (::fstat(directory.get(), &checkout) != 0)
        throw WorkspaceError("repository_invalid_signal");
    auto deadline = std::chrono::steady_clock::now() + budget_;
    auto root = runGit(directory.get(), executable_, {"rev-parse", "--path-format=absolute", "--show-toplevel"}, cancelled_, deadline);
    if (root.exit != 0)
        throw WorkspaceError("repository_process_failure");
    auto inspectedRoot = WorkspacePath::openAbsoluteDirectory(line(root.output));
    struct stat actual{};
    if (::fstat(inspectedRoot.get(), &actual) != 0 || !WorkspacePath::sameFile(checkout, actual))
        throw WorkspaceError("repository_root_mismatch");
    auto common = runGit(directory.get(), executable_, {"rev-parse", "--path-format=absolute", "--git-common-dir"}, cancelled_, deadline);
    if (common.exit != 0)
        throw WorkspaceError("repository_process_failure");
    auto commonDirectory = WorkspacePath::openAbsoluteDirectory(line(common.output));
    struct stat commonInfo{};
    if (::fstat(commonDirectory.get(), &commonInfo) != 0)
        throw WorkspaceError("repository_invalid_signal");
    auto remote = runGit(directory.get(), executable_, {"config", "--null", "--get-all", "remote.origin.url"}, cancelled_, deadline);
    if (remote.exit != 0 && remote.exit != 1)
        throw WorkspaceError("repository_process_failure");
    std::string origin;
    if (remote.exit == 0) {
        if (remote.output.empty() || remote.output.back() != '\0')
            throw WorkspaceError("repository_invalid_signal");
        remote.output.pop_back();
        origin = line(remote.output);
    }
    return RepositoryData{alias, WorkspacePath::identity(checkout), WorkspacePath::identity(commonInfo), origin};
}
}
