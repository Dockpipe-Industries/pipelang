// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "support.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pipelang::tools {
#ifndef _WIN32
namespace {
class Pipe {
public:
    Pipe() {
        require(::pipe(descriptors.data()) == 0, "cannot create process pipe");
        for (int descriptor : descriptors)
            require(::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0, "cannot protect process pipe");
    }
    ~Pipe() {
        close(0);
        close(1);
    }
    void close(std::size_t index) {
        if (descriptors[index] >= 0)
            ::close(std::exchange(descriptors[index], -1));
    }
    std::array<int, 2> descriptors{-1, -1};
};
}
#endif

ProcessResult process(const std::vector<std::string>& arguments, std::string_view input,
                      std::chrono::milliseconds timeout, std::size_t maximumOutput,
                      std::size_t maximumDiagnostic, const std::function<void()>& monitor,
                      const fs::path& log) {
    require(!arguments.empty() && fs::path(arguments.front()).is_absolute(), "process requires an absolute executable");
    require(timeout.count() > 0, "process requires a positive deadline");
#ifdef _WIN32
    throw std::runtime_error("native process transport has not yet qualified Windows");
#else
    Pipe source;
    Pipe output;
    Pipe diagnostic;
    std::vector<char*> argv;
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const auto child = ::fork();
    require(child >= 0, "cannot create native process");
    if (child == 0) {
        if (::setpgid(0, 0) != 0 || ::dup2(source.descriptors[0], STDIN_FILENO) < 0 ||
            ::dup2(output.descriptors[1], STDOUT_FILENO) < 0 || ::dup2(diagnostic.descriptors[1], STDERR_FILENO) < 0)
            ::_exit(126);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::setpgid(child, child);
    source.close(0);
    output.close(1);
    diagnostic.close(1);
    const auto previousSignal = std::signal(SIGPIPE, SIG_IGN);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    ProcessResult result;
    std::size_t written = 0;
    bool reaped = false;
    int status = 0;
    try {
        for (int descriptor : {source.descriptors[1], output.descriptors[0], diagnostic.descriptors[0]})
            require(::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL) | O_NONBLOCK) == 0, "cannot poll native protocol");
        std::ofstream stream;
        if (!log.empty()) {
            stream.open(log, std::ios::binary | std::ios::trunc);
            require(stream.good(), "cannot open native process log");
        }
        while (!reaped || output.descriptors[0] >= 0 || diagnostic.descriptors[0] >= 0) {
            if (monitor)
                monitor();
            require(std::chrono::steady_clock::now() < deadline, "native process or protocol stream timed out");
            if (written == input.size())
                source.close(1);
            std::array<pollfd, 3> watched{{
                {source.descriptors[1], POLLOUT, 0},
                {output.descriptors[0], POLLIN, 0},
                {diagnostic.descriptors[0], POLLIN, 0}
            }};
            const int polled = ::poll(watched.data(), watched.size(), 10);
            require(polled >= 0 || errno == EINTR, "cannot poll native process streams");
            if (source.descriptors[1] >= 0 && watched[0].revents) {
                const auto count = ::write(source.descriptors[1], input.data() + written, input.size() - written);
                if (count > 0)
                    written += static_cast<std::size_t>(count);
                else if (count < 0 && errno == EPIPE)
                    source.close(1);
                else
                    require(count >= 0 || errno == EAGAIN || errno == EINTR, "cannot write native request");
            }
            const auto receive = [&](Pipe& pipe, std::string& bytes, std::size_t maximum) {
                if (pipe.descriptors[0] < 0)
                    return;
                std::array<char, 8192> buffer{};
                while (true) {
                    const auto count = ::read(pipe.descriptors[0], buffer.data(), buffer.size());
                    if (count == 0) {
                        pipe.close(0);
                        return;
                    }
                    if (count < 0) {
                        require(errno == EAGAIN || errno == EINTR, "cannot read native protocol stream");
                        return;
                    }
                    require(static_cast<std::size_t>(count) <= maximum - bytes.size(), "native process output exceeds its capacity");
                    bytes.append(buffer.data(), static_cast<std::size_t>(count));
                    if (stream.is_open()) {
                        stream.write(buffer.data(), count);
                        stream.flush();
                        require(stream.good(), "cannot write native process log");
                    }
                }
            };
            receive(output, result.output, maximumOutput);
            receive(diagnostic, result.diagnostic, maximumDiagnostic);
            if (!reaped) {
                const auto waited = ::waitpid(child, &status, WNOHANG);
                require(waited >= 0 || errno == EINTR, "cannot observe native process");
                reaped = waited == child;
            }
        }
        result.exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    } catch (...) {
        ::kill(-child, SIGKILL);
        if (!reaped) {
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        std::signal(SIGPIPE, previousSignal);
        throw;
    }
    // A tool must not leave descendants after closing its protocol streams.
    ::kill(-child, SIGKILL);
    std::signal(SIGPIPE, previousSignal);
    return result;
#endif
}
}
