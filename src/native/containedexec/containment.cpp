// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "containment.hpp"
#include <algorithm>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pipelang::contained {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) {
    interrupted = 1;
}
std::string environment(const char* name) {
    const char* value = std::getenv(name);
    require(value && *value, std::string("required containment environment: ") + name);
    return value;
}
std::uint64_t number(const fs::path& path) {
    auto value = readFile(path, 4096);
    while (!value.empty() && (value.back() == '\n' || value.back() == ' '))
        value.pop_back();
    std::size_t used = 0;
    const auto result = std::stoull(value, &used);
    require(used == value.size(), "invalid cgroup counter: " + path.string());
    return result;
}
Json limits(const fs::path& group, bool job, bool coordinator = false) {
    std::uint64_t memory = 1024 * mebibyte;
    int tasks = 128;
    if (job) {
        memory = 2048 * mebibyte;
        tasks = 384;
    } else if (coordinator) {
        memory = 512 * mebibyte;
        tasks = 64;
    }
    Json expected{{"memory.max", memory}, {"memory.swap.max", 0}, {"pids.max", tasks}};
    if (job)
        expected["memory.high"] = 1536 * mebibyte;
    else if (!coordinator)
        expected["memory.high"] = 700 * mebibyte;
    for (auto property = expected.begin(); property != expected.end(); ++property)
        require(number(group / property.key()) == property.value().get<std::uint64_t>(), "verified cgroup limit differs: " + property.key());
    return expected;
}
Json events(const fs::path& group) {
    std::istringstream input(readFile(group / "memory.events", 4096));
    Json result = Json::object();
    std::string key;
    std::uint64_t value = 0;
    while (input >> key >> value)
        result[key] = value;
    return result;
}
std::uint64_t allocated(const fs::path& estate, std::uint64_t limit) {
    std::uint64_t total = 0;
    std::size_t entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(estate)) {
        require(++entries <= 1000000, "proof estate entry limit exceeded");
#ifndef _WIN32
        struct stat status{};
        if (::lstat(entry.path().c_str(), &status) != 0) {
            require(errno == ENOENT, "cannot inspect proof estate");
            continue;
        }
        if (S_ISREG(status.st_mode))
            total += static_cast<std::uint64_t>(status.st_blocks) * 512;
#else
        if (entry.is_regular_file())
            total += entry.file_size();
#endif
        require(total < limit, "proof estate reached its byte limit");
    }
    return total;
}
std::string manager(const std::vector<std::string>& arguments, bool requireSuccess = true) {
    const auto result = process(arguments, {}, std::chrono::seconds(5), 65536, 65536);
    require(!requireSuccess || result.exit == 0, "containment manager failed: " + result.output + result.diagnostic);
    return result.exit == 0 ? result.output : std::string{};
}
fs::path serviceGroup(const std::string& unit) {
    auto relative = manager({"/usr/bin/systemctl", "--user", "show", unit, "--property=ControlGroup", "--value"}, false);
    while (!relative.empty() && relative.back() == '\n')
        relative.pop_back();
    if (relative.empty())
        return {};
    require(relative.starts_with('/') && relative != "/" && relative.find("..") == std::string::npos, "invalid managed cgroup path");
    return fs::path("/sys/fs/cgroup") / relative.substr(1);
}
}
fs::path currentGroup() {
#ifndef __linux__
    throw std::runtime_error("native containment requires a separately qualified platform backend");
#else
    const auto contents = readFile("/proc/self/cgroup", 4096);
    require(contents.starts_with("0::/"), "unified cgroup v2 is unavailable");
    const auto relative = contents.substr(4, contents.find('\n') - 4);
    require(relative.find("..") == std::string::npos, "invalid current cgroup path");
    return fs::path("/sys/fs/cgroup") / relative;
#endif
}
fs::path verifyJob() {
    const fs::path group = environment("CONTAINED_JOB_GROUP");
    require(group.is_absolute() && group.filename() == environment("CONTAINED_JOB_SLICE") &&
            group.string().starts_with("/sys/fs/cgroup/"), "invalid aggregate group identity");
    bool found = false;
    for (auto current = currentGroup().parent_path(); current != current.root_path(); current = current.parent_path())
        found = found || current == group;
    require(found, "workload is outside its declared aggregate cgroup");
    limits(group, true);
    return group;
}
void verifyInside(bool job) {
    verifyJob();
    const auto group = currentGroup();
    if (job) {
        require(group.filename() == environment("CONTAINED_JOB_UNIT"), "coordinator identity differs");
        limits(group, false, true);
    } else {
        require(group.filename().string().starts_with("pipelang-unit-"), "unit identity differs");
        limits(group, false);
    }
}
int replace(const Request& request) {
    verifyInside(request.job);
#ifndef _WIN32
    if (!request.job) {
        const auto group = currentGroup();
        const fs::path accounting = request.output.string() + ".accounting.json";
        const auto before = events(group);
        Json report{{"outcome", "running"}, {"exit", nullptr}, {"cgroup", group.string()},
                    {"limits", limits(group, false)}, {"memory_events_before", before}};
        atomicWrite(accounting, canonical(report));
        try {
            const auto monitor = [&] {
                require(interrupted == 0, "contained command interrupted");
                require(number(group / "memory.current") < 800 * mebibyte, "proactive memory stop");
            };
            std::signal(SIGTERM, interrupt);
            std::signal(SIGINT, interrupt);
            const auto result = process(request.command, {}, request.timeout, 64U << 20, 1U << 20,
                                        monitor, request.output.string() + ".output");
            report["exit"] = result.exit;
            report["outcome"] = result.exit == 0 ? "completed" : "failed";
        } catch (const std::exception& error) {
            report["outcome"] = interrupted ? "cancelled" : "failed";
            report["error"] = error.what();
        }
        report["aggregate_peak_bytes"] = number(group / "memory.peak");
        report["swap_current"] = number(group / "memory.swap.current");
        report["memory_events_after"] = events(group);
        for (const auto& event : {"oom", "oom_kill"}) {
            if (report["memory_events_after"].value(event, 0ULL) != before.value(event, 0ULL))
                report["outcome"] = "failed";
        }
        atomicWrite(accounting, canonical(report));
        return report["outcome"] == "completed" && report["exit"] == 0 ? 0 : 1;
    }
    std::vector<char*> arguments;
    for (const auto& argument : request.command)
        arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);
    ::execvp(arguments[0], arguments.data());
