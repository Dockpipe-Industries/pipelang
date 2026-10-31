// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "support.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <memory>
#include <cstdint>
#include <stdexcept>

namespace pipelang::tools {
namespace {
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    HANDLE* address() { return &value_; }
    HANDLE release() { return std::exchange(value_, nullptr); }
    void close() {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(std::exchange(value_, nullptr));
    }
private:
    HANDLE value_;
};
std::wstring wide(std::string_view value) {
    require(value.find('\0') == std::string_view::npos, "native argument contains NUL");
    require(value.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()), "native argument exceeds UTF-8 capacity");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    require(count > 0 || value.empty(), "native argument is not UTF-8");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count) == count,
        "cannot convert native argument");
    return result;
}
std::wstring quoted(const std::string& value) {
    const auto argument = wide(value);
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
class Attributes {
public:
    explicit Attributes(const std::array<HANDLE, 3>& handles) {
        SIZE_T capacity = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &capacity);
        require(capacity > 0, "cannot size native inherited handles");
        storage_.resize(capacity);
        value_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        require(InitializeProcThreadAttributeList(value_, 1, 0, &capacity), "cannot initialize native inherited handles");
        if (!UpdateProcThreadAttribute(value_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                const_cast<HANDLE*>(handles.data()), sizeof(HANDLE) * handles.size(), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(value_);
            value_ = nullptr;
            throw std::runtime_error("cannot select native inherited handles");
        }
    }
    ~Attributes() {
        if (value_)
            DeleteProcThreadAttributeList(value_);
    }
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return value_; }
private:
    std::vector<unsigned char> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST value_ = nullptr;
};
struct InputWriter {
    HANDLE pipe;
    std::string bytes;
    std::shared_ptr<std::atomic<bool>> failed;

    static DWORD WINAPI run(void* context) {
        std::unique_ptr<InputWriter> owner(static_cast<InputWriter*>(context));
        auto& writer = *owner;
        std::size_t written = 0;
        while (written < writer.bytes.size()) {
            DWORD count = 0;
            const auto capacity = static_cast<DWORD>(std::min<std::size_t>(writer.bytes.size() - written, 65536));
            if (!WriteFile(writer.pipe, writer.bytes.data() + written, capacity, &count, nullptr)) {
                const auto error = GetLastError();
                *writer.failed = error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA && error != ERROR_OPERATION_ABORTED;
                break;
            }
            if (count == 0) {
                *writer.failed = true;
                break;
            }
            written += count;
        }
        CloseHandle(writer.pipe);
        return 0;
    }
};

struct JobUsage {
    std::uint64_t privateCommit = 0;
    std::uint64_t threads = 0;
};
JobUsage jobUsage(HANDLE job, DWORD maximumProcesses) {
    // Toolhelp's system snapshot is filtered to the kernel-reported job members.
    const auto capacity = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + sizeof(ULONG_PTR) * maximumProcesses;
    std::vector<unsigned char> storage(capacity);
    auto* members = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
    require(QueryInformationJobObject(job, JobObjectBasicProcessIdList, members, static_cast<DWORD>(capacity), nullptr),
        "cannot inspect native job members");
    require(members->NumberOfProcessIdsInList == members->NumberOfAssignedProcesses,
        "native job member inventory exceeds its capacity");
    std::set<DWORD> identifiers;
    JobUsage usage;
    for (DWORD index = 0; index < members->NumberOfProcessIdsInList; ++index) {
        const auto identifier = static_cast<DWORD>(members->ProcessIdList[index]);
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, identifier));
        if (!process.get()) {
            require(GetLastError() == ERROR_INVALID_PARAMETER, "cannot inspect live native job memory");
            continue;
        }
        BOOL member = FALSE;
        require(IsProcessInJob(process.get(), job, &member), "cannot verify native memory ownership");
        if (!member)
            continue;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        const bool observed = GetProcessMemoryInfo(process.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
        if (!observed && WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
            continue;
        require(observed, "cannot account live native private commit");
        usage.privateCommit += memory.PrivateUsage;
        identifiers.insert(identifier);
    }
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    require(snapshot.get() != INVALID_HANDLE_VALUE, "cannot inspect native job threads");
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot.get(), &entry)) {
        do {
            if (identifiers.contains(entry.th32OwnerProcessID))
                ++usage.threads;
        } while (Thread32Next(snapshot.get(), &entry));
        require(GetLastError() == ERROR_NO_MORE_FILES, "cannot finish native thread inventory");
    } else {
        require(GetLastError() == ERROR_NO_MORE_FILES, "cannot read native thread inventory");
    }
    return usage;
}

