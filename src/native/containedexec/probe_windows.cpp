// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "../support/support.hpp"
#include "../../../tests/native-models/windows_junction.hpp"
#include <csignal>
#include <array>
#include <atomic>
#include <exception>
#include <iostream>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
using namespace pipelang::tools;
using CleanupClock = std::chrono::steady_clock;

class ProbeHandle {
public:
    explicit ProbeHandle(HANDLE value) : value_(value) {
        require(value_ != INVALID_HANDLE_VALUE, "cannot open owned cleanup probe handle");
    }
    ProbeHandle(const ProbeHandle&) = delete;
    ProbeHandle& operator=(const ProbeHandle&) = delete;
    ~ProbeHandle() { close(); }

    bool close() noexcept {
        if (value_ == INVALID_HANDLE_VALUE)
            return true;
        if (!CloseHandle(value_))
            return false;
        value_ = INVALID_HANDLE_VALUE;
        return true;
    }
private:
    HANDLE value_;
};

bool retryableOwnedRemovalError(const std::error_code& error) {
    if (!error)
        return true;
    if (error.category() == std::system_category()) {
        switch (error.value()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_DIR_NOT_EMPTY:
        case ERROR_DELETE_PENDING:
            return true;
        default:
            return false;
        }
    }
    if (error.category() == std::generic_category()) {
        return error == std::errc::no_such_file_or_directory || error == std::errc::permission_denied ||
            error == std::errc::directory_not_empty || error == std::errc::device_or_resource_busy;
    }
    return false;
}

void removeOwnedDirectory(const fs::path& directory, CleanupClock::time_point deadline) {
    for (;;) {
        std::error_code removalError;
        fs::remove_all(directory, removalError);
        if (!retryableOwnedRemovalError(removalError))
            throw fs::filesystem_error("cannot remove owned probe directory", directory, removalError);

        const auto attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return;
            removalError = std::error_code(static_cast<int>(error), std::system_category());
            if (!retryableOwnedRemovalError(removalError))
                throw fs::filesystem_error("cannot verify owned probe directory removal", directory, removalError);
        } else if (!removalError) {
            removalError = std::error_code(ERROR_DELETE_PENDING, std::system_category());
        }

        // The read-only estate sampler may still own a share-delete handle.
        // Do not reuse the name until the last close makes absence observable.
        if (CleanupClock::now() >= deadline)
            throw fs::filesystem_error("owned probe directory removal deadline exceeded", directory, removalError);
        Sleep(1);
    }
}