#endif
    throw std::runtime_error("cannot execute contained command");
}
int execute(const Request& request, const fs::path& executable) {
    require(request.output.is_absolute() && request.estate.is_absolute(), "proof and estate paths must be absolute");
    require(!request.command.empty() && request.timeout.count() > 0 && request.timeout.count() <= (request.job ? 21600 : 1800), "invalid contained command or deadline");
    require(request.estateLimit > 0 && request.estateLimit <= 8ULL * 1024 * mebibyte, "estate limit cannot exceed 8 GiB");
    const auto receipt = fs::path(request.output.string() + ".json");
    require(!fs::exists(receipt), "use a fresh containment receipt path");
    fs::create_directories(request.output.parent_path());
    require(fs::is_directory(request.estate), "proof estate must exist");
    const auto identity = uniqueName();
    const auto slice = "pipelangjob" + identity + ".slice";
    const auto unit = std::string(request.job ? "pipelang-coordinator-" : "pipelang-unit-") + identity + ".service";
    fs::path group;
    fs::path aggregate;
    std::string aggregateName;
    std::string coordinator;
    Json report{{"protocol", "pipelang.contained-execution.v1"}, {"command", request.command},
                {"outcome", "starting"}, {"exit", nullptr}, {"tree_removed", false}, {"unit", unit}};
    report["runner_sha256"] = fileHash(executable);
    report["command_sha256"] = sha256(Json(request.command).dump());
    report["estate_limit_bytes"] = request.estateLimit;
    atomicWrite(receipt, canonical(report));
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t peak = 0;
    std::uint64_t bytes = 0;
    auto lastEstate = start - std::chrono::seconds(1);
    Json before;
    bool attempted = false;
    try {
        bytes = allocated(request.estate, request.estateLimit);
        if (request.job) {
            attempted = true;
            manager({"/usr/bin/busctl", "--user", "--timeout=5", "call", "org.freedesktop.systemd1",
                "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "StartTransientUnit", "ssa(sv)a(sa(sv))",
                slice, "fail", "4", "MemoryMax", "t", "2147483648", "MemoryHigh", "t", "1610612736",
                "MemorySwapMax", "t", "0", "TasksMax", "t", "384", "0"});
            aggregate = serviceGroup(slice);
            require(!aggregate.empty() && fs::exists(aggregate), "aggregate cgroup was not created");
            report["limits"] = limits(aggregate, true);
            aggregateName = slice;
            coordinator = unit;
            group = aggregate;
            before = events(group);
        } else {
            aggregate = verifyJob();
            aggregateName = environment("CONTAINED_JOB_SLICE");
            coordinator = environment("CONTAINED_JOB_UNIT");
        }
        report["cgroup"] = group.string();
        report["aggregate_cgroup"] = aggregate.string();
        report["outcome"] = "running";
        atomicWrite(receipt, canonical(report));
        std::vector<std::string> command{"/usr/bin/systemd-run", "--user", "--wait", "--pipe", "--collect", "--unit=" + unit,
            "--slice=" + aggregateName, "-p", request.job ? "MemoryMax=512M" : "MemoryMax=1G",
            "-p", "MemorySwapMax=0", "-p", request.job ? "TasksMax=64" : "TasksMax=128",
            "-p", "RuntimeMaxSec=" + std::to_string(request.timeout.count() + 10), "-p", "KillMode=control-group",
            "-p", "TimeoutStopSec=2", "--working-directory=" + fs::current_path().string(),
            "--setenv=CONTAINED_JOB_SLICE=" + aggregateName, "--setenv=CONTAINED_JOB_GROUP=" + aggregate.string(),
            "--setenv=CONTAINED_JOB_UNIT=" + coordinator};
        if (const char* path = std::getenv("PATH"))
            command.push_back(std::string("--setenv=PATH=") + path);
        if (!request.job) {
            command.insert(command.end(), {"-p", "MemoryHigh=700M", "-p", "BindsTo=" + coordinator, "-p", "After=" + coordinator});
        }
        command.insert(command.end(), {executable.string(), request.job ? "--inside-job" : "--inside-unit", "--"});
        if (!request.job) {
            command.pop_back();
            command.insert(command.end(), {"--output", request.output.string(), "--timeout", std::to_string(request.timeout.count()), "--"});
        }
        command.insert(command.end(), request.command.begin(), request.command.end());
        attempted = true;
        const auto monitor = [&] {
            require(interrupted == 0, "contained execution interrupted");
            const auto now = std::chrono::steady_clock::now();
            if (now - lastEstate >= std::chrono::milliseconds(200)) {
                bytes = allocated(request.estate, request.estateLimit);
                lastEstate = now;
            }
            if (group.empty()) {
                const fs::path accounting = request.output.string() + ".accounting.json";
                if (fs::exists(accounting)) {
                    const auto identity = parseJson(readFile(accounting, 65536));
                    group = identity.at("cgroup").get<std::string>();
                    before = identity.at("memory_events_before");
                    report["limits"] = identity.at("limits");
                    report["cgroup"] = group.string();
                }
            }
            if (!group.empty() && fs::exists(group / "memory.current")) {
                try {
                    peak = std::max(peak, number(group / "memory.peak"));
                    require(number(group / "memory.current") < (request.job ? 1800ULL : 800ULL) * mebibyte, "proactive memory stop");
                    report["swap_current"] = number(group / "memory.swap.current");
                    report["memory_events_after"] = events(group);
                } catch (const std::exception&) {
                    // systemd may collect a completed unit between counter reads.
                    // The in-unit final receipt remains mandatory below.
                    if (request.job || fs::exists(group))
                        throw;
                }
            }
        };
        const auto result = process(command, {}, request.timeout + std::chrono::seconds(15), 64U << 20, 1U << 20,
                                    monitor, request.output.string() + (request.job ? ".output" : ".manager.output"));
        report["exit"] = result.exit;
        report["outcome"] = result.exit == 0 ? "completed" : "failed";
        if (!request.job) {
            const auto accounting = parseJson(readFile(request.output.string() + ".accounting.json", 65536));
            group = accounting.at("cgroup").get<std::string>();
            report["cgroup"] = group.string();
            report["limits"] = accounting.at("limits");
            report["workload"] = accounting;
            report["swap_current"] = accounting.at("swap_current");
            peak = std::max(peak, accounting.at("aggregate_peak_bytes").get<std::uint64_t>());
            require(accounting.at("outcome") == "completed" && accounting.at("exit") == 0, "contained workload failed");
        }
        if (!before.is_null() && report.contains("memory_events_after")) {
            for (const auto& event : {"oom", "oom_kill"})
                require(report["memory_events_after"].value(event, 0ULL) == before.value(event, 0ULL), "cgroup OOM event observed");
        }
    } catch (const std::exception& error) {
        report["outcome"] = interrupted ? "cancelled" : "failed";
        report["error"] = error.what();
    }
    try {
        const fs::path accounting = request.output.string() + ".accounting.json";
        if (!request.job && group.empty()) {
            if (fs::exists(accounting)) {
                const auto identity = parseJson(readFile(accounting, 65536));
                group = identity.at("cgroup").get<std::string>();
                report["cgroup"] = group.string();
                report["limits"] = identity.at("limits");
            } else {
                group = serviceGroup(unit);
            }
        }
        manager({"/usr/bin/systemctl", "--user", "stop", request.job ? slice : unit}, false);
        if (!request.job && fs::exists(accounting)) {
            const auto final = parseJson(readFile(accounting, 65536));
            report["workload"] = final;
            if (final.contains("aggregate_peak_bytes"))
                peak = std::max(peak, final.at("aggregate_peak_bytes").get<std::uint64_t>());
            if (final.contains("swap_current"))
                report["swap_current"] = final.at("swap_current");
        }
        if (!attempted)
            report["tree_removed"] = true;
        else if (!group.empty())
            report["tree_removed"] = !fs::exists(group);
    } catch (const std::exception& error) {
        report["cleanup_error"] = error.what();
    }
    report["elapsed_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    report["aggregate_peak_bytes"] = peak;
    report["allocated_bytes"] = bytes;
    report["memory_events_before"] = before;
    atomicWrite(receipt, canonical(report));
    const bool accepted = report["outcome"] == "completed" && report["exit"] == 0 && report["tree_removed"] == true;
    std::cout << canonical(report);
    return accepted ? 0 : 1;
}
}
