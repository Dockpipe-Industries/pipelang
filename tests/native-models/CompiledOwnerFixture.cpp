// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "CompiledOwnerFixture.hpp"
#include "../../src/native/support/support.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace pipelang::tools;

void invoke(const std::vector<std::string>& command) {
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0,
            "compiled owner fixture command failed: " + result.output + result.diagnostic);
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
    atomicWrite(root / "NativeCounter.hpp", R"cpp(#pragma once
#include <cstdint>
#include <memory>

class NativeCounter {
public:
    explicit NativeCounter(std::int64_t initial);
    ~NativeCounter();
    std::int64_t read() const;
    bool updateAndRetain(std::int64_t next);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
)cpp");
    atomicWrite(root / "NativeCounter.cpp", R"cpp(#include "NativeCounter.hpp"
#include "Model.hpp"

struct NativeCounter::Impl {
    explicit Impl(std::int64_t initial)
        : model(initial) {
    }

    owner_fixture::source::Archive::Counter model;
};

NativeCounter::NativeCounter(std::int64_t initial)
    : impl_(std::make_unique<Impl>(initial)) {
}

NativeCounter::~NativeCounter() = default;

std::int64_t NativeCounter::read() const {
    return impl_->model.Read();
}

bool NativeCounter::updateAndRetain(std::int64_t next) {
    const auto before = impl_->model.Read();
    const auto snapshot = impl_->model.snapshot();
    const auto result = impl_->model.Set(next);
    return result.is_success() && snapshot.value == before && impl_->model.Read() == next;
}
)cpp");
    atomicWrite(root / "producer-policy.hpp", R"cpp(#pragma once
#if PIPELANG_OWNER_DIRECT != 29 || PIPELANG_OWNER_TRANSITIVE != 37
#error Compiled archive lost its producer policy
#endif
)cpp");
    atomicWrite(root / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(CompiledOwner LANGUAGES CXX)
include(GNUInstallDirs)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
option(OWNER_SANITIZERS "Instrument the producer and installed consumer consistently" OFF)
option(OWNER_CONFLICT "Exercise duplicate installation refusal" OFF)
pipelang_add_model(owner_model COMPILATION COMPILED
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Counter.pipe
    NAMESPACE owner_fixture OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
pipelang_add_model(header_model
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Counter.pipe
    NAMESPACE header_fixture OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/header-generated")
add_library(native_counter STATIC NativeCounter.cpp)
set_target_properties(native_counter PROPERTIES EXPORT_NAME Counter POSITION_INDEPENDENT_CODE ON)
target_compile_features(native_counter PUBLIC cxx_std_20)
target_include_directories(native_counter PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" "$<INSTALL_INTERFACE:include>")
target_link_libraries(native_counter PRIVATE "$<BUILD_INTERFACE:owner_model>")
pipelang_install_model_implementation(owner_model OWNER native_counter
    EXPORT OwnerTargets ARCHIVE_DESTINATION lib)
# Default HEADER_ONLY usage has no private archive to register.
pipelang_install_model_implementation(header_model OWNER native_counter
    EXPORT OwnerTargets ARCHIVE_DESTINATION lib)
# Repeated admission must not duplicate an archive installation or dependency.
pipelang_install_model_implementation(owner_model OWNER native_counter
    EXPORT OwnerTargets ARCHIVE_DESTINATION lib)
if(OWNER_CONFLICT)
    pipelang_install_model_implementation(owner_model OWNER native_counter
        EXPORT DifferentTargets ARCHIVE_DESTINATION lib)
endif()
add_library(producer_policy INTERFACE)
target_compile_definitions(producer_policy INTERFACE PIPELANG_OWNER_TRANSITIVE=37)
target_link_libraries(owner_model INTERFACE producer_policy)
target_compile_definitions(owner_model INTERFACE PIPELANG_OWNER_DIRECT=29)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(owner_model INTERFACE -ffp-contract=off
        "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/producer-policy.hpp\"")
endif()
if(OWNER_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Owner sanitizer fixture requires GNU or Clang")
    endif()
    target_compile_options(owner_model INTERFACE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(owner_model INTERFACE -fsanitize=address,undefined)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(TARGET owner_model_implementation native_counter PROPERTY EXPORT_COMPILE_COMMANDS ON)
install(TARGETS native_counter EXPORT OwnerTargets ARCHIVE DESTINATION lib)
install(FILES NativeCounter.hpp DESTINATION include)
install(EXPORT OwnerTargets NAMESPACE Fixture:: DESTINATION lib/cmake/CompiledOwner)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/CompiledOwnerConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/OwnerTargets.cmake")
]=])
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CompiledOwnerConfig.cmake"
    DESTINATION lib/cmake/CompiledOwner)
)cmake");
}

void writeConsumer(const fs::path& root) {
    fs::create_directories(root);
    atomicWrite(root / "main.cpp", R"cpp(#include <NativeCounter.hpp>
#ifdef PIPELANG_OWNER_DIRECT
#error Private producer policy escaped into the installed consumer
#endif
int main() {
    NativeCounter counter(19);
    if (counter.read() != 19 || !counter.updateAndRetain(41))
        return 1;
    return counter.read() == 41 ? 0 : 2;
}
)cpp");
    atomicWrite(root / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(InstalledOwnerConsumer LANGUAGES CXX)
option(OWNER_SANITIZERS "Use the matching instrumented native package" OFF)
find_package(CompiledOwner CONFIG REQUIRED)
if(TARGET PipeLang::ModelRuntime OR TARGET owner_model)
    message(FATAL_ERROR "Private model build targets escaped the native package")
endif()
add_executable(owner_consumer main.cpp)
target_link_libraries(owner_consumer PRIVATE Fixture::Counter)
if(OWNER_SANITIZERS)
    target_compile_options(owner_consumer PRIVATE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(owner_consumer PRIVATE -fsanitize=address,undefined)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(TARGET owner_consumer PROPERTY EXPORT_COMPILE_COMMANDS ON)
)cmake");
}

std::vector<std::string> configure(const fs::path& source, const fs::path& build,
                                   const fs::path& prefix, bool sanitizer) {
    std::vector<std::string> command{
        nativeTool("cmake").string(), "-S", source.string(), "-B", build.string(),
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(),
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
    std::size_t implementations = 0;
    std::size_t owners = 0;
    std::size_t precompiledHeaders = 0;
    for (const auto& entry : commands) {
        const auto file = fs::path(entry.at("file").get<std::string>()).filename().string();
        if (file.starts_with("Implementation_"))
            ++implementations;
        else if (file == "cmake_pch.hxx.cxx")
            ++precompiledHeaders;
        else if (file == "NativeCounter.cpp")
            ++owners;
        else
            throw std::runtime_error("unexpected compiled-owner producer translation unit");
        const auto command = entry.at("command").get<std::string>();
        for (const auto* required : {"PIPELANG_OWNER_DIRECT=29", "PIPELANG_OWNER_TRANSITIVE=37",
                                     "-ffp-contract=off", "producer-policy.hpp"})
            require(command.find(required) != std::string::npos,
                    "compiled owner lost producer policy: " + std::string(required));
        if (sanitizer) {
            for (const auto* required : {"-O1", "-g1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"})
                require(command.find(required) != std::string::npos,
                        "compiled owner lost sanitizer policy: " + std::string(required));
        }
    }
    require(implementations != 0 && owners == 1 && precompiledHeaders == 1,
            "compiled owner policy coverage is incomplete");
}

}

void prepareCompiledOwnerFixture(const std::filesystem::path& sdk,
                                 const std::filesystem::path& output,
                                 bool sanitizer) {
    using namespace pipelang::tools;
    require(!fs::exists(output), "compiled owner fixture output must be fresh");
    fs::create_directories(output);
    const auto producer = output / "producer";
    const auto build = output / "producer-build";
    const auto installed = output / "installed";
    const auto relocated = output / "relocated";
    const auto consumer = output / "consumer";
    const auto consumerBuild = output / "consumer-build";
    writeProducer(producer);
    writeConsumer(consumer);
    auto producerCommand = configure(producer, build, sdk, sanitizer);
    producerCommand.push_back("-DCMAKE_INSTALL_PREFIX=" + installed.string());
    invoke(producerCommand);
    verifyProducerPolicy(build, sanitizer);
    invoke({nativeTool("cmake").string(), "--build", build.string(), "--parallel", "1"});
    invoke({nativeTool("cmake").string(), "--install", build.string()});
    fs::rename(installed, relocated);
    std::size_t headers = 0;
    std::size_t archives = 0;
    for (const auto& entry : fs::recursive_directory_iterator(relocated)) {
        require(!fs::is_symlink(entry.symlink_status()), "installed fixture contains a symbolic entry");
        if (!entry.is_regular_file())
            continue;
        const auto name = entry.path().filename().string();
        if (entry.path().extension() == ".hpp") {
            require(name == "NativeCounter.hpp", "private generated/runtime header was installed");
            ++headers;
        } else if (entry.path().extension() == ".a" || entry.path().extension() == ".lib") {
            ++archives;
        } else {
            require(entry.path().extension() == ".cmake", "unexpected installed owner artifact");
            const auto contents = readFile(entry.path(), 4U << 20);
            for (const auto& privatePath : {producer, build, sdk})
                require(contents.find(privatePath.generic_string()) == std::string::npos,
                        "installed native export retained a private producer path");
            require(contents.find("PipeLang::ModelRuntime") == std::string::npos &&
                    contents.find("Model.hpp") == std::string::npos,
                    "installed native export retained private model usage requirements");
        }
    }
    require(headers == 1 && archives == 2, "installed native owner lost its two-archive closure");
    auto rejected = configure(producer, output / "conflicting-install", sdk, sanitizer);
    rejected.push_back("-DOWNER_CONFLICT=ON");
    const auto refusal = process(rejected, {}, std::chrono::seconds(90), 4U << 20);
    require(refusal.exit != 0 &&
            (refusal.output + refusal.diagnostic).find("already belongs to a different installation") != std::string::npos,
            "conflicting compiled archive installation was not refused");
    // Make the original source/build/install locations unavailable while the
    // independent consumer configures and links the relocated native package.
    fs::rename(producer, output / "retained-producer");
    fs::rename(build, output / "retained-producer-build");
    invoke(configure(consumer, consumerBuild, relocated, sanitizer));
    const auto consumerCommands = readFile(consumerBuild / "compile_commands.json", 4U << 20);
    require(consumerCommands.find(sdk.generic_string()) == std::string::npos &&
            consumerCommands.find(producer.generic_string()) == std::string::npos &&
            consumerCommands.find(build.generic_string()) == std::string::npos,
            "installed native consumer retained private compiler or producer include paths");
    if (sanitizer) {
        for (const auto* required : {"-O1", "-g1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"})
            require(consumerCommands.find(required) != std::string::npos,
                    "installed consumer lost matching sanitizer policy: " + std::string(required));
    }
    invoke({nativeTool("cmake").string(), "--build", consumerBuild.string(), "--parallel", "1"});
    atomicWrite(output / "prepared.json", canonical({{"status", "prepared-not-run"},
        {"sanitizer", sanitizer}, {"installed_archives", archives},
        {"public_headers", headers}, {"private_model_headers_installed", false}}));
}
