// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "estate.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/stat.h>

namespace pipelang::contained {
namespace {
using namespace pipelang::tools;
constexpr std::uint64_t testLimit = 64ULL * 1024 * 1024;

void writeAllocatedFile(const fs::path& path, std::size_t blocks) {
    std::ofstream stream(path, std::ios::binary);
    require(bool(stream), "cannot create estate fixture");
    const std::string block(4096, 'x');
    for (std::size_t index = 0; index < blocks; ++index)
        stream.write(block.data(), static_cast<std::streamsize>(block.size()));
    stream.close();
    require(bool(stream), "cannot finish estate fixture");
}

std::uint64_t fileAllocation(const fs::path& path) {
    struct stat status{};
    require(::lstat(path.c_str(), &status) == 0, "cannot stat estate fixture");
    return static_cast<std::uint64_t>(status.st_blocks) * 512;
}

template<class Action>
void refuses(Action action, const std::string& detail) {
    bool refused = false;
    try {
        action();
    } catch (const std::exception& error) {
        refused = std::string(error.what()).find(detail) != std::string::npos;
    }
    require(refused, "estate probe did not refuse with expected diagnostic: " + detail);
}

void publish(const fs::path& root, int round) {
    const auto staging = root / (".staging-" + std::to_string(round));
    const auto published = root / ("published-" + std::to_string(round));
    for (int index = 0; index < 16; ++index) {
        const auto child = staging / std::to_string(index) / "nested";
        fs::create_directories(child);
        writeAllocatedFile(child / "payload", 1);
    }
    fs::rename(staging, published);
    fs::remove_all(published);
}
}

void probeEstate(const fs::path& output) {
    fs::create_directories(output);
    const auto estate = output / "estate";
    fs::create_directory(estate);
    require(allocatedEstate(estate, testLimit) == 0, "empty estate allocation changed");
    const auto retained = estate / "retained";
    writeAllocatedFile(retained, 512);
    const auto allocation = fileAllocation(retained);
    require(allocation >= 2ULL * 1024 * 1024, "dense file fixture is not allocated");
    require(allocatedEstate(estate, testLimit) == allocation, "estate did not count allocated blocks");
    refuses([&] { allocatedEstate(estate, allocation); }, "byte limit");

    const auto outside = output / "outside";
    fs::create_directory(outside);
    writeAllocatedFile(outside / "large", 1024);
    fs::create_directory_symlink(outside, estate / "directory-link");
    fs::create_symlink(outside / "large", estate / "file-link");
    fs::create_symlink(outside / "absent", estate / "dangling-link");
    require(allocatedEstate(estate, allocation + 1) == allocation,
        "estate followed a symlink or counted its target");
    const auto redirected = output / "redirected-estate";
    fs::create_directory_symlink(estate, redirected);
    refuses([&] { allocatedEstate(redirected, testLimit); }, "symlink redirection");

    const auto denied = estate / "denied";
    fs::create_directory(denied);
    writeAllocatedFile(denied / "hidden", 1);
    fs::permissions(denied, fs::perms::none);
    try {
        refuses([&] { allocatedEstate(estate, testLimit); }, "Permission denied");
    } catch (...) {
        fs::permissions(denied, fs::perms::owner_all);
        throw;
    }
    fs::permissions(denied, fs::perms::owner_all);
    fs::remove_all(denied);
    refuses([&] { allocatedEstate(output / "always-absent", testLimit); }, "remained unstable after 8");

    // Real rename/remove churn exercises iterator increment, not just lstat.
    // A continuously unscannable tree may fail closed; a successful sample must
    // still include the stable dense file. Quiescent completion is mandatory.
    const auto churn = estate / "churn";
    fs::create_directory(churn);
    std::atomic<bool> ready = false;
    std::atomic<bool> finished = false;
    std::exception_ptr writerFailure;
    std::jthread writer([&] {
        try {
            ready.store(true);
            for (int round = 0; round < 128; ++round)
                publish(churn, round);
        } catch (...) {
            writerFailure = std::current_exception();
        }
        finished.store(true);
    });
    while (!ready.load())
        std::this_thread::yield();
    std::size_t completeSamples = 0;
    std::size_t unstableRefusals = 0;
    do {
        try {
            require(allocatedEstate(estate, testLimit) >= allocation,
                "changing estate omitted its stable allocation");
            ++completeSamples;
        } catch (const std::exception& error) {
            require(std::string(error.what()).find("remained unstable after 8") != std::string::npos,
                "churn did not rescan/fail closed: " + std::string(error.what()));
            ++unstableRefusals;
        }
    } while (!finished.load());
    writer.join();
    if (writerFailure)
        std::rethrow_exception(writerFailure);
    require(allocatedEstate(estate, testLimit) == allocation, "quiescent churn estate was not fully accounted");
    require(completeSamples > 0, "finite churn never yielded a complete scan");
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"},
        {"dense_allocated_bytes", allocation}, {"complete_churn_samples", completeSamples},
        {"fail_closed_churn_samples", unstableRefusals}, {"missing_root_retry_exhaustion", true},
        {"permission_errors_refused", true}, {"symlink_targets_not_followed", true},
        {"redirected_root_refused", true}, {"byte_limit_preserved", true}}));
}

void churnEstate(const fs::path& root) {
    fs::create_directories(root);
    for (int round = 0; round < 96; ++round) {
        publish(root, round);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
}
