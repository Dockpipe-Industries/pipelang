// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../support/support.hpp"
#include <csignal>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <libproc.h>

namespace {
using namespace pipelang::tools;
void workload(const std::string& mode, const fs::path& marker) {
    require(std::getenv("PIPELANG_MACOS_COORDINATOR"), "probe has no native coordinator");
    atomicWrite(marker, "started\n");
    if (mode == "normal")
        return;
    if (mode == "nonzero")
        std::exit(17);
    if (mode == "memory") {
        std::vector<std::vector<unsigned char>> blocks;
        for (int index = 0; index < 950; ++index) {
            blocks.emplace_back(1U << 20, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("sampled memory stop did not fire");
    }
    if (mode == "threads") {
        std::vector<std::thread> threads;
        for (int index = 0; index < 140; ++index) {
            threads.emplace_back([] {
                while (true)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (auto& thread : threads)
            thread.join();
    }
    if (mode == "logs") {
        const std::string bytes(8192, 'x');
        for (int index = 0; index < 12000; ++index)
            std::cout << bytes;
        return;
    }
    if (mode == "descendant") {
        const auto child = ::fork();
        require(child >= 0, "cannot create native descendant probe");
        if (child == 0) {
            require(::setsid() >= 0, "cannot detach native descendant probe");
            atomicWrite(marker.string() + ".pid", std::to_string(::getpid()));
            while (true)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return;
    }
    if (mode == "cancellation")
        require(::kill(::getppid(), SIGTERM) == 0, "cannot request native unit cancellation");
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
void inside(const fs::path& runner, const fs::path& self, const fs::path& output) {
    Json cases = Json::array();
    const auto unit = [&](const std::string& mode, bool success, int timeout = 10) {
        const auto prefix = output / mode;
        const auto marker = output / (mode + ".marker");
        const auto result = process({runner.string(), "unit", "--output", prefix.string(), "--estate", output.string(),
            "--timeout", std::to_string(timeout), "--", self.string(), "--workload", mode, marker.string()}, {}, std::chrono::seconds(timeout + 10));
        const auto receipt = parseJson(readFile(prefix.string() + ".json", 65536));
        require(receipt.at("policy") == "macos.sampled-tree.v1" && receipt.at("memory_stop_bytes") == 838860800ULL &&
            receipt.at("thread_stop_count") == 128 && receipt.at("swap_enforced") == false &&
            receipt.at("kernel_hard_memory_limit") == false && receipt.at("escape_prevention") == false,
            "native policy receipt overclaims guarantees or changes limits");
        require(fs::exists(marker) && receipt.at("sampled_descendants_removed") == true, "native workload start or cleanup was not observed");
        require((result.exit == 0) == success, "unexpected native outcome: " + mode + ": " + result.output + result.diagnostic);
        if (mode == "memory")
            require(receipt.value("error", "").find("proactive memory stop") != std::string::npos, "memory probe failed for another reason");
        if (mode == "threads")
            require(receipt.value("error", "").find("native thread stop") != std::string::npos, "thread probe failed for another reason");
        if (mode == "logs")
            require(receipt.value("error", "").find("output exceeds") != std::string::npos, "output probe failed for another reason");
        if (mode == "cancellation")
            require(receipt.at("outcome") == "cancelled", "native cancellation was misclassified");
        cases.push_back(mode);
    };
    unit("normal", true);
    unit("nonzero", false);
    unit("timeout", false, 1);
    unit("memory", false, 30);
    unit("threads", false);
    unit("logs", false);
    unit("descendant", false, 1);
    unit("cancellation", false);
    const auto old = fileHash(output / "normal.json");
    require(process({runner.string(), "unit", "--output", (output / "normal").string(), "--estate", output.string(),
        "--", self.string(), "--workload", "normal", (output / "duplicate.marker").string()}).exit != 0 &&
        fileHash(output / "normal.json") == old && !fs::exists(output / "duplicate.marker"), "native receipt was overwritten");
    cases.push_back("immutable-receipt");
    atomicWrite(output / "estate-padding", std::string(2U << 20, 'x'));
    const auto storage = process({runner.string(), "unit", "--output", (output / "estate").string(), "--estate", output.string(),
        "--estate-limit-mib", "1", "--", self.string(), "--workload", "normal", (output / "estate.marker").string()});
    require(storage.exit != 0 && !fs::exists(output / "estate.marker"), "native estate preflight launched a workload");
    cases.push_back("estate-preflight");
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"policy", "macos.sampled-tree.v1"},
        {"runner_sha256", fileHash(runner)}, {"cases", cases}, {"scope", "sampled native policy; no kernel hard memory, zero-swap or escape-prevention claim"}}));
}
}
int main(int argc, char** argv) {
    try {
        using namespace pipelang::tools;
        require(argc >= 2, "native probe requires a mode");
        const std::string mode = argv[1];
        if (mode == "--workload" && argc == 4) {
            workload(argv[2], argv[3]);
            return 0;
        }
        if (mode == "--inside" && argc == 4) {
            inside(argv[2], executablePath(), argv[3]);
            return 0;
        }
        if (mode == "--admit" && argc == 5) {
            require(fileHash(argv[3]) == argv[4], "native policy receipt hash differs");
            const auto receipt = parseJson(readFile(argv[3], 65536));
            require(receipt.at("status") == "passed" && receipt.at("policy") == "macos.sampled-tree.v1" &&
                receipt.at("runner_sha256") == fileHash(argv[2]) && receipt.at("cases").size() == 10, "native policy receipt is incomplete");
            return 0;
        }
        require(mode == "--suite" && argc == 4, "usage: native probe --suite RUNNER OUTPUT");
        const fs::path runner = fs::canonical(argv[2]);
        const fs::path output = argv[3];
        require(output.is_absolute() && !fs::exists(output), "native probe output must be fresh and absolute");
        fs::create_directories(output);
        const auto refusal = process({runner.string(), "unit", "--output", (output / "uncontained").string(),
            "--estate", output.string(), "--", executablePath().string(), "--workload", "normal", (output / "uncontained.marker").string()});
        require(refusal.exit != 0 && !fs::exists(output / "uncontained.marker"), "native runner admitted an uncontained unit");
        const auto result = process({runner.string(), "job", "--output", (output / "job").string(), "--estate", output.string(),
            "--timeout", "120", "--", executablePath().string(), "--inside", runner.string(), output.string()}, {}, std::chrono::seconds(140), 1U << 20);
        require(result.exit == 0 && fs::exists(output / "acceptance.json"), "native policy probes failed: " + result.output + result.diagnostic);
        std::cout << "macOS native policy: ten workload probes and uncontained-entry refusal passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "macOS native policy probe: " << error.what() << '\n';
        return 1;
    }
}