void checkOwnedDirectoryCleanup(const fs::path& marker, CleanupClock::time_point deadline) {
    const fs::path sharing = marker.string() + ".sharing";
    fs::create_directories(sharing);
    ProbeHandle heldDirectory(CreateFileW(sharing.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const bool removedWhileHeld = RemoveDirectoryW(sharing.c_str()) != FALSE;
    const auto sharingError = GetLastError();
    require(!removedWhileHeld && (sharingError == ERROR_SHARING_VIOLATION || sharingError == ERROR_ACCESS_DENIED),
        "owned no-delete-share directory did not refuse removal");
    std::atomic<bool> handleClosed = false;
    std::jthread release([&] {
        Sleep(50);
        handleClosed.store(heldDirectory.close());
    });
    removeOwnedDirectory(sharing, deadline);
    require(fs::create_directory(sharing), "removed owned directory name cannot be reused");
    release.join();
    require(handleClosed.load(), "cannot close owned sharing-denial probe handle");
    removeOwnedDirectory(sharing, deadline);

    const fs::path denied = marker.string() + ".denied";
    const auto protectedFile = denied / "value";
    atomicWrite(protectedFile, "owned protected publication\n");
    ProbeHandle heldFile(CreateFileW(protectedFile.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    const auto started = CleanupClock::now();
    bool refused = false;
    try {
        removeOwnedDirectory(denied, started + std::chrono::milliseconds(100));
    } catch (const fs::filesystem_error& failure) {
        refused = retryableOwnedRemovalError(failure.code()) &&
            std::string(failure.what()).find("deadline exceeded") != std::string::npos;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(CleanupClock::now() - started).count();
    require(refused && elapsed < 2000, "persistent owned removal denial was not bounded and refused");
    require(readFile(protectedFile, 128) == "owned protected publication\n", "denied cleanup changed protected fixture bytes");
    require(heldFile.close(), "cannot close owned denial probe handle");
    removeOwnedDirectory(denied, deadline);
    atomicWrite(marker.string() + ".cleanup.json", canonical({{"sharing_denial_observed", true},
        {"initial_removal_error", sharingError}, {"permanent_denial_refused", true},
        {"denial_elapsed_ms", elapsed}, {"name_reuse_after_absence", true}}));
}
void workload(const std::string& mode, const fs::path& marker) {
    require(std::getenv("PIPELANG_WINDOWS_COORDINATOR"), "probe has no native coordinator");
    atomicWrite(marker, "started\n");
    if (mode == "normal")
        return;
    if (mode == "process-churn") {
        const auto self = executablePath().string();
        std::array<std::exception_ptr, 4> failures{};
        std::vector<std::thread> workers;
        for (std::size_t worker = 0; worker < failures.size(); ++worker) {
            workers.emplace_back([&, worker] {
                try {
                    for (int iteration = 0; iteration < 24; ++iteration) {
                        const auto result = process({self, "--churn-child"});
                        require(result.exit == 0, "short-lived native child failed");
                    }
                } catch (...) {
                    failures[worker] = std::current_exception();
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        for (const auto& failure : failures) {
            if (failure)
                std::rethrow_exception(failure);
        }
        return;
    }
    if (mode == "directory-churn") {
        const auto deadline = CleanupClock::now() + std::chrono::seconds(20);
        checkOwnedDirectoryCleanup(marker, deadline);
        const fs::path directories = marker.string() + ".directories";
        fs::create_directories(directories);
        for (int round = 0; round < 25; ++round) {
            for (int index = 0; index < 25; ++index)
                atomicWrite(directories / std::to_string(index) / "new" / "value", "owned publication\n");
            for (int index = 0; index < 25; ++index)
                removeOwnedDirectory(directories / std::to_string(index), deadline);
            Sleep(1);
        }
        removeOwnedDirectory(directories, deadline);
        atomicWrite(marker.string() + ".churn.json", canonical({{"rounds", 25}, {"entries_per_round", 25},
            {"publications", 625}, {"verified_removals", 625}, {"directory_absent", true}}));
        return;
    }
    if (mode == "reparse-accounting") {
        const fs::path target = marker.string() + ".target";
        const fs::path link = marker.string() + ".junction";
        atomicWrite(target / "protected", "owned target");
        pipelang::tests::DirectoryJunction junction(link, target);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        require(readFile(target / "protected", 128) == "owned target", "estate accounting changed a junction target");
        junction.remove();
        return;
    }
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
    if (mode == "hard-cap") {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        require(QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr) &&
            (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) && limits.JobMemoryLimit == (1ULL << 30),
            "hard-cap workload is outside its exact kernel commit policy");
        SetLastError(ERROR_SUCCESS);
        void* bytes = VirtualAlloc(nullptr, 1100ULL << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        const auto error = GetLastError();
        if (bytes)
            VirtualFree(bytes, 0, MEM_RELEASE);
        atomicWrite(marker.string() + ".allocation.json", canonical({{"allocation_refused", bytes == nullptr},
            {"requested_bytes", 1100ULL << 20}, {"kernel_limit_bytes", limits.JobMemoryLimit}, {"win32_error", error}}));
        require(bytes == nullptr, "Windows hard commit limit admitted an oversized allocation");
        return;
    }
    if (mode == "descendant") {
        const auto executable = executablePath().wstring();
        auto command = L"\"" + executable + L"\" --orphan \"" + marker.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            DETACHED_PROCESS, nullptr, nullptr, &startup, &child), "cannot create native detached probe");
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!fs::exists(marker.string() + ".pid")) {
            require(std::chrono::steady_clock::now() < deadline, "detached native probe did not start");
            Sleep(10);
        }
        return;
    }
    if (mode == "cancellation") {
        const char* name = std::getenv("PIPELANG_WINDOWS_CANCEL_EVENT");
        require(name && *name, "native cancellation event is absent");
        const HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
        require(event != nullptr, "cannot open native cancellation event");
        const bool signalled = SetEvent(event) != FALSE;
        CloseHandle(event);
        require(signalled, "cannot request native unit cancellation");
    }
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
        require(receipt.at("policy") == "windows.job-commit.v1" && receipt.at("memory_stop_bytes") == 838860800ULL &&
            receipt.at("commit_limit_bytes") == 1073741824ULL && receipt.at("sampled_thread_stop_count") == 128 &&
            receipt.at("process_limit_count") == 128 && receipt.at("swap_enforced") == false &&
            receipt.at("kernel_hard_commit_limit") == true && receipt.at("escape_prevention") == true &&
            receipt.at("estate_accounting") == "allocated-entry-bytes-without-reparse-traversal" &&
            receipt.at("filesystem_write_isolation") == false,
            "native policy receipt overclaims guarantees or changes limits");
        require(fs::exists(marker) && receipt.at("descendants_removed") == true, "native workload start or cleanup was not observed");
        require((result.exit == 0) == success, "unexpected native outcome: " + mode + ": " + result.output + result.diagnostic);
        if (mode == "memory")
            require(receipt.value("error", "").find("proactive memory stop") != std::string::npos, "memory probe failed for another reason");
        if (mode == "threads")
            require(receipt.value("error", "").find("proactive thread stop") != std::string::npos, "thread probe failed for another reason");
        if (mode == "logs")
            require(receipt.value("error", "").find("output exceeds") != std::string::npos, "output probe failed for another reason");
        if (mode == "cancellation")
            require(receipt.at("outcome") == "cancelled", "native cancellation was misclassified");
        cases.push_back(mode);
    };
    unit("normal", true);
    unit("process-churn", true, 30);
    unit("directory-churn", true, 30);
    unit("reparse-accounting", true);
    unit("nonzero", false);
    unit("timeout", false, 1);
    unit("memory", false, 30);
    unit("threads", false);
    unit("logs", false);
    unit("hard-cap", true);
    const auto refusal = parseJson(readFile(output / "hard-cap.marker.allocation.json", 65536));
    require(refusal.at("allocation_refused") == true && refusal.at("requested_bytes") == (1100ULL << 20) &&
        refusal.at("kernel_limit_bytes") == (1ULL << 30) && refusal.at("win32_error") != ERROR_SUCCESS,
        "kernel oversized-commit refusal was not independently observed");
    unit("descendant", true);
    const auto detachedPid = static_cast<DWORD>(std::stoul(readFile(output / "descendant.marker.pid", 32)));
    const HANDLE detached = OpenProcess(SYNCHRONIZE, FALSE, detachedPid);
    if (detached) {
        const auto stopped = WaitForSingleObject(detached, 0);
        CloseHandle(detached);
        require(stopped == WAIT_OBJECT_0, "Windows job left a detached descendant running");
    } else {
        require(GetLastError() == ERROR_INVALID_PARAMETER, "cannot verify detached Windows descendant cleanup");
    }
    unit("cancellation", false);
    const auto old = fileHash(output / "normal.json");
    require(process({runner.string(), "unit", "--output", (output / "normal").string(), "--estate", output.string(),
        "--", self.string(), "--workload", "normal", (output / "duplicate.marker").string()}).exit != 0 &&
        fileHash(output / "normal.json") == old && !fs::exists(output / "duplicate.marker"), "native receipt was overwritten");
    cases.push_back("immutable-receipt");
    const auto linkedEstate = output / "linked-estate";
    const auto linkedTarget = output / "linked-target";
    fs::create_directory(linkedEstate);
    atomicWrite(linkedTarget / "padding", std::string(2U << 20, 'x'));
    pipelang::tests::DirectoryJunction junction(linkedEstate / "junction", linkedTarget);
    const auto linkedPrefix = output / "linked-allocation";
    const auto linkedMarker = output / "linked-allocation.marker";
    const auto linked = process({runner.string(), "unit", "--output", linkedPrefix.string(), "--estate", linkedEstate.string(),
        "--estate-limit-mib", "1", "--", self.string(), "--workload", "normal", linkedMarker.string()});
    const auto linkedReceipt = parseJson(readFile(linkedPrefix.string() + ".json", 65536));
    require(linked.exit == 0 && fs::exists(linkedMarker) && linkedReceipt.at("allocated_bytes") < (1U << 20),
        "estate accounting followed a reparse target");
    cases.push_back("estate-reparse-target-not-followed");
    const auto redirected = process({runner.string(), "unit", "--output", (output / "redirected-root").string(),
        "--estate", (linkedEstate / "junction").string(), "--", self.string(), "--workload", "normal",
        (output / "redirected-root.marker").string()});
    require(redirected.exit != 0 && !fs::exists(output / "redirected-root.marker"), "native estate accepted a redirected root");
    cases.push_back("estate-reparse-root-refused");
    junction.remove();
    atomicWrite(output / "estate-padding", std::string(2U << 20, 'x'));
    const auto storage = process({runner.string(), "unit", "--output", (output / "estate").string(), "--estate", output.string(),
        "--estate-limit-mib", "1", "--", self.string(), "--workload", "normal", (output / "estate.marker").string()});
    require(storage.exit != 0 && !fs::exists(output / "estate.marker"), "native estate preflight launched a workload");
    cases.push_back("estate-preflight");
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"policy", "windows.job-commit.v1"},
        {"runner_sha256", fileHash(runner)}, {"cases", cases}, {"scope", "native Job Object hard commit and active-process caps; sampled thread stop; no zero-swap claim"}}));
}
}
int main(int argc, char** argv) {
    try {
        using namespace pipelang::tools;
        require(argc >= 2, "native probe requires a mode");
        const std::string mode = argv[1];
        if (mode == "--churn-child")
            return 0;
        if (mode == "--orphan" && argc == 3) {
            atomicWrite(std::string(argv[2]) + ".pid", std::to_string(GetCurrentProcessId()));
            while (true)
                Sleep(1000);
        }
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
            require(receipt.at("status") == "passed" && receipt.at("policy") == "windows.job-commit.v1" &&
                receipt.at("runner_sha256") == fileHash(argv[2]) && receipt.at("cases").size() == 16, "native policy receipt is incomplete");
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
        std::cout << "Windows native policy: sixteen workload probes and uncontained-entry refusal passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows native policy probe: " << error.what() << '\n';
        return 1;
    }
}
