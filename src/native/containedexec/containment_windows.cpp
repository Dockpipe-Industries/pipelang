// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "containment.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <memory>

namespace pipelang::contained {
namespace {
class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};
class Environment {
public:
    Environment(const std::string& name, const std::string& value) : name_(name) {
        const char* previous = std::getenv(name.c_str());
        if (previous)
            previous_ = previous;
        require(_putenv_s(name.c_str(), value.c_str()) == 0, "cannot establish Windows coordinator context");
    }
    ~Environment() {
        _putenv_s(name_.c_str(), previous_ ? previous_->c_str() : "");
    }
private:
    std::string name_;
    std::optional<std::string> previous_;
};
std::uint64_t creation(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(process, &created, &exited, &kernel, &user), "cannot identify Windows coordinator start");
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
std::wstring selected(const char* name) {
    const char* value = std::getenv(name);
    require(value && *value, "Windows workload requires coordinator context");
    const std::string text(value);
    require(text.size() < 200 && text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.\\") == std::string::npos,
        "invalid Windows coordinator object name");
    return std::wstring(text.begin(), text.end());
}
std::uint64_t allocated(const fs::path& estate, std::uint64_t limit) {
    std::uint64_t total = 0;
    std::size_t entries = 0;
    const auto charge = [&](LONGLONG allocation) {
        require(allocation >= 0, "invalid proof allocation size");
        const auto size = static_cast<std::uint64_t>(allocation);
        require(size < limit - total, "proof estate reached its byte limit");
        total += size;
    };
    std::vector<fs::path> pending{estate};
    while (!pending.empty()) {
        const auto directory = pending.back();
        pending.pop_back();
        Handle handle(CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (directory != estate && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_DELETE_PENDING))
                continue;
            throw std::runtime_error("cannot enumerate proof estate (Win32 " + std::to_string(error) + "): " + directory.string());
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        require(GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)),
            "cannot inspect proof directory attributes");
        require(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY, "proof estate contains a non-directory");
        if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            require(directory != estate, "proof estate root is a reparse point");
            FILE_STANDARD_INFO information{};
            require(GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &information, sizeof(information)),
                "cannot inspect reparse entry allocation");
            charge(information.AllocationSize.QuadPart);
            continue;
        }

        // Directory records include allocation sizes without opening linker
        // temporary files, which can enter delete-pending state during a scan.
        alignas(8) std::array<unsigned char, 65536> buffer{};
        for (;;) {
            if (!GetFileInformationByHandleEx(handle.get(), FileIdBothDirectoryInfo, buffer.data(),
                static_cast<DWORD>(buffer.size()))) {
                const auto error = GetLastError();
                if (error == ERROR_NO_MORE_FILES)
                    break;
                throw std::runtime_error("cannot read proof directory (Win32 " + std::to_string(error) + "): " + directory.string());
            }
            std::size_t offset = 0;
            for (;;) {
                constexpr auto headerSize = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                require(offset <= buffer.size() - headerSize, "invalid proof directory record");
                const auto* information = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
                require(information->FileNameLength % sizeof(wchar_t) == 0 &&
                    information->FileNameLength <= buffer.size() - offset - headerSize, "invalid proof directory name");
                const std::wstring name(information->FileName, information->FileNameLength / sizeof(wchar_t));
                if (name != L"." && name != L"..") {
                    require(++entries <= 1000000, "proof estate entry limit exceeded");
                    if ((information->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                        !(information->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        pending.push_back(directory / name);
                    } else {
                        // Account for the entry itself without following reparse targets.
                        charge(information->AllocationSize.QuadPart);
                    }
                }
                const auto next = information->NextEntryOffset;
                if (next == 0)
                    break;
                require(next % 8 == 0 && next >= headerSize + information->FileNameLength &&
                    next <= buffer.size() - offset - headerSize, "invalid proof directory offset");
                offset += next;
            }
        }
    }
    return total;
}
volatile LONG cancelled = 0;
BOOL WINAPI cancel(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT)
        return FALSE;
    InterlockedExchange(&cancelled, 1);
    return TRUE;
}
}
fs::path currentGroup() {
    throw std::runtime_error("Windows policy uses kernel Job Objects rather than cgroups");
}
fs::path verifyJob() {
    const auto name = selected("PIPELANG_WINDOWS_JOB");
    Handle job(OpenJobObjectW(JOB_OBJECT_QUERY, FALSE, name.c_str()));
    require(job.get() != nullptr, "cannot open Windows coordinator job");
    BOOL member = FALSE;
    require(IsProcessInJob(GetCurrentProcess(), job.get(), &member) && member, "native workload is outside its Windows job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    require(QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr),
        "cannot inspect Windows coordinator limits");
    const auto required = JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require((limits.BasicLimitInformation.LimitFlags & required) == required && limits.JobMemoryLimit == 2ULL * 1024 * mebibyte &&
        limits.BasicLimitInformation.ActiveProcessLimit == 384, "Windows coordinator kernel limits differ");
    const char* coordinator = std::getenv("PIPELANG_WINDOWS_COORDINATOR");
    const char* admittedIdentity = std::getenv("PIPELANG_WINDOWS_COORDINATOR_IDENTITY");
    require(coordinator && admittedIdentity, "Windows coordinator identity is absent");
    std::size_t consumed = 0;
    const auto expectedValue = std::stoull(coordinator, &consumed);
    require(consumed == std::string(coordinator).size() && expectedValue > 0 && expectedValue <= MAXDWORD,
        "invalid Windows coordinator PID");
    const auto expected = static_cast<DWORD>(expectedValue);
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    require(snapshot.get() != INVALID_HANDLE_VALUE, "cannot inspect Windows coordinator ancestry");
    std::map<DWORD, DWORD> parents;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    require(Process32FirstW(snapshot.get(), &entry), "cannot read Windows process inventory");
    do {
        parents.emplace(entry.th32ProcessID, entry.th32ParentProcessID);
    } while (Process32NextW(snapshot.get(), &entry));
    require(GetLastError() == ERROR_NO_MORE_FILES, "cannot finish Windows process inventory");
    auto current = GetCurrentProcessId();
    for (int depth = 0; depth < 128 && current != 0; ++depth) {
        if (current == expected) {
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, current));
            require(process.get() != nullptr && std::to_string(creation(process.get())) == admittedIdentity,
                "Windows coordinator process identity differs");
            std::array<wchar_t, 32768> path{};
            DWORD capacity = static_cast<DWORD>(path.size());
            require(QueryFullProcessImageNameW(process.get(), 0, path.data(), &capacity) &&
                fs::equivalent(fs::path(std::wstring(path.data(), capacity)), executablePath()), "Windows coordinator executable differs");
            return fs::path(name);
        }
        const auto parent = parents.find(current);
        require(parent != parents.end() && parent->second != current, "Windows coordinator ancestry is incomplete");
        current = parent->second;
    }
    throw std::runtime_error("native workload is outside its Windows coordinator ancestry");
}
void verifyInside(bool) {
    verifyJob();
}
int replace(const Request&) {
    throw std::runtime_error("Linux inside modes are unavailable under Windows Job Object policy");
}
int execute(const Request& request, const fs::path& executable) {
    require(request.output.is_absolute() && request.estate.is_absolute() && fs::is_directory(request.estate),
        "native proof and estate must be absolute existing directories");
    require(fs::weakly_canonical(request.estate) == request.estate.lexically_normal(), "proof estate has symbolic ancestors");
    require(request.estateLimit > 0 && request.estateLimit <= 8ULL * 1024 * mebibyte, "estate limit cannot exceed 8 GiB");
    const fs::path receipt = request.output.string() + ".json";
    require(!fs::exists(fs::symlink_status(receipt)), "use a fresh native receipt");
    if (!request.job)
        verifyInside(false);
    fs::create_directories(request.output.parent_path());
    WindowsJobLimits policy;
    policy.memoryLimit = (request.job ? 2048ULL : 1024ULL) * mebibyte;
    policy.memoryStop = (request.job ? 1800ULL : 800ULL) * mebibyte;
    policy.processLimit = request.job ? 384 : 128;
    policy.threadLimit = request.job ? 384 : 128;
    const auto identity = std::to_string(GetCurrentProcessId()) + "-" + uniqueName();
    if (request.job)
        policy.name = "Local\\PipeLangJob-" + identity;
    const std::string cancelName = "Local\\PipeLangCancel-" + identity;
    Handle event(CreateEventA(nullptr, TRUE, FALSE, cancelName.c_str()));
    require(event.get() != nullptr && GetLastError() != ERROR_ALREADY_EXISTS, "cannot create native cancellation event");
    std::vector<std::unique_ptr<Environment>> environment;
    if (request.job) {
        environment.push_back(std::make_unique<Environment>("PIPELANG_WINDOWS_JOB", policy.name));
        environment.push_back(std::make_unique<Environment>("PIPELANG_WINDOWS_COORDINATOR", std::to_string(GetCurrentProcessId())));
        environment.push_back(std::make_unique<Environment>("PIPELANG_WINDOWS_COORDINATOR_IDENTITY", std::to_string(creation(GetCurrentProcess()))));
    }
    environment.push_back(std::make_unique<Environment>("PIPELANG_WINDOWS_CANCEL_EVENT", cancelName));
    Json report{{"protocol", "pipelang.contained-execution.v1"}, {"policy", "windows.job-commit.v1"},
        {"command", request.command}, {"runner_sha256", fileHash(executable)},
        {"command_sha256", sha256(Json(request.command).dump())}, {"outcome", "starting"}, {"exit", nullptr},
        {"kernel_hard_commit_limit", true}, {"swap_enforced", false}, {"escape_prevention", true},
        {"descendants_removed", false}, {"estate_limit_bytes", request.estateLimit},
        {"estate_accounting", "allocated-entry-bytes-without-reparse-traversal"}, {"filesystem_write_isolation", false},
        {"commit_limit_bytes", policy.memoryLimit}, {"memory_stop_bytes", policy.memoryStop},
        {"memory_stop_metric", "sampled-process-private-commit"},
        {"process_limit_count", policy.processLimit}, {"sampled_thread_stop_count", policy.threadLimit}};
    atomicWrite(receipt, canonical(report));
    const auto start = std::chrono::steady_clock::now();
    auto lastEstate = start - std::chrono::seconds(1);
    std::uint64_t bytes = 0;
    std::uint64_t peak = 0;
    std::uint64_t kernelPeak = 0;
    InterlockedExchange(&cancelled, 0);
    const bool consoleHandler = SetConsoleCtrlHandler(cancel, TRUE) != FALSE;
    try {
        bytes = allocated(request.estate, request.estateLimit);
        const auto monitor = [&] {
            require(!InterlockedCompareExchange(&cancelled, 0, 0) && WaitForSingleObject(event.get(), 0) == WAIT_TIMEOUT,
                "native workload cancelled");
            const auto now = std::chrono::steady_clock::now();
            if (now - lastEstate >= std::chrono::milliseconds(200)) {
                bytes = allocated(request.estate, request.estateLimit);
                lastEstate = now;
            }
        };
        const auto result = windowsProcess(request.command, {}, request.timeout, 64U << 20, 1U << 20,
            monitor, request.output.string() + ".output", policy, [&](std::uint64_t memory, std::uint64_t reportedPeak) {
                peak = std::max(peak, memory);
                kernelPeak = std::max(kernelPeak, reportedPeak);
            }, [&] { report["descendants_removed"] = true; });
        report["descendants_removed"] = true;
        report["exit"] = result.exit;
        report["outcome"] = result.exit == 0 ? "completed" : "failed";
        if (result.exit != 0) {
            std::cerr << result.diagnostic.substr(result.diagnostic.size() > 8192 ? result.diagnostic.size() - 8192 : 0);
            std::cerr << result.output.substr(result.output.size() > 8192 ? result.output.size() - 8192 : 0);
        }
    } catch (const std::exception& error) {
        report["error"] = error.what();
        std::cerr << "native containment: " << std::string_view(error.what()).substr(0, 8192) << '\n';
        report["outcome"] = InterlockedCompareExchange(&cancelled, 0, 0) || WaitForSingleObject(event.get(), 0) == WAIT_OBJECT_0 ? "cancelled" : "failed";
        // Cleanup is reported only after the transport verifies zero active job members.
    }
    if (consoleHandler)
        SetConsoleCtrlHandler(cancel, FALSE);
    try {
        bytes = allocated(request.estate, request.estateLimit);
    } catch (const std::exception& error) {
        report["outcome"] = "failed";
        report["estate_error"] = error.what();
        std::cerr << "native containment estate: " << std::string_view(error.what()).substr(0, 8192) << '\n';
    }
    report["sampled_peak_private_commit_bytes"] = peak;
    report["kernel_reported_peak_bytes"] = kernelPeak;
    report["allocated_bytes"] = bytes;
    report["elapsed_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    atomicWrite(receipt, canonical(report));
    const bool success = report.at("outcome") == "completed" && report.at("descendants_removed") == true;
    return success ? 0 : 1;
}
}
