// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "containment.hpp"
#include <algorithm>
#include <array>
#include <csignal>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <iostream>

namespace pipelang::contained {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) {
    interrupted = 1;
}
struct Identity {
    pid_t pid;
    std::uint64_t seconds;
    std::uint64_t microseconds;
};
bool information(pid_t pid, proc_bsdinfo& value) {
    return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &value, sizeof(value)) == static_cast<int>(sizeof(value));
}
bool same(const Identity& identity) {
    proc_bsdinfo value{};
    return information(identity.pid, value) && value.pbi_start_tvsec == identity.seconds &&
        value.pbi_start_tvusec == identity.microseconds && value.pbi_status != SZOMB;
}
class Tree {
public:
    explicit Tree(pid_t root) : root_(root) {}
    void sample() {
        const int needed = proc_listallpids(nullptr, 0);
        require(needed > 0 && needed < 1000000, "cannot bound native process inventory");
        std::vector<pid_t> pids(static_cast<std::size_t>(needed) + 1024);
        const int count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        require(count > 0 && static_cast<std::size_t>(count) < pids.size(), "native process inventory changed beyond capacity");
        std::map<pid_t, proc_bsdinfo> snapshot;
        for (int index = 0; index < count; ++index) {
            proc_bsdinfo value{};
            if (information(pids[index], value))
                snapshot.emplace(pids[index], value);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [pid, value] : snapshot) {
                if (pid == root_)
                    continue;
                const auto existing = known_.find(pid);
                if (existing != known_.end()) {
                    if (existing->second.seconds == value.pbi_start_tvsec && existing->second.microseconds == value.pbi_start_tvusec)
                        continue;
                    known_.erase(existing);
                }
                const auto parent = known_.find(static_cast<pid_t>(value.pbi_ppid));
                if (static_cast<pid_t>(value.pbi_ppid) != root_ && (parent == known_.end() || !same(parent->second)))
                    continue;
                known_.emplace(pid, Identity{pid, value.pbi_start_tvsec, value.pbi_start_tvusec});
                changed = true;
            }
        }
        memory_ = 0;
        threads_ = 0;
        for (const auto& [pid, identity] : known_) {
            if (!same(identity))
                continue;
            proc_taskinfo task{};
            require(proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) == static_cast<int>(sizeof(task)) || !same(identity),
                "cannot account a live native workload process");
            memory_ += task.pti_resident_size;
            threads_ += task.pti_threadnum;
        }
        peak_ = std::max(peak_, memory_);
    }
    bool cleanup() {
        sample();
        for (const auto& [pid, identity] : known_) {
            if (same(identity))
                require(::kill(pid, SIGKILL) == 0 || errno == ESRCH, "cannot stop observed native descendant");
        }
        for (int attempt = 0; attempt < 200; ++attempt) {
            bool alive = false;
            for (const auto& [pid, identity] : known_)
                alive = alive || same(identity);
            if (!alive)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    std::uint64_t memory() const { return memory_; }
    std::uint64_t threads() const { return threads_; }
    std::uint64_t peak() const { return peak_; }
private:
    pid_t root_;
    std::map<pid_t, Identity> known_;
    std::uint64_t memory_ = 0;
    std::uint64_t threads_ = 0;
    std::uint64_t peak_ = 0;
};
std::uint64_t allocated(const fs::path& estate, std::uint64_t limit) {
    std::uint64_t total = 0;
    std::size_t entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(estate)) {
        require(++entries <= 1000000, "proof estate entry limit exceeded");
        struct stat value{};
        if (::lstat(entry.path().c_str(), &value) != 0) {
            require(errno == ENOENT, "cannot inspect proof estate");
            continue;
        }
        if (S_ISREG(value.st_mode))
            total += static_cast<std::uint64_t>(value.st_blocks) * 512;
        require(total < limit, "proof estate reached its byte limit");
    }
    return total;
}
}
fs::path currentGroup() {
    throw std::runtime_error("macOS sampled policy does not provide a kernel process group receipt");
}
fs::path verifyJob() {
    const char* selected = std::getenv("PIPELANG_MACOS_COORDINATOR");
    require(selected && *selected, "native workload requires a macOS coordinator");
    const auto expected = std::stol(selected);
    require(expected > 1, "invalid macOS coordinator identity");
    auto current = ::getppid();
    for (int depth = 0; depth < 128 && current > 1; ++depth) {
        if (current == expected) {
            proc_bsdinfo coordinator{};
            require(information(current, coordinator), "cannot identify macOS coordinator");
            const auto identity = std::to_string(coordinator.pbi_start_tvsec) + ":" + std::to_string(coordinator.pbi_start_tvusec);
            const char* admitted = std::getenv("PIPELANG_MACOS_COORDINATOR_IDENTITY");
            require(admitted && identity == admitted, "macOS coordinator process identity differs");
            std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
            require(proc_pidpath(current, path.data(), static_cast<std::uint32_t>(path.size())) > 0 &&
                fs::canonical(path.data()) == executablePath(), "macOS coordinator executable differs");
            return fs::path(selected);
        }
        proc_bsdinfo value{};
        require(information(current, value), "cannot verify macOS coordinator ancestry");
        current = static_cast<pid_t>(value.pbi_ppid);
    }
    throw std::runtime_error("native workload is outside its macOS coordinator");
}
void verifyInside(bool) {
    verifyJob();
}
int replace(const Request&) {
    throw std::runtime_error("Linux inside modes are unavailable under the macOS sampled policy");
}
int execute(const Request& request, const fs::path& executable) {
    require(request.output.is_absolute() && request.estate.is_absolute() && fs::is_directory(request.estate),
        "native proof and estate must be absolute existing directories");
    require(request.estateLimit > 0 && request.estateLimit <= 8ULL * 1024 * mebibyte, "estate limit cannot exceed 8 GiB");
    const fs::path receipt = request.output.string() + ".json";
    require(!fs::exists(fs::symlink_status(receipt)), "use a fresh native receipt");
    if (!request.job)
        verifyInside(false);
    fs::create_directories(request.output.parent_path());
    Json report{{"protocol", "pipelang.contained-execution.v1"}, {"policy", "macos.sampled-tree.v1"},
        {"command", request.command}, {"runner_sha256", fileHash(executable)},
        {"command_sha256", sha256(Json(request.command).dump())}, {"outcome", "starting"}, {"exit", nullptr},
        {"kernel_hard_memory_limit", false}, {"swap_enforced", false}, {"escape_prevention", false},
        {"sampled_descendants_removed", false}, {"estate_limit_bytes", request.estateLimit},
        {"memory_stop_bytes", (request.job ? 1800ULL : 800ULL) * mebibyte},
        {"thread_stop_count", request.job ? 384 : 128}};
    atomicWrite(receipt, canonical(report));
    Tree tree(::getpid());
    const char* previous = std::getenv("PIPELANG_MACOS_COORDINATOR");
    const std::string saved = previous ? previous : "";
    const char* previousIdentity = std::getenv("PIPELANG_MACOS_COORDINATOR_IDENTITY");
    const std::string savedIdentity = previousIdentity ? previousIdentity : "";
    if (request.job) {
        proc_bsdinfo identity{};
        require(information(::getpid(), identity), "cannot establish coordinator identity");
        const auto value = std::to_string(identity.pbi_start_tvsec) + ":" + std::to_string(identity.pbi_start_tvusec);
        require(::setenv("PIPELANG_MACOS_COORDINATOR_IDENTITY", value.c_str(), 1) == 0, "cannot set coordinator identity");
        require(::setenv("PIPELANG_MACOS_COORDINATOR", std::to_string(::getpid()).c_str(), 1) == 0,
            "cannot establish macOS coordinator context");
    }
    const auto start = std::chrono::steady_clock::now();
    auto lastEstate = start - std::chrono::seconds(1);
    std::uint64_t bytes = 0;
    const auto oldInterrupt = std::signal(SIGINT, interrupt);
    const auto oldTerminate = std::signal(SIGTERM, interrupt);
    interrupted = 0;
    try {
        bytes = allocated(request.estate, request.estateLimit);
        const auto monitor = [&] {
            require(!interrupted, "native workload cancelled");
            tree.sample();
            require(tree.memory() < (request.job ? 1800ULL : 800ULL) * mebibyte, "proactive memory stop");
            require(tree.threads() <= (request.job ? 384ULL : 128ULL), "native thread stop");
            const auto now = std::chrono::steady_clock::now();
            if (now - lastEstate >= std::chrono::milliseconds(200)) {
                bytes = allocated(request.estate, request.estateLimit);
                lastEstate = now;
            }
        };
        const auto result = process(request.command, {}, request.timeout, 64U << 20, 1U << 20,
            monitor, request.output.string() + ".output");
        if (result.exit != 0) {
            std::cerr << result.diagnostic.substr(result.diagnostic.size() > 8192 ? result.diagnostic.size() - 8192 : 0);
            std::cerr << result.output.substr(result.output.size() > 8192 ? result.output.size() - 8192 : 0);
        }
        report["exit"] = result.exit;
        report["outcome"] = result.exit == 0 ? "completed" : "failed";
    } catch (const std::exception& error) {
        report["outcome"] = interrupted ? "cancelled" : "failed";
        report["error"] = error.what();
    }
    try {
        report["sampled_descendants_removed"] = tree.cleanup();
        bytes = allocated(request.estate, request.estateLimit);
    } catch (const std::exception& error) {
        report["outcome"] = "failed";
        report["cleanup_error"] = error.what();
    }
    std::signal(SIGINT, oldInterrupt);
    std::signal(SIGTERM, oldTerminate);
    if (request.job) {
        if (previous)
            ::setenv("PIPELANG_MACOS_COORDINATOR", saved.c_str(), 1);
        else
            ::unsetenv("PIPELANG_MACOS_COORDINATOR");
    }
    if (request.job) {
        if (previousIdentity)
            ::setenv("PIPELANG_MACOS_COORDINATOR_IDENTITY", savedIdentity.c_str(), 1);
        else
            ::unsetenv("PIPELANG_MACOS_COORDINATOR_IDENTITY");
    }
    report["aggregate_peak_bytes"] = tree.peak();
    report["allocated_bytes"] = bytes;
    report["elapsed_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    atomicWrite(receipt, canonical(report));
    std::cout << canonical(report);
    return report["outcome"] == "completed" && report["sampled_descendants_removed"] == true ? 0 : 1;
}
}
