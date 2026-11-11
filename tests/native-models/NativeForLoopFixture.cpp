// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NativeForLoopFixture.hpp"
#include "../../src/native/sdk-tools/SourceProject.hpp"
#include <set>

namespace {
using namespace pipelang::tools;
void invoke(const std::vector<std::string>& command)
{
    const auto result = process(command, {}, std::chrono::seconds(90), 4U << 20);
    require(result.exit == 0, "for loop fixture failed: " + result.output + result.diagnostic);
}
}

void nativeForLoopContracts(const std::filesystem::path& prefix,
                            const std::filesystem::path& output)
{
    fs::create_directories(output / "src");
    atomicWrite(output / "src/Queries.pipe", R"pipe(module ForLoops;
public Interface Events { Result<int,string> Record(int value); }
public model Class Runner {
    private Events events;
    public Runner(Events service) { events = service; }
    private void Mark(int value) { int observed = events.Record(value).Value; }
    private int Start() { Mark(1); return 0; }
    private bool Condition(int index) { Mark(10 + index); return index < 2; }
    private int Update(int index) { Mark(20 + index); return index + 1; }
    public int Run(int mode) {
        for (int index = Start(); Condition(index); index = Update(index)) {
            Mark(100 + index);
            try {
                if (mode == 1 && index == 0)
                    continue;
                if (mode == 2 && index == 0)
                    break;
                if (mode == 3 && index == 0)
                    return 7;
                if (mode == 4 && index == 0)
                    throw Error("body fault");
            } finally {
                Mark(200 + index);
            }
            Mark(150 + index);
        }
        Mark(300);
        return 9;
    }
    public int ZeroTrip() {
        for (int index = Start(); Condition(2); index = Update(index))
            Mark(999);
        return 11;
    }
    public int CallClauses() {
        int index = 0;
        for (Mark(1); index < 2; Mark(20 + index)) {
            Mark(100 + index);
            index = index + 1;
        }
        return index;
    }
}
public model Class Queries {
    public pure int Sum() {
        int sum = 0;
        for (int index = 0; index < 4; index = index + 1)
            sum = sum + index;
        return sum;
    }
    public pure int Assigned() {
        int index = 99;
        int sum = 0;
        for (index = 1; index <= 3; index = index + 1)
            sum = sum + index;
        return sum;
    }
    public pure int Step() {
        int sum = 0;
        for (int index = 1; index < 8; index = index + 2)
            sum = sum + index;
        return sum;
    }
    public pure FixedArray<int,3> Fixed() {
        FixedArray<int,3> values = FixedArray<int,3>.Create(0);
        for (int index = 0; index < 3; index = index + 1)
            values.Set(index, index + 2);
        return values;
    }
    public pure int Nested() {
        int sum = 0;
        for (int row = 0; row < 3; row = row + 1) {
            for (int column = 0; column < 2; column = column + 1)
                sum = sum + row * 2 + column;
        }
        return sum;
    }
    public pure int Omitted() {
        int index = 0;
        for (;;) {
            if (index == 3)
                break;
            index = index + 1;
        }
        return index;
    }
    public pure int MissingCondition() {
        int result = 0;
        for (int index = 0;; index = index + 1) {
            if (index == 2)
                break;
            result = result + 1;
        }
        return result;
    }
    public pure int MissingInitializer() {
        int index = 0;
        for (; index < 3; index = index + 1) { }
        return index;
    }
    public pure int MissingUpdate() {
        int index = 0;
        for (; index < 3;) {
            index = index + 1;
        }
        return index;
    }
    public pure int MixedControl() {
        int sum = 0;
        FixedArray<int,3> values = FixedArray<int,3>.Create(1);
        for (int outer = 0; outer < 3; outer = outer + 1) {
            int inner = 0;
            while (inner < 3) {
                inner = inner + 1;
                if (inner == 1)
                    continue;
                if (inner == 3)
                    break;
                sum = sum + 1;
            }
            foreach (int value in values) {
                if (value == 1)
                    break;
                sum = sum + 100;
            }
            if (outer == 1)
                continue;
            sum = sum + 10;
        }
        return sum;
    }
    public pure int InnerForControl() {
        int sum = 0;
        int outer = 0;
        while (outer < 2) {
            outer = outer + 1;
            for (int index = 0; index < 4; index = index + 1) {
                if (index == 0)
                    continue;
                if (index == 2)
                    break;
                sum = sum + 1;
            }
        }
        return sum;
    }
    public pure int UnsupportedBreak() {
        int sum = 0;
        for (int index = 0; index < 4; index = index + 1) {
            if (index == 2)
                break;
            sum = sum + index;
        }
        return sum;
    }
    public pure int UnsupportedContinue() {
        int sum = 0;
        for (int index = 0; index < 4; index = index + 1) {
            if (index == 2)
                continue;
            sum = sum + index;
        }
        return sum;
    }
    public pure int MutatedControl() {
        int count = 0;
        for (int index = 0; index < 4; index = index + 1) {
            index = index + 1;
            count = count + 1;
        }
        return count;
    }
    public pure int OverflowControl() {
        int count = 0;
        for (int index = 9223372036854775806; index <= 9223372036854775807; index = index + 1)
            count = count + 1;
        return count;
    }
    public pure int Unbounded() {
        for (;;) { }
        return 0;
    }
}
)pipe");
    const auto manifest = prefix / "share/pipelang-model/sdk-manifest.json";
    const auto compiler = nativeExecutable(prefix / "bin/pipelang-modelc");
    const auto project = SourceProject::load(output, "src", "Queries.pipe");
    const auto request = project.request("for_header");
    const auto response = process({compiler.string()}, request.dump(), std::chrono::seconds(45), 4U << 20);
    require(response.exit == 0, "for compiler transport failed");
    const auto parsed = parseJson(response.output, 4U << 20);
    require(parsed.at("ok") == true, "for source refused: " + parsed.dump());
    const auto& model = parsed.at("manifest");
    std::set<std::string> methods;
    for (const auto& item : model.at("nativeNumericExports")) {
        if (item.at("owner") == "ForLoops.Queries")
            methods.insert(item.at("method").get<std::string>());
    }
    // MissingInitializer has the same stable ascending owner as an existing
    // while loop; body-mutated or absent update clauses remain conservative.
    const std::set<std::string> expected{"Sum", "Assigned", "Step", "Fixed", "Nested", "MissingInitializer"};
    require(methods == expected, "for numeric admission widened or lost its bounded counted proof: " + canonical(methods));
    admitModel(model, "for_header");
    for (const std::string body : {
        "for (int i = 0 i < 3; i = i + 1) { } return 0;",
        "for (int i = 0; i < 3 i = i + 1) { } return 0;",
        "for (int i = 0; i < 3; int other = 1) { } return 0;",
        "for (int i = 0; 1; i = i + 1) { } return 0;",
        "for (int i = 0; i < 3; i + 1) { } return 0;",
        "for (int i = 0; i < 1; i = i + 1) { } return i;",
        "for (int i = 0; i < 1; i = local) { int local = 1; } return 0;",
        "break; return 0;", "continue; return 0;"}) {
        auto invalid = request;
        invalid["modules"][0]["source"] = "module ForLoops; public model Class Invalid { public pure int Check() { " + body + " } }";
        const auto rejected = process({compiler.string()}, invalid.dump(), std::chrono::seconds(45), 4U << 20);
        require(rejected.exit == 0 && parseJson(rejected.output, 4U << 20).at("ok") == false,
            "for admitted malformed header or escaped loop scope: " + body);
    }
    generateProjectBindings(prefix, manifest, fileHash(manifest), compiler, output, "src", "Queries.pipe",
        "for_header", output / "header");
    const auto information = compiledProjectInformation(prefix, manifest, fileHash(manifest), compiler,
        output, "src", "Queries.pipe", "for_compiled");
    generateCompiledProjectBindings(prefix, manifest, fileHash(manifest), compiler, output, "src", "Queries.pipe",
        "for_compiled", output / "compiled", information.at("plan_sha256").get<std::string>());
    atomicWrite(output / "checks.hpp", R"cpp(#pragma once
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>
extern bool forbidForAllocations;

template<class Query>
concept ExposesUnboundedFor = requires { &Query::Unbounded; };
template<class Query>
concept ExposesOverflowFor = requires { &Query::OverflowControl; };
template<class Query>
concept ExposesMutatedFor = requires { &Query::MutatedControl; };
template<class Query>
concept ExposesBreakFor = requires { &Query::UnsupportedBreak; };
template<class Query>
concept ExposesContinueFor = requires { &Query::UnsupportedContinue; };

template<class Query>
constexpr bool verifyNumericFor()
{
    static_assert(!ExposesUnboundedFor<Query> && !ExposesOverflowFor<Query> && !ExposesMutatedFor<Query>);
    static_assert(!ExposesBreakFor<Query> && !ExposesContinueFor<Query>);
    static_assert(noexcept(Query::Sum()) && noexcept(Query::Fixed()));
    const auto values = Query::Fixed();
    return Query::Sum() == 6 && Query::Assigned() == 6 && Query::Step() == 16
        && Query::Nested() == 15 && Query::MissingInitializer() == 3 && values.count() == 3
        && values.uncheckedAt(0) == 2 && values.uncheckedAt(1) == 3 && values.uncheckedAt(2) == 4;
}

template<class Port>
class Recorder final : public Port {
public:
    pipelang::model::Result<std::int64_t, std::string> Record(std::int64_t value) override
    {
        events.push_back(value);
        return pipelang::model::Result<std::int64_t, std::string>::Success(value);
    }
    std::vector<std::int64_t> events;
};

template<class Runner, class Port, class Ordinary>
bool verifyOrdinaryFor()
{
    auto host = std::make_shared<Recorder<Port>>();
    Runner runner(host);
    const std::vector<std::vector<std::int64_t>> expected{
        {1, 10, 100, 200, 150, 20, 11, 101, 201, 151, 21, 12, 300},
        {1, 10, 100, 200, 20, 11, 101, 201, 151, 21, 12, 300},
        {1, 10, 100, 200, 300}, {1, 10, 100, 200}, {1, 10, 100, 200}};
    for (std::int64_t mode = 0; mode < 5; ++mode) {
        host->events.clear();
        const auto returned = runner.Run(mode);
        if (host->events != expected[static_cast<std::size_t>(mode)])
            return false;
        if (mode == 4) {
            if (!returned.is_failure() || returned.error() != "body fault")
                return false;
        } else {
            if (!returned.is_success() || returned.value() != (mode == 3 ? 7 : 9))
                return false;
        }
    }
    host->events.clear();
    const auto zeroTrip = runner.ZeroTrip();
    if (!zeroTrip.is_success() || zeroTrip.value() != 11 || host->events != std::vector<std::int64_t>{1, 12})
        return false;
    host->events.clear();
    const auto callClauses = runner.CallClauses();
    if (!callClauses.is_success() || callClauses.value() != 2
        || host->events != std::vector<std::int64_t>{1, 100, 21, 101, 22})
        return false;
    Ordinary ordinary;
    if (ordinary.Sum() != 6 || ordinary.Assigned() != 6 || ordinary.Step() != 16 || ordinary.Nested() != 15
        || ordinary.Omitted() != 3 || ordinary.MissingCondition() != 2
        || ordinary.MissingInitializer() != 3 || ordinary.MissingUpdate() != 3
        || ordinary.MixedControl() != 23 || ordinary.InnerForControl() != 2
        || ordinary.UnsupportedBreak() != 1 || ordinary.UnsupportedContinue() != 4 || ordinary.MutatedControl() != 2)
        return false;
    bool exhausted = false;
    try {
        (void)ordinary.Unbounded();
    } catch (const pipelang::model::ExecutionFailure& error) {
        exhausted = std::string(error.what()).find("budget") != std::string::npos;
    }
    bool overflowed = false;
    try {
        (void)ordinary.OverflowControl();
    } catch (const pipelang::model::ExecutionFailure& error) {
        overflowed = std::string(error.what()).find("overflow") != std::string::npos;
    }
    // A failed loop must release its invocation budget for the next call.
    return exhausted && overflowed && ordinary.Sum() == 6;
}
)cpp");
    const std::string allocationTrap = R"cpp(
void* operator new(std::size_t bytes)
{
    if (forbidForAllocations)
        std::abort();
    if (void* value = std::malloc(bytes))
        return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
)cpp";
    atomicWrite(output / "allocation.cpp", R"cpp(#include <cstddef>
#include <cstdlib>
#include <new>
bool forbidForAllocations = false;
)cpp" + allocationTrap);
    atomicWrite(output / "main.cpp", R"cpp(#include "header/Model.hpp"
#include "compiled/Model.hpp"
#include "checks.hpp"
int main()
{
    using Header = for_header::native_numeric::ForLoops::Queries;
    using Compiled = for_compiled::native_numeric::ForLoops::Queries;
    forbidForAllocations = true;
    const bool numeric = verifyNumericFor<Header>() && verifyNumericFor<Compiled>();
    forbidForAllocations = false;
    if (!numeric)
        return 1;
    const bool ordinary = verifyOrdinaryFor<for_header::source::ForLoops::Runner,
        for_header::source::ForLoops::Events, for_header::source::ForLoops::Queries>()
        && verifyOrdinaryFor<for_compiled::source::ForLoops::Runner,
            for_compiled::source::ForLoops::Events, for_compiled::source::ForLoops::Queries>();
    return ordinary ? 0 : 2;
}
)cpp");
    atomicWrite(output / "standalone.cpp", R"cpp(#include "header/Numeric.hpp"
#include "compiled/Numeric.hpp"
#include "checks.hpp"
#if defined(PIPELANG_ACTIVE_HOST_FLOATING)
#error Integer for headers must not impose active floating environment requirements
#endif
using Header = for_header::numeric_header::native_numeric::ForLoops::Queries;
using Compiled = for_compiled::numeric_header::native_numeric::ForLoops::Queries;
static_assert(verifyNumericFor<Header>() && verifyNumericFor<Compiled>());
int main()
{
    forbidForAllocations = true;
    const bool valid = verifyNumericFor<Header>() && verifyNumericFor<Compiled>();
    forbidForAllocations = false;
    return valid ? 0 : 1;
}
)cpp");
    atomicWrite(output / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.20)
project(ForLoops LANGUAGES CXX)
find_package(Threads REQUIRED)
option(FOR_LOOP_SANITIZERS "Instrument for loop consumers" OFF)
if(FOR_LOOP_SANITIZERS)
    add_compile_options(-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED
    PATHS "${FOR_LOOP_SDK_PREFIX}/lib/cmake/PipeLangModel" NO_DEFAULT_PATH)
pipelang_add_model(for_compiled PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCE_ROOT src ENTRY Queries.pipe NAMESPACE for_compiled
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/compiled" COMPILATION COMPILED)
add_executable(for_loops main.cpp allocation.cpp)
add_executable(for_loop_headers standalone.cpp allocation.cpp)
foreach(target for_loops for_loop_headers)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${FOR_LOOP_SDK_PREFIX}/include")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
endforeach()
target_link_libraries(for_loops PRIVATE Threads::Threads for_compiled)
)cmake");
    invoke({nativeTool("cmake").string(), "-S", output.string(), "-B", (output / "initial-normal").string(),
        "-DFOR_LOOP_SDK_PREFIX=" + prefix.string(), "-DCMAKE_CXX_COMPILER=" + nativeTool("c++").string(), "-DCMAKE_BUILD_TYPE=Release"});
    invoke({nativeTool("cmake").string(), "--build", (output / "initial-normal").string(), "--parallel", "1"});
    invoke({nativeExecutable(output / "initial-normal/for_loops").string()});
    invoke({nativeExecutable(output / "initial-normal/for_loop_headers").string()});
    atomicWrite(output / "acceptance.json", canonical({{"status", "passed"}, {"normal", true},
        {"ordinary_header_and_compiled", true}, {"host_event_schedule", true}, {"cleanup_before_update", true},
        {"scope_and_header_rejections", true}, {"nested_control_flow", true}, {"omitted_clauses", true},
        {"budget_and_overflow_refusal", true}, {"conservative_native_exports", true},
        {"constexpr_integer_headers", true}, {"first_call_allocation_trap", true}}));
}