void pipe(Handle& read, Handle& write, bool inheritRead) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    require(CreatePipe(read.address(), write.address(), &security, 65536), "cannot create native process pipe");
    require(SetHandleInformation(inheritRead ? write.get() : read.get(), HANDLE_FLAG_INHERIT, 0), "cannot protect native process pipe");
}
}
ProcessResult windowsProcess(const std::vector<std::string>& arguments, std::string_view input,
    std::chrono::milliseconds timeout, std::size_t maximumOutput, std::size_t maximumDiagnostic,
    const std::function<void()>& monitor, const fs::path& log, const WindowsJobLimits& policy,
    const std::function<void(std::uint64_t, std::uint64_t)>& account, const std::function<void()>& cleaned) {
    require(!arguments.empty(), "native process requires an absolute executable");
    const auto executable = wide(arguments.front());
    require(fs::path(executable).is_absolute(), "native process requires an absolute executable");
    require(timeout.count() > 0 && policy.memoryLimit <= 2ULL * 1024 * 1024 * 1024 &&
        policy.memoryStop > 0 && policy.memoryStop < policy.memoryLimit && policy.processLimit > 0 && policy.processLimit <= 384 &&
        policy.threadLimit > 0 && policy.threadLimit <= 384,
        "invalid native Windows process policy");
    Handle sourceRead;
    Handle sourceWrite;
    Handle outputRead;
    Handle outputWrite;
    Handle diagnosticRead;
    Handle diagnosticWrite;
    pipe(sourceRead, sourceWrite, true);
    pipe(outputRead, outputWrite, false);
    pipe(diagnosticRead, diagnosticWrite, false);
    const auto name = wide(policy.name);
    SetLastError(ERROR_SUCCESS);
    Handle job(CreateJobObjectW(nullptr, policy.name.empty() ? nullptr : name.c_str()));
    const auto jobError = GetLastError();
    require(job.get() != nullptr && jobError != ERROR_ALREADY_EXISTS, "cannot create exclusive native process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = policy.processLimit;
    limits.JobMemoryLimit = policy.memoryLimit;
    require(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "cannot set native job limits");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION observed{};
    require(QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &observed, sizeof(observed), nullptr) &&
        observed.JobMemoryLimit == limits.JobMemoryLimit &&
        observed.BasicLimitInformation.ActiveProcessLimit == limits.BasicLimitInformation.ActiveProcessLimit &&
        observed.BasicLimitInformation.LimitFlags == limits.BasicLimitInformation.LimitFlags, "native job limit readback differs");
    const std::array<HANDLE, 3> handles{sourceRead.get(), outputWrite.get(), diagnosticWrite.get()};
    Attributes attributes(handles);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = sourceRead.get();
    startup.StartupInfo.hStdOutput = outputWrite.get();
    startup.StartupInfo.hStdError = diagnosticWrite.get();
    startup.lpAttributeList = attributes.get();
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty())
            command += L' ';
        command += quoted(argument);
    }
    require(command.size() < 32767, "native command line exceeds Windows capacity");
    PROCESS_INFORMATION child{};
    const bool created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
        &startup.StartupInfo, &child) != FALSE;
    const auto createError = GetLastError();
    require(created, "cannot create native process: " + std::to_string(createError));
    Handle processHandle(child.hProcess);
    Handle threadHandle(child.hThread);
    Handle writer;
    std::shared_ptr<std::atomic<bool>> inputFailed;
    ProcessResult result;
    std::uint64_t sampledPeak = 0;
    std::uint64_t kernelPeak = 0;
    const auto cleanup = [&] {
        require(TerminateJobObject(job.get(), 1), "cannot stop native process job");
        // Assignment itself can fail while the root remains suspended.
        if (WaitForSingleObject(processHandle.get(), 0) == WAIT_TIMEOUT)
            TerminateProcess(processHandle.get(), 1);
        if (writer.get()) {
            CancelSynchronousIo(writer.get());
            require(WaitForSingleObject(writer.get(), 5000) == WAIT_OBJECT_0, "cannot finish native input writer");
        }
        require(WaitForSingleObject(processHandle.get(), 5000) == WAIT_OBJECT_0, "native root did not stop");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (true) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            require(QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr),
                "cannot verify native job cleanup");
            if (accounting.ActiveProcesses == 0)
                break;
            require(std::chrono::steady_clock::now() < deadline, "native job descendants did not stop");
            Sleep(10);
        }
        if (cleaned)
            cleaned();
    };
    try {
        require(AssignProcessToJobObject(job.get(), processHandle.get()), "cannot admit native process into job");
        inputFailed = std::make_shared<std::atomic<bool>>(false);
        sourceRead.close();
        outputWrite.close();
        diagnosticWrite.close();
        require(ResumeThread(threadHandle.get()) != static_cast<DWORD>(-1), "cannot resume admitted native process");
        // Only the writer owns the input pipe once its thread has started.
        auto inputWriter = std::make_unique<InputWriter>(InputWriter{sourceWrite.get(), std::string(input), inputFailed});
        const auto writerThread = CreateThread(nullptr, 0, InputWriter::run, inputWriter.get(), 0, nullptr);
        require(writerThread != nullptr, "cannot start native input writer");
        *writer.address() = writerThread;
        inputWriter.release();
        sourceWrite.release();
        std::ofstream stream;
        if (!log.empty()) {
            stream.open(log, std::ios::binary | std::ios::trunc);
            require(stream.good(), "cannot open native process log");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        bool exited = false;
        bool outputOpen = true;
        bool diagnosticOpen = true;
        while (!exited || outputOpen || diagnosticOpen) {
            if (monitor)
                monitor();
            require(std::chrono::steady_clock::now() < deadline, "native process or protocol stream timed out");
            require(!*inputFailed, "cannot write native request");
            require(QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &observed, sizeof(observed), nullptr),
                "cannot inspect native job memory");
            kernelPeak = std::max<std::uint64_t>(kernelPeak, observed.PeakJobMemoryUsed);
            const auto usage = jobUsage(job.get(), policy.processLimit);
            sampledPeak = std::max(sampledPeak, usage.privateCommit);
            if (account)
                account(sampledPeak, kernelPeak);
            // PeakJobMemoryUsed can include an allocation attempt that the
            // kernel refused. Current private commit owns the sampled stop.
            require(usage.privateCommit < policy.memoryStop, "proactive memory stop");
            require(usage.threads <= policy.threadLimit, "proactive thread stop");
            const auto receive = [&](HANDLE handle, std::string& bytes, std::size_t maximum, bool& open) {
                if (!open)
                    return;
                // Drain a bounded batch so output cannot starve the deadline
                // monitor, and large writes do not stall behind one 8 KiB read.
                std::size_t received = 0;
                while (received < (1U << 20)) {
                    DWORD available = 0;
                    if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
                        require(GetLastError() == ERROR_BROKEN_PIPE, "cannot inspect native protocol stream");
                        open = false;
                        return;
                    }
                    if (available == 0)
                        return;
                    std::array<char, 8192> buffer{};
                    DWORD count = 0;
                    require(ReadFile(handle, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr) && count > 0,
                        "cannot read native protocol stream");
                    require(count <= maximum - bytes.size(), "native process output exceeds its capacity");
                    bytes.append(buffer.data(), count);
                    received += count;
                    if (stream.is_open()) {
                        stream.write(buffer.data(), count);
                        stream.flush();
                        require(stream.good(), "cannot write native process log");
                    }
                }
            };
            receive(outputRead.get(), result.output, maximumOutput, outputOpen);
            receive(diagnosticRead.get(), result.diagnostic, maximumDiagnostic, diagnosticOpen);
            const auto waited = WaitForSingleObject(processHandle.get(), 10);
            require(waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT, "cannot observe native process");
            exited = waited == WAIT_OBJECT_0;
        }
        require(QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &observed, sizeof(observed), nullptr),
            "cannot inspect final native job commit");
        kernelPeak = std::max<std::uint64_t>(kernelPeak, observed.PeakJobMemoryUsed);
        if (account)
            account(sampledPeak, kernelPeak);
        DWORD exitCode = 0;
        require(GetExitCodeProcess(processHandle.get(), &exitCode), "cannot read native exit status");
        result.exit = static_cast<int>(exitCode);
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    return result;
}
}
#endif
