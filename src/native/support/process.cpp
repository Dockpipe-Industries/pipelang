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
#ifdef __APPLE__
#include <crt_externs.h>
#include <spawn.h>
#endif
#endif

namespace pipelang::tools {
#ifndef _WIN32
namespace {
#ifdef __APPLE__
class SpawnConfiguration {
public:
    ~SpawnConfiguration() {
        if (actionsInitialized_)
            ::posix_spawn_file_actions_destroy(&actions);
        if (attributesInitialized_)
            ::posix_spawnattr_destroy(&attributes);
    }
    int configure(int input, int output, int diagnostic, const char*& stage) {
        stage = "spawn attributes init";
        int failure = ::posix_spawnattr_init(&attributes);
        if (failure != 0)
            return failure;
        attributesInitialized_ = true;
        stage = "spawn file actions init";
        failure = ::posix_spawn_file_actions_init(&actions);
        if (failure != 0)
            return failure;
        actionsInitialized_ = true;
        stage = "spawn private group";
        failure = ::posix_spawnattr_setpgroup(&attributes, 0);
        if (failure != 0)
            return failure;
        failure = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (failure != 0)
            return failure;
        stage = "spawn dup2 stdin";
        failure = ::posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
        if (failure != 0)
            return failure;
        stage = "spawn dup2 stdout";
        failure = ::posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
        if (failure != 0)
            return failure;
        stage = "spawn dup2 stderr";
        return ::posix_spawn_file_actions_adddup2(&actions, diagnostic, STDERR_FILENO);
    }
    posix_spawnattr_t attributes{};
    posix_spawn_file_actions_t actions{};
private:
    bool attributesInitialized_ = false;
    bool actionsInitialized_ = false;
};

bool executableSpawnFailure(int failure) {
    switch (failure) {
    case ENOENT:
    case ENOTDIR:
    case EACCES:
    case ENOEXEC:
    case ELOOP:
    case ENAMETOOLONG:
    case ETXTBSY:
    case E2BIG:
        return true;
    default:
        return false;
    }
}
#endif

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
    return windowsProcess(arguments, input, timeout, maximumOutput, maximumDiagnostic, monitor, log);
#else
    Pipe source;
    Pipe output;
    Pipe diagnostic;
    std::vector<char*> argv;
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
#ifdef __APPLE__
    const auto failedSpawn = [&](const char* stage, int failure, int exit) {
        ProcessResult result;
        result.exit = exit;
        result.diagnostic = "native process " + std::string(stage) + " failed (errno " + std::to_string(failure) + ")\n";
        require(result.diagnostic.size() <= maximumDiagnostic, "native process output exceeds its capacity");
        if (!log.empty()) {
            std::ofstream stream(log, std::ios::binary | std::ios::trunc);
            require(stream.good(), "cannot open native process log");
            stream << result.diagnostic;
            stream.flush();
            require(stream.good(), "cannot write native process log");
        }
        return result;
    };
    pid_t child = -1;
    {
        SpawnConfiguration configuration;
        const char* stage = nullptr;
        const int setupFailure = configuration.configure(source.descriptors[0], output.descriptors[1],
                                                          diagnostic.descriptors[1], stage);
        if (setupFailure != 0)
            return failedSpawn(stage, setupFailure, 126);
        // Apple applies the private-group attribute and pipe actions before the
        // new image executes. Group zero means PGID == the new child's PID.
        // No child PID is valid on failure; never poll, signal or wait for it.
        const int failure = ::posix_spawn(&child, argv[0], &configuration.actions,
                                          &configuration.attributes, argv.data(), *_NSGetEnviron());
        if (failure != 0) {
            if (executableSpawnFailure(failure))
                return failedSpawn("exec", failure, 127);
            return failedSpawn("spawn", failure, 126);
        }
    }
#else
    const auto child = ::fork();
    require(child >= 0, "cannot create native process");
    if (child == 0) {
        // After fork, report failures using only stack storage and write. Setup
        // failures use the pipe directly because stderr may not be connected yet.
        const auto emit = [](int descriptor, const char* bytes, std::size_t remaining) {
            while (remaining > 0) {
                const auto written = ::write(descriptor, bytes, remaining);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    return;
                bytes += written;
                remaining -= static_cast<std::size_t>(written);
            }
        };
        const auto reportFailure = [&](int descriptor, const char* stage, std::size_t stageLength, int failure) {
            constexpr char prefix[] = "native process ";
            emit(descriptor, prefix, sizeof(prefix) - 1);
            emit(descriptor, stage, stageLength);
            constexpr char reason[] = " failed (errno ";
            emit(descriptor, reason, sizeof(reason) - 1);
            std::array<char, 16> digits{};
            std::size_t count = 0;
            unsigned number = static_cast<unsigned>(failure);
            do {
                digits[count++] = static_cast<char>('0' + number % 10);
                number /= 10;
            } while (number != 0 && count < digits.size());
            while (count > 0) {
                --count;
                emit(descriptor, &digits[count], 1);
            }
            constexpr char suffix[] = ")\n";
            emit(descriptor, suffix, sizeof(suffix) - 1);
        };
        const auto failSetup = [&](const auto& stage) {
            const int failure = errno;
            reportFailure(diagnostic.descriptors[1], stage, sizeof(stage) - 1, failure);
            ::_exit(126);
        };
        // Parent and child both establish this group to cover either fork
        // scheduling order. An already established child-led group satisfies
        // ownership; a failed syscall alone never permits execution to proceed.
        const pid_t privateGroup = ::getpid();
        if (::getpgrp() != privateGroup && ::setpgid(0, 0) != 0) {
            const int failure = errno;
            if (::getpgrp() != privateGroup) {
                errno = failure;
                failSetup("setpgid");
            }
        }
        if (::dup2(source.descriptors[0], STDIN_FILENO) < 0)
            failSetup("dup2 stdin");
        if (::dup2(output.descriptors[1], STDOUT_FILENO) < 0)
            failSetup("dup2 stdout");
        if (::dup2(diagnostic.descriptors[1], STDERR_FILENO) < 0)
            failSetup("dup2 stderr");
        ::execv(argv[0], argv.data());
        const int failure = errno;
        constexpr char stage[] = "exec";
        reportFailure(STDERR_FILENO, stage, sizeof(stage) - 1, failure);
        ::_exit(127);
    }
    ::setpgid(child, child);
#endif
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
