// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "native_app_execution.hpp"
#include "NativeSourceTrace.hpp"

#include <pipelang/model/extended_collections.hpp>
#include <pipelang/model/native_host_value.hpp>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

using namespace pipelang::model;
using namespace pipelang::native_app_v19;
using Completion = Result<void>;

enum class StartMode {
    Normal, Cancel, RequireSuspension, RequireTwoSuspensions, CancelAfterSuspension, CancelAfterSecond,
    CancelBodyAfterSuspension, CancelCleanupAfterSecond
};

bool takesProcessArguments(const AsyncFunction& entry)
{
    return (entry.cancellationArguments & processArgumentsCapability) != 0;
}

bool takesIntegerArgument(const AsyncFunction& entry)
{
    return entry.argumentCount != 0 && !takesProcessArguments(entry) && (cancellationArgumentMask(entry) & 1) == 0;
}

template<class ReadArgument>
HostedValue collectProcessArguments(int argc, const ReadArgument& readArgument)
{
    const auto count = argc > 0 ? static_cast<std::size_t>(argc - 1) : 0;
    if (count > maximumListElements)
        throw ExecutionFailure("native process arguments exceed the collection element limit");

    std::vector<HostedValue> values;
    values.reserve(count);
    std::size_t bytes = sizeof(Array<std::string>);
    for (int index = 1; index < argc; ++index) {
        auto text = readArgument(index);
        validateText(text);
        bytes = boundedOwnedSum(bytes, ownedBytes(text), maximumListBytes);
        if (bytes > maximumListBytes)
            throw ExecutionFailure("native process arguments exceed the collection byte limit");
        values.push_back(HostedValue::fromText(std::move(text)));
    }
    return HostedValue::fromList("Array<string>", std::move(values));
}

#ifdef _WIN32
std::string processArgumentText(std::wstring_view argument)
{
    if (argument.empty())
        return {};

    const auto length = static_cast<int>(argument.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument.data(), length, nullptr, 0,
                                              nullptr, nullptr);
    if (required == 0)
        throw ExecutionFailure("native process argument is not valid Unicode");

    std::string text(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument.data(), length, text.data(), required,
                            nullptr, nullptr) != required)
        throw ExecutionFailure("native process argument could not be converted to UTF-8");
    return text;
}
#endif

HostedValue processArguments(int argc, char** argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> wideArguments(
        CommandLineToArgvW(GetCommandLineW(), &count), &LocalFree);
    if (!wideArguments)
        throw ExecutionFailure("native process could not read its Unicode command line");
    return collectProcessArguments(count, [&](int index) {
        return processArgumentText(wideArguments.get()[index]);
    });
#else
    return collectProcessArguments(argc, [&](int index) {
        return std::string(argv[index]);
    });
#endif
}

