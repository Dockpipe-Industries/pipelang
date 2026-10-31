// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../support/support.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace pipelang::tools;
std::size_t failures = 0;
fs::path group() {
    const auto contents = readFile("/proc/self/cgroup", 4096);
    require(contents.starts_with("0::/"), "probe requires cgroup v2");
    return fs::path("/sys/fs/cgroup") / contents.substr(4, contents.find('\n') - 4);
}
std::uint64_t counter(const fs::path& path) {
    return std::stoull(readFile(path, 4096));
}
void assertLimits(bool coordinator) {
    const auto current = group();
    require(counter(current / "memory.max") == (coordinator ? 536870912ULL : 1073741824ULL), "probe found incorrect memory.max");
    require(counter(current / "memory.swap.max") == 0, "probe found swap enabled");
    require(counter(current / "pids.max") == (coordinator ? 64 : 128), "probe found incorrect pids.max");
    if (!coordinator)
        require(counter(current / "memory.high") == 734003200ULL, "probe found incorrect memory.high");
    const auto aggregate = current.parent_path();
    const char* declaredAggregate = std::getenv("CONTAINED_JOB_GROUP");
    require(declaredAggregate && aggregate == fs::path(declaredAggregate), "probe is outside aggregate");
    require(counter(aggregate / "memory.max") == 2147483648ULL &&
            counter(aggregate / "memory.high") == 1610612736ULL &&
            counter(aggregate / "memory.swap.max") == 0 &&
            counter(aggregate / "pids.max") == 384, "probe found incorrect aggregate limits");
}
int workload(const std::string& mode, const fs::path& marker) {
    assertLimits(false);
    atomicWrite(marker, "verified workload started\n");
    if (mode == "normal")
        return 0;
    if (mode == "error")
        return 17;
    if (mode == "memory") {
        std::vector<std::vector<unsigned char>> blocks;
        for (std::size_t index = 0; index < 950; ++index) {
            blocks.emplace_back(1U << 20, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }
    if (mode == "logs") {
        const std::string block(8192, 'x');
        for (std::size_t index = 0; index < 12000; ++index)
            std::cout << block;
        return 0;
    }
#ifndef _WIN32
    if (mode == "descendant") {
        const auto child = ::fork();
        require(child >= 0, "cannot create teardown probe");
        if (child == 0) {
            ::setsid();
            std::signal(SIGTERM, SIG_IGN);
            atomicWrite(marker.string() + ".pid", std::to_string(::getpid()));
            while (true)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
#endif
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
void inside(const fs::path& runner, const fs::path& self, const fs::path& output) {
    assertLimits(true);
    const auto unit = [&](const std::string& mode, bool success, int timeout = 10) {
        const auto prefix = output / mode;
        const auto marker = output / (mode + ".marker");
        const auto result = process({runner.string(), "unit", "--output", prefix.string(), "--estate", output.parent_path().string(),
            "--timeout", std::to_string(timeout), "--", self.string(), "--workload", mode, marker.string()}, {}, std::chrono::seconds(timeout + 20));
        const auto receipt = parseJson(readFile(prefix.string() + ".json", 65536));
        require(receipt.at("tree_removed") == true, "probe found an unreaped process cgroup");
        require(fs::exists(marker), "probe failed before executing the independently checked workload");
        require((result.exit == 0) == success, "unexpected containment success/failure for " + mode + ": " + result.output + result.diagnostic);
        require(receipt.at("limits").at("memory.max") == 1073741824ULL && receipt.at("swap_current") == 0,
                "receipt does not preserve required limits");
        if (success)
            require(receipt.at("outcome") == "completed" && receipt.at("workload").at("exit") == 0, "successful receipt is incomplete");
        else {
            require(receipt.at("outcome") == "failed", "failed workload was accepted");
            ++failures;
        }
        return receipt;
    };
    unit("normal", true);
    const auto error = unit("error", false);
    require(error.at("workload").at("exit") == 17, "child failure exit was lost");
    unit("timeout", false, 1);
    unit("descendant", false, 1);
#ifndef _WIN32
    const auto descendant = std::stol(readFile(output / "descendant.marker.pid", 128));
    if (::kill(static_cast<pid_t>(descendant), 0) == 0) {
        const auto status = readFile(fs::path("/proc") / std::to_string(descendant) / "stat", 4096);
        const auto nameEnd = status.rfind(')');
        require(nameEnd != std::string::npos && status.substr(nameEnd + 2, 1) == "Z", "escaped-session descendant survived cleanup");
    }
#endif
    unit("memory", false);
    unit("logs", false);
    const auto receipt = output / "normal.json";
    const auto before = fileHash(receipt);
    const auto repeat = process({runner.string(), "unit", "--output", (output / "normal").string(), "--", self.string(), "--workload", "normal", (output / "repeat.marker").string()});
    require(repeat.exit != 0 && fileHash(receipt) == before && !fs::exists(output / "repeat.marker"), "receipt reuse overwrote proof or launched a workload");
    ++failures;
    const auto storage = process({runner.string(), "unit", "--output", (output / "storage").string(),
        "--estate", output.string(), "--estate-limit-mib", "1", "--", self.string(),
        "--workload", "normal", (output / "storage.marker").string()});
    const auto storageReceipt = parseJson(readFile(output / "storage.json", 65536));
    require(storage.exit != 0 && !fs::exists(output / "storage.marker") &&
            storageReceipt.at("tree_removed") == true && storageReceipt.at("error").get<std::string>().find("estate") != std::string::npos,
            "estate limit did not refuse before launch");
    ++failures;
#ifndef _WIN32
    const auto cancelledPrefix = output / "cancelled";
    const auto child = ::fork();
    require(child >= 0, "cannot create cancellation probe");
    if (child == 0) {
        const auto prefix = cancelledPrefix.string();
        const auto executable = self.string();
        const auto marker = (output / "cancelled.marker").string();
        ::execl(runner.c_str(), runner.c_str(), "unit", "--output", prefix.c_str(), "--timeout", "30", "--",
                executable.c_str(), "--workload", "timeout", marker.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists(output / "cancelled.marker")) {
        require(std::chrono::steady_clock::now() < deadline, "cancellation probe did not start");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::kill(child, SIGTERM);
    int status = 0;
    require(::waitpid(child, &status, 0) == child, "cannot reap cancellation launcher");
    const auto cancelled = parseJson(readFile(cancelledPrefix.string() + ".json", 65536));
    require(cancelled.at("outcome") == "cancelled" && cancelled.at("tree_removed") == true,
            "cancellation did not retain proof and clean the process tree");
    ++failures;
#endif
    atomicWrite(output / "inside-acceptance.json", canonical({{"status", "passed"}, {"failure_probes", failures}}));
}
}

int main(int argc, char** argv) {
    using namespace pipelang::tools;
    try {
        require(argc >= 2, "containment probe requires a mode");
        const std::string mode = argv[1];
        if (mode == "--admit") {
            require(argc == 5, "admission requires runner, prior acceptance and exact receipt SHA256");
            const auto bytes = readFile(argv[3], 65536);
            require(sha256(bytes) == argv[4], "prior containment acceptance checksum differs");
            const auto acceptance = parseJson(bytes);
            exactObject(acceptance, {"status", "runner_sha256", "failure_probes", "limits_checked_independently", "aggregate_tree_removed"}, {}, "containment acceptance");
            require(acceptance.at("status") == "passed" && acceptance.at("runner_sha256") == fileHash(argv[2]) &&
                acceptance.at("failure_probes") == 9 && acceptance.at("limits_checked_independently") == true &&
                acceptance.at("aggregate_tree_removed") == true, "prior containment acceptance does not qualify this exact runner");
            std::cout << "native containment: exact runner and prior independent qualification admitted\n";
            return 0;
        }
        if (mode == "--workload") {
            require(argc == 4, "workload requires a kind and marker");
            return workload(argv[2], argv[3]);
        }
        if (mode == "--inside") {
            require(argc == 4, "inside probe requires runner and output");
            inside(argv[2], fs::canonical(argv[0]), argv[3]);
            return 0;
        }
        require(mode == "--suite" && argc == 4, "usage: containment-probe --suite RUNNER OUTPUT");
        const fs::path runner = fs::canonical(argv[2]);
        const fs::path output = fs::absolute(argv[3]);
        require(!fs::exists(output), "probe output must be fresh");
        fs::create_directories(output);
        const auto self = fs::canonical(argv[0]);
        const auto direct = process({runner.string(), "--inside-unit", "--output", (output / "uncontained").string(), "--",
            self.string(), "--workload", "normal", (output / "uncontained.marker").string()});
        require(direct.exit != 0 && !fs::exists(output / "uncontained.marker"), "inside flag bypassed containment");
        const auto result = process({runner.string(), "job", "--output", (output / "job").string(),
            "--estate", output.parent_path().string(), "--timeout", "180", "--", self.string(), "--inside", runner.string(), output.string()}, {}, std::chrono::seconds(200));
        const auto job = parseJson(readFile(output / "job.json", 65536));
        require(result.exit == 0 && job.at("outcome") == "completed" && job.at("tree_removed") == true,
                "native aggregate probe failed: " + result.output + result.diagnostic);
        const auto inside = parseJson(readFile(output / "inside-acceptance.json", 65536));
        atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"runner_sha256", fileHash(runner)},
            {"failure_probes", inside.at("failure_probes").get<int>() + 1}, {"limits_checked_independently", true},
            {"aggregate_tree_removed", true}}));
        std::cout << "native containment: independent limits, failures, timeout, detached descendant, memory, logs and receipt preservation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
