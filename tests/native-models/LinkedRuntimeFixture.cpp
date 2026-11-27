// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "LinkedRuntimeFixture.hpp"
#include "../../src/native/support/support.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pipelang::tools;

void invoke(const std::vector<std::string>& command) {
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0,
        "linked runtime fixture command failed: " + result.output + result.diagnostic);
}

void verifyStrictCommand(const std::string& command, bool sanitizer) {
    std::istringstream words(command);
    std::string word;
    bool fullDebug = false;
    bool expectedOptimization = false;
    while (words >> word) {
        // CMake quotes individual flags on some generators. Paths may contain
        // spaces; only exact flag words are relevant to these checks.
        while (!word.empty() && (word.front() == '"' || word.front() == '\''))
            word.erase(word.begin());
        while (!word.empty() && (word.back() == '"' || word.back() == '\''))
            word.pop_back();
        require(word != "-DNDEBUG" && !word.starts_with("-DNDEBUG=") &&
                word != "/DNDEBUG" && !word.starts_with("/DNDEBUG="),
            "linked fixture disabled native assertions");
        require(word != "-g0" && word != "-g1" && word != "-gline-tables-only",
            "linked fixture reduced full debug information");
        require(word != "-fno-sanitize" && !word.starts_with("-fno-sanitize="),
            "linked fixture disabled sanitizer instrumentation");
        if (word.starts_with("-O"))
            require(word == "-O1", "linked fixture has conflicting optimization: " + word);
        fullDebug = fullDebug || word == "-g";
        expectedOptimization = expectedOptimization || word == "-O1";
    }
    require(fullDebug && expectedOptimization, "linked fixture lost exact -O1 -g policy");
    if (sanitizer) {
        for (const auto* flag : {"-fsanitize=address,undefined", "-fno-omit-frame-pointer"})
            require(command.find(flag) != std::string::npos, "linked fixture lost sanitizer flag");
    }
}

