// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../src/native/support/support.hpp"

inline void writePureHostProjectFixture(const std::filesystem::path& root) {
    using pipelang::tools::atomicWrite;
    std::filesystem::create_directories(root / "src");
    atomicWrite(root / "src/Pure.pipe", R"pipe(module Pure;
public Interface Math { pure float32 Support(float32 value); }
public Class Calculation {
    private Math math;
    public Calculation(Math host) { math = host; }
    public pure float32 Read(float32 value) => math.Support(value);
}
public model Class App {
    private Calculation calculation;
    public int accepted = 0;
    public App(Math host) { calculation = new Calculation(host); }
    public pure float32 Read(float32 value) => calculation.Read(value);
    public Result<float32,string> Change(float32 value) {
        accepted = accepted + 1;
        return Result<float32,string>.Success(calculation.Read(value));
    }
}
)pipe");
    atomicWrite(root / "Host.hpp", R"cpp(#pragma once
#include "Model.hpp"
#include <stdexcept>
class Host final : public pure_project::source::Pure::Math {
public:
    int fault = 0;
    float Support(float value) const override {
        if (fault == 1)
            throw std::runtime_error("pure project failure");
        if (fault == 2)
            throw pipelang::model::OperationCancelled("pure project cancelled");
        return value;
    }
};
)cpp");
    atomicWrite(root / "main.cpp", R"cpp(#include "Host.hpp"
#include <bit>
#include <cstdint>
#include <memory>
#include <stdexcept>
void require(bool value) {
    if (!value)
        throw std::runtime_error("pure project contract failed");
}
int main() {
    auto host = std::make_shared<Host>();
    pure_project::source::Pure::App owner(host);
    for (const auto word : {UINT32_C(0x80000000), UINT32_C(0x7fc01234), UINT32_C(1)}) {
        require(std::bit_cast<std::uint32_t>(owner.Read(std::bit_cast<float>(word))) == word);
    }
    const auto before = owner.snapshot();
    host->fault = 1;
    const auto failed = owner.Change(1.0f);
    require(failed.is_failure() && failed.error() == "pure project failure");
    require(owner.snapshot() == before);
    host->fault = 2;
    const auto cancelled = owner.Change(1.0f);
    require(cancelled.is_cancelled() && cancelled.error() == "pure project cancelled");
    require(owner.snapshot() == before);
    host->fault = 0;
    require(owner.Change(2.0f).is_success() && owner.snapshot().accepted == 1);
}
)cpp");
    atomicWrite(root / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.21.1)
project(PureHostProject LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
option(PURE_HOST_SANITIZERS "Instrument both model compilation modes" OFF)
foreach(mode IN ITEMS HEADER_ONLY COMPILED)
    string(TOLOWER "${mode}" name)
    pipelang_add_model(pure_${name} COMPILATION ${mode}
        PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Pure.pipe
        NAMESPACE pure_project OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    add_executable(pure_${name}_test main.cpp)
    target_link_libraries(pure_${name}_test PRIVATE pure_${name})
    if(PURE_HOST_SANITIZERS)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "Pure host sanitizers require GNU or Clang")
        endif()
        target_compile_options(pure_${name} INTERFACE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(pure_${name} INTERFACE -fsanitize=address,undefined)
    endif()
endforeach()
enable_testing()
add_test(NAME pure-header-only COMMAND pure_header_only_test)
add_test(NAME pure-compiled COMMAND pure_compiled_test)
)cmake");
}