Completion run(std::int64_t input, const HostedValue* commandLine, StartMode mode,
               std::int64_t* output, SourceTrace* trace)
{
    const auto& entry = pipelang_native_v19_entry;
    const bool takesInteger = takesIntegerArgument(entry);
    const bool takesArguments = takesProcessArguments(entry);
    const std::size_t firstToken = takesInteger || takesArguments ? 1 : 0;
    if (entry.argumentCount < firstToken || entry.argumentCount > firstToken + 2)
        throw ExecutionFailure("native launcher received an invalid argument count");

    const auto tokenCount = entry.argumentCount - firstToken;
    const auto expectedTokens = ((std::uint64_t{1} << tokenCount) - 1) << firstToken;
    if (entry.resultWords != resultHeaderWords + (output ? 1 : 0) ||
        cancellationArgumentMask(entry) != expectedTokens || takesArguments != (commandLine != nullptr))
        throw ExecutionFailure("native launcher received an invalid entry signature");

    std::optional<CancellationSource> body;
    std::optional<CancellationSource> cleanup;
    CancellationToken bodyToken;
    CancellationToken cleanupToken;
    if (tokenCount >= 1) {
        body.emplace();
        bodyToken = body->token();
    }
    if (tokenCount == 2) {
        cleanup.emplace();
        cleanupToken = cleanup->token();
    }
    if ((mode == StartMode::CancelBodyAfterSuspension && !body) ||
        (mode == StartMode::CancelCleanupAfterSecond && !cleanup))
        throw ExecutionFailure("native launcher entry has no corresponding explicit cancellation token");

    const CancellationSource caller;
    if (mode == StartMode::Cancel)
        caller.cancel();

    std::array<Argument, 3> arguments{};
    if (takesInteger)
        arguments.front().integer = input;
    if (body)
        arguments[firstToken].cancellation = &bodyToken;
    if (cleanup)
        arguments[firstToken + 1].cancellation = &cleanupToken;
    if (commandLine)
        arguments.front().cancellation = commandLine;
    std::uint64_t waits = 0;
    TaskScope scope;
    const auto project = [](void* destination, const std::uint64_t* payload, const NativeLists&) {
        if (destination)
            std::memcpy(destination, payload, sizeof(std::int64_t));
    };
    const auto task = scope.start<Completion>([&](CancellationToken taskCancellation) {
        return runNative(scope, entry, nullptr, arguments.data(),
                         CancellationToken::combine(caller.token(), std::move(taskCancellation)), waits,
                         project, output, {}, nullptr, {}, trace);
    });

    const bool requiresTwoSuspensions = mode == StartMode::RequireTwoSuspensions || mode == StartMode::CancelAfterSecond ||
        mode == StartMode::CancelCleanupAfterSecond;
    const bool requiresSuspension = mode == StartMode::RequireSuspension || requiresTwoSuspensions ||
        mode == StartMode::CancelAfterSuspension || mode == StartMode::CancelBodyAfterSuspension;
    if (requiresSuspension && (task.completed() || waits == 0))
        throw ExecutionFailure("native task did not suspend");
    if (mode == StartMode::CancelAfterSuspension)
        task.cancel();
    if (mode == StartMode::CancelBodyAfterSuspension)
        body->cancel();

    const auto firstObservedWait = waits;
    bool observedSecondWait = false;
    while (!task.completed()) {
        scope.poll();
        if (!task.completed()) {
            if (waits != firstObservedWait && !observedSecondWait) {
                observedSecondWait = true;
                if (mode == StartMode::CancelAfterSecond)
                    task.cancel();
                if (mode == StartMode::CancelCleanupAfterSecond)
                    cleanup->cancel();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (requiresTwoSuspensions && !observedSecondWait)
        throw ExecutionFailure("native task did not suspend twice");
    return task.result();
}

void printSourceTrace(const SourceTrace& trace)
{
    for (const auto& frame : trace.frames())
        std::cerr << "  at " << frame.function << " (" << frame.file << ':' << frame.line << ")\n";
}

}

int main(int argc, char** argv)
{
    const auto& entry = pipelang_native_v19_entry;
    const bool takesInteger = takesIntegerArgument(entry);
    const bool takesArguments = takesProcessArguments(entry);
    const bool hasSourceTrace = (entry.cancellationArguments & sourceTraceCapability) != 0;
    const int required = takesInteger ? 2 : 1;
    StartMode mode = StartMode::Normal;
    bool showSourceTrace = takesArguments && hasSourceTrace;
    if (!takesArguments && argc == required + 1) {
        const std::string_view option(argv[required]);
        if (option == "--cancel")
            mode = StartMode::Cancel;
        else if (option == "--require-suspension")
            mode = StartMode::RequireSuspension;
        else if (option == "--require-two-suspensions")
            mode = StartMode::RequireTwoSuspensions;
        else if (option == "--cancel-after-suspension")
            mode = StartMode::CancelAfterSuspension;
        else if (option == "--cancel-after-second-suspension")
            mode = StartMode::CancelAfterSecond;
        else if (option == "--cancel-body-after-suspension")
            mode = StartMode::CancelBodyAfterSuspension;
        else if (option == "--cancel-cleanup-after-second-suspension")
            mode = StartMode::CancelCleanupAfterSecond;
        else if (option == "--source-trace")
            showSourceTrace = true;
    }
    if (!takesArguments && argc != required && mode == StartMode::Normal && !showSourceTrace) {
        std::cerr << "usage: app" << (takesInteger ? " INTEGER" : "")
                  << " [--cancel|--require-suspension|--require-two-suspensions"
                     "|--cancel-after-suspension|--cancel-after-second-suspension|--cancel-body-after-suspension"
                     "|--cancel-cleanup-after-second-suspension|--source-trace]\n";
        return 64;
    }
    if (showSourceTrace && !hasSourceTrace) {
        std::cerr << "source trace metadata is unavailable. Recompile with --source-trace\n";
        return 64;
    }

    std::int64_t input = 0;
    if (takesInteger) {
        const std::string_view argument(argv[1]);
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), input);
        if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size()) {
            std::cerr << "expected a signed 64-bit integer\n";
            return 64;
        }
    }

    std::optional<SourceTrace> trace;
    if (showSourceTrace)
        trace.emplace();
    try {
        std::optional<HostedValue> arguments;
        if (takesArguments)
            arguments.emplace(processArguments(argc, argv));
        std::int64_t value = 0;
        const bool hasValue = entry.resultWords == resultHeaderWords + 1;
        const auto result = run(input, arguments ? &*arguments : nullptr, mode, hasValue ? &value : nullptr,
                                trace ? &*trace : nullptr);
        if (result.is_success()) {
            if (hasValue)
                std::cout << value << '\n';
            return 0;
        }

        std::cerr << result.error() << '\n';
        if (trace)
            printSourceTrace(*trace);
        return result.is_cancelled() ? 2 : 1;
    } catch (const std::exception& error) {
        std::cerr << safeErrorMessage(error) << '\n';
        if (trace)
            printSourceTrace(*trace);
        return 1;
    }
}