void writeProducer(const fs::path& root) {
    fs::create_directories(root / "src");
    atomicWrite(root / "src/Counter.pipe", R"pipe(module Archive;
public model Class Counter {
    public int value;
    public Counter(int initial) { value = initial; }
    public void Set(int next) { value = next; }
    public pure int Read() => value;
}
)pipe");
    atomicWrite(root / "NativeCounters.hpp", R"cpp(#pragma once
#include <cstdint>
#include <memory>

class NativeCounters {
public:
    NativeCounters(std::int64_t first, std::int64_t second);
    ~NativeCounters();
    bool update(std::int64_t first, std::int64_t second);
    std::int64_t total() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
)cpp");
    atomicWrite(root / "NativeCounters.cpp", R"cpp(#include "NativeCounters.hpp"
#include "first/Model.hpp"
#include "second/Model.hpp"

struct NativeCounters::Impl {
    Impl(std::int64_t firstValue, std::int64_t secondValue)
        : first(firstValue), second(secondValue) {}
    first_fixture::source::Archive::Counter first;
    second_fixture::source::Archive::Counter second;
};

NativeCounters::NativeCounters(std::int64_t first, std::int64_t second)
    : impl_(std::make_unique<Impl>(first, second)) {}
NativeCounters::~NativeCounters() = default;
bool NativeCounters::update(std::int64_t first, std::int64_t second) {
    const auto firstSnapshot = impl_->first.snapshot();
    const auto secondSnapshot = impl_->second.snapshot();
    const auto firstResult = impl_->first.Set(first);
    const auto secondResult = impl_->second.Set(second);
    return firstResult.is_success() && secondResult.is_success()
        && firstSnapshot.value + secondSnapshot.value == 30
        && impl_->first.Read() == first && impl_->second.Read() == second;
}
std::int64_t NativeCounters::total() const {
    return impl_->first.Read() + impl_->second.Read();
}
)cpp");
    atomicWrite(root / "producer-policy.hpp", R"cpp(#pragma once
#ifdef NDEBUG
#error Linked fixture must retain native assertions
#endif
#if PIPELANG_LINKED_DIRECT != 29 || PIPELANG_LINKED_TRANSITIVE != 37
#error Linked runtime lost finalized producer policy
#endif
#if PIPELANG_MODEL_LINKED_RUNTIME != 1
#error Linked producer received the wrong SDK body ownership mode
#endif
)cpp");
    atomicWrite(root / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(LinkedOwner LANGUAGES CXX)
include(GNUInstallDirs)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
foreach(unit task_scope.cpp object_domain.cpp)
    if(NOT EXISTS "${PipeLangModel_NATIVE_RUNTIME_SOURCE_DIRECTORY}/${unit}")
        message(FATAL_ERROR "Installed SDK lacks canonical runtime source: ${unit}")
    endif()
endforeach()
foreach(header object.hpp task.hpp detail/object_domain_impl.hpp detail/task_scope_impl.hpp)
    if(NOT EXISTS "${PipeLangModel_NATIVE_RUNTIME_INCLUDE_DIRECTORY}/pipelang/model/${header}")
        message(FATAL_ERROR "Installed SDK lacks canonical runtime header: ${header}")
    endif()
endforeach()
option(OWNER_SANITIZERS "Use the matching full-debug sanitizer profile" OFF)
set(LINKED_CASE "valid" CACHE STRING "Focused contract witness")
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "This full-debug policy fixture requires GNU or Clang")
endif()
add_library(transitive_policy INTERFACE)
target_compile_definitions(transitive_policy INTERFACE PIPELANG_LINKED_TRANSITIVE=37)
add_library(OFF INTERFACE)
target_compile_definitions(OFF INTERFACE PIPELANG_FALSE_NAMED_POLICY=43)
add_library(producer_policy INTERFACE)
target_link_libraries(producer_policy INTERFACE transitive_policy OFF)
target_compile_definitions(producer_policy INTERFACE PIPELANG_LINKED_DIRECT=29)
target_compile_options(producer_policy INTERFACE -O1 -g -Wall -Wextra -Wpedantic -Werror
    -ffp-contract=off "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/producer-policy.hpp\"")
if(OWNER_SANITIZERS)
    target_compile_options(producer_policy INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(producer_policy INTERFACE -fsanitize=address,undefined)
endif()
if(LINKED_CASE STREQUAL "dynamic-policy")
    target_link_libraries(producer_policy INTERFACE "$<$<BOOL:1>:transitive_policy>")
endif()
pipelang_add_linked_runtime(runtime POLICY_TARGET producer_policy)
if(LINKED_CASE STREQUAL "duplicate-runtime")
    pipelang_add_linked_runtime(runtime POLICY_TARGET producer_policy)
endif()
if(LINKED_CASE STREQUAL "policy-change")
    target_compile_definitions(producer_policy INTERFACE PIPELANG_POLICY_CHANGED=1)
elseif(LINKED_CASE STREQUAL "transitive-policy-change")
    target_compile_definitions(transitive_policy INTERFACE PIPELANG_TRANSITIVE_CHANGED=1)
elseif(LINKED_CASE STREQUAL "false-name-transitive-policy-change")
    target_compile_definitions(OFF INTERFACE PIPELANG_FALSE_NAMED_CHANGED=1)
endif()
set(second_runtime runtime)
if(LINKED_CASE STREQUAL "duplicate-owner")
    pipelang_add_linked_runtime(other_runtime POLICY_TARGET producer_policy)
    set(second_runtime other_runtime)
elseif(LINKED_CASE STREQUAL "profile")
    # Both models reach one owner. The install helper checks PROFILE before
    # OWNER, and the refusal assertion below requires that exact property.
    add_library(other_policy INTERFACE)
    target_link_libraries(other_policy INTERFACE producer_policy)
    target_compile_definitions(other_policy INTERFACE PIPELANG_OTHER_ABI_PROFILE=1)
    pipelang_add_linked_runtime(other_runtime POLICY_TARGET other_policy)
    set(second_runtime other_runtime)
endif()
pipelang_add_model(first_model COMPILATION COMPILED RUNTIME_TARGET runtime
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Counter.pipe
    NAMESPACE first_fixture OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/first")
pipelang_add_model(second_model COMPILATION COMPILED RUNTIME_TARGET ${second_runtime}
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Counter.pipe
    NAMESPACE second_fixture OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/second")
if(LINKED_CASE STREQUAL "header-runtime")
    pipelang_add_model(invalid_header RUNTIME_TARGET runtime
        PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Counter.pipe
        NAMESPACE invalid_header OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/invalid-header")
endif()
# Preserve the default mode witness separately; linking it to the owner is an
# intentional rejected case rather than a change to its default behavior.
pipelang_add_model(header_model
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Counter.pipe
    NAMESPACE header_fixture OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/header-generated")
if(TARGET header_model_implementation)
    message(FATAL_ERROR "Default HEADER_ONLY acquired an implementation archive")
endif()
add_library(native_counters STATIC NativeCounters.cpp)
set_target_properties(native_counters PROPERTIES EXPORT_NAME Counters POSITION_INDEPENDENT_CODE ON)
target_compile_features(native_counters PUBLIC cxx_std_20)
target_include_directories(native_counters PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" "$<INSTALL_INTERFACE:include>")
target_include_directories(native_counters PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(native_counters PRIVATE
    "$<BUILD_INTERFACE:first_model>" "$<BUILD_INTERFACE:second_model>")
if(LINKED_CASE STREQUAL "mixed")
    target_link_libraries(native_counters PRIVATE "$<BUILD_INTERFACE:header_model>")
endif()
pipelang_install_model_implementation(first_model OWNER native_counters
    EXPORT LinkedOwnerTargets ARCHIVE_DESTINATION lib)
pipelang_install_model_implementation(second_model OWNER native_counters
    EXPORT LinkedOwnerTargets ARCHIVE_DESTINATION lib)
# Repeated linked registration is idempotent. The independent default target
# above remains HEADER_ONLY; it is linked to this owner only in the mixed case.
pipelang_install_model_implementation(first_model OWNER native_counters
    EXPORT LinkedOwnerTargets ARCHIVE_DESTINATION lib)
if(LINKED_CASE STREQUAL "export-conflict")
    pipelang_install_model_implementation(second_model OWNER native_counters
        EXPORT DifferentTargets ARCHIVE_DESTINATION lib)
endif()
install(TARGETS native_counters EXPORT LinkedOwnerTargets ARCHIVE DESTINATION lib)
install(FILES NativeCounters.hpp DESTINATION include)
install(EXPORT LinkedOwnerTargets NAMESPACE Fixture:: DESTINATION lib/cmake/LinkedOwner)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/LinkedOwnerConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/LinkedOwnerTargets.cmake")
]=])
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LinkedOwnerConfig.cmake"
    DESTINATION lib/cmake/LinkedOwner)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(TARGET runtime first_model_implementation second_model_implementation native_counters
    PROPERTY EXPORT_COMPILE_COMMANDS ON)
)cmake");
}

void writeConsumer(const fs::path& root) {
    fs::create_directories(root);
    atomicWrite(root / "main.cpp", R"cpp(#include <NativeCounters.hpp>
#ifdef PIPELANG_LINKED_DIRECT
#error Private producer policy escaped into installed consumer
#endif
#ifdef PIPELANG_MODEL_LINKED_RUNTIME
#error Private SDK mode escaped into installed consumer
#endif
int main() {
    NativeCounters counters(11, 19);
    if (counters.total() != 30 || !counters.update(17, 29))
        return 1;
    return counters.total() == 46 ? 0 : 2;
}
)cpp");
    atomicWrite(root / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(InstalledLinkedOwnerConsumer LANGUAGES CXX)
option(OWNER_SANITIZERS "Use the matching instrumented native package" OFF)
find_package(LinkedOwner CONFIG REQUIRED)
if(TARGET PipeLang::ModelRuntime OR TARGET first_model OR TARGET second_model)
    message(FATAL_ERROR "Private SDK/model targets escaped installed native owner")
endif()
add_executable(linked_owner_consumer main.cpp)
target_link_libraries(linked_owner_consumer PRIVATE Fixture::Counters)
target_compile_options(linked_owner_consumer PRIVATE -O1 -g -Wall -Wextra -Wpedantic -Werror)
if(OWNER_SANITIZERS)
    target_compile_options(linked_owner_consumer PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(linked_owner_consumer PRIVATE -fsanitize=address,undefined)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(TARGET linked_owner_consumer PROPERTY EXPORT_COMPILE_COMMANDS ON)
)cmake");
}

std::vector<std::string> configure(const fs::path& source, const fs::path& build,
                                   const fs::path& prefix, bool sanitizer) {
    std::vector<std::string> command{
        nativeTool("cmake").string(), "-S", source.string(), "-B", build.string(),
        "-DCMAKE_BUILD_TYPE=", "-DCMAKE_CXX_FLAGS=", "-DCMAKE_EXE_LINKER_FLAGS=",
        "-DCMAKE_STATIC_LINKER_FLAGS=", "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
        "-DCMAKE_PREFIX_PATH=" + prefix.string(), "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
        "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
        "-DOWNER_SANITIZERS=" + std::string(sanitizer ? "ON" : "OFF")};
#ifdef _WIN32
    command.insert(command.end(), {"-G", "Ninja"});
#endif
    return command;
}

void verifyProducerPolicy(const fs::path& build, bool sanitizer) {
    const auto commands = parseJson(readFile(build / "compile_commands.json", 4U << 20));
    std::size_t taskUnits = 0;
    std::size_t graphUnits = 0;
    std::size_t implementations = 0;
    std::size_t owners = 0;
    std::size_t precompiledHeaders = 0;
    for (const auto& entry : commands) {
        const auto file = fs::path(entry.at("file").get<std::string>()).filename().string();
        if (file == "task_scope.cpp")
            ++taskUnits;
        else if (file == "object_domain.cpp")
            ++graphUnits;
        else if (file.starts_with("Implementation_"))
            ++implementations;
        else if (file == "cmake_pch.hxx.cxx")
            ++precompiledHeaders;
        else if (file == "NativeCounters.cpp")
            ++owners;
        else
            throw std::runtime_error("unexpected linked producer translation unit: " + file);
        const auto command = entry.at("command").get<std::string>();
        verifyStrictCommand(command, sanitizer);
        for (const auto* required : {"PIPELANG_LINKED_DIRECT=29", "PIPELANG_LINKED_TRANSITIVE=37",
                                     "PIPELANG_FALSE_NAMED_POLICY=43",
                                     "PIPELANG_MODEL_LINKED_RUNTIME=1", "-ffp-contract=off",
                                     "producer-policy.hpp", "-O1", "-g"})
            require(command.find(required) != std::string::npos,
                "linked producer lost policy: " + std::string(required));
        require(command.find("-g1") == std::string::npos, "linked producer reduced full debug information");
        if (sanitizer) {
            for (const auto* required : {"-fsanitize=address,undefined", "-fno-omit-frame-pointer"})
                require(command.find(required) != std::string::npos,
                    "linked producer lost sanitizer policy: " + std::string(required));
        }
    }
    require(taskUnits == 1 && graphUnits == 1 && owners == 1 &&
            implementations >= 2 && precompiledHeaders == 2,
        "two models did not share one separately compiled runtime");
}

void verifyRefusal(const fs::path& producer, const fs::path& output,
                   const fs::path& sdk, bool sanitizer,
                   const std::string& scenario, const std::vector<std::string>& diagnostics) {
    auto command = configure(producer, output / ("rejected-" + scenario), sdk, sanitizer);
    command.push_back("-DLINKED_CASE=" + scenario);
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    const auto report = result.output + result.diagnostic;
    bool recognized = false;
    for (const auto& diagnostic : diagnostics) {
        if (report.find(diagnostic) != std::string::npos)
            recognized = true;
    }
    require(result.exit != 0 && recognized,
        "linked runtime contract was not specifically refused: " + scenario + "\n" +
        result.output + result.diagnostic);
}
} // namespace

void prepareLinkedRuntimeFixture(const std::filesystem::path& sdk,
                                 const std::filesystem::path& output,
                                 bool sanitizer) {
    using namespace pipelang::tools;
    require(!fs::exists(output), "linked fixture output must be fresh");
    fs::create_directories(output);
    const auto producer = output / "producer";
    const auto build = output / "producer-build";
    const auto installed = output / "installed";
    const auto relocated = output / "relocated";
    const auto consumer = output / "consumer";
    const auto consumerBuild = output / "consumer-build";
    writeProducer(producer);
    writeConsumer(consumer);
    auto command = configure(producer, build, sdk, sanitizer);
    command.push_back("-DCMAKE_INSTALL_PREFIX=" + installed.string());
    invoke(command);
    verifyProducerPolicy(build, sanitizer);
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--parallel", "1"});
    invoke({nativeTool("cmake").string(), "--install", build.string()});
    fs::rename(installed, relocated);
    std::size_t headers = 0;
    std::size_t archives = 0;
    for (const auto& entry : fs::recursive_directory_iterator(relocated)) {
        require(!fs::is_symlink(entry.symlink_status()), "installed linked fixture contains symlink");
        if (!entry.is_regular_file())
            continue;
        const auto extension = entry.path().extension();
        if (extension == ".hpp") {
            require(entry.path().filename() == "NativeCounters.hpp", "private SDK header escaped native owner");
            ++headers;
        } else if (extension == ".a" || extension == ".lib") {
            ++archives;
        } else {
            require(extension == ".cmake", "unexpected installed native owner artifact");
            const auto contents = readFile(entry.path(), 4U << 20);
            for (const auto& privatePath : {producer, build, sdk})
                require(contents.find(privatePath.generic_string()) == std::string::npos,
                    "installed runtime closure retained private path");
            for (const auto* privateUsage : {"PipeLang::ModelRuntime", "producer_policy", "producer-policy.hpp",
                                            "PIPELANG_MODEL_LINKED_RUNTIME", "Model.hpp"})
                require(contents.find(privateUsage) == std::string::npos,
                    "installed hidden owner leaked private usage requirement");
        }
    }
    require(headers == 1 && archives == 4,
        "installed native owner lost its two-model, one-runtime four-archive closure");
    verifyRefusal(producer, output, sdk, sanitizer, "duplicate-runtime",
        {"PipeLang linked runtime target already exists:"});
    verifyRefusal(producer, output, sdk, sanitizer, "duplicate-owner",
        {"PIPELANG_RUNTIME_OWNER"});
    verifyRefusal(producer, output, sdk, sanitizer, "profile",
        {"PIPELANG_RUNTIME_PROFILE"});
    verifyRefusal(producer, output, sdk, sanitizer, "mixed",
        {"PIPELANG_RUNTIME_LINKAGE"});
    verifyRefusal(producer, output, sdk, sanitizer, "export-conflict",
        {"already belongs to a different installation"});
    verifyRefusal(producer, output, sdk, sanitizer, "policy-change",
        {"policy/profile changed after declaration"});
    verifyRefusal(producer, output, sdk, sanitizer, "transitive-policy-change",
        {"policy/profile changed after declaration"});
    verifyRefusal(producer, output, sdk, sanitizer, "false-name-transitive-policy-change",
        {"policy/profile changed after declaration"});
    verifyRefusal(producer, output, sdk, sanitizer, "dynamic-policy",
        {"Unsupported dynamic linked runtime policy dependency:"});
    verifyRefusal(producer, output, sdk, sanitizer, "header-runtime",
        {"RUNTIME_TARGET requires COMPILATION COMPILED"});
    fs::rename(producer, output / "retained-producer");
    fs::rename(build, output / "retained-producer-build");
    auto consumerCommand = configure(consumer, consumerBuild, relocated, sanitizer);
    consumerCommand.push_back("-DCMAKE_DISABLE_FIND_PACKAGE_PipeLangModel=TRUE");
    invoke(consumerCommand);
    const auto consumerCommands = readFile(consumerBuild / "compile_commands.json", 4U << 20);
    for (const auto& entry : parseJson(consumerCommands))
        verifyStrictCommand(entry.at("command").get<std::string>(), sanitizer);
    for (const auto& privatePath : {producer, build, sdk})
        require(consumerCommands.find(privatePath.generic_string()) == std::string::npos,
            "installed consumer retained private producer or SDK include path");
    for (const auto* privateUsage : {"producer-policy.hpp", "PIPELANG_MODEL_LINKED_RUNTIME", "generated/"})
        require(consumerCommands.find(privateUsage) == std::string::npos,
            "installed consumer retained private model policy");
    if (sanitizer) {
        for (const auto* required : {"-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"})
            require(consumerCommands.find(required) != std::string::npos,
                "installed consumer lost matching sanitizer policy");
        require(consumerCommands.find("-g1") == std::string::npos, "installed consumer reduced debug information");
    }
    invoke({nativeTool("cmake").string(), "--build", consumerBuild.string(), "--parallel", "1"});
    atomicWrite(output / "prepared.json", canonical({{"status", "prepared-not-run"},
        {"sanitizer", sanitizer}, {"compiled_models", 2}, {"runtime_owners", 1},
        {"installed_archives", archives}, {"public_headers", headers},
        {"private_sdk_headers_required", false}, {"per_command_deadline_seconds", 90},
        {"resource_wrapper", "parent-owned whole-workload containment required"}}));
}
