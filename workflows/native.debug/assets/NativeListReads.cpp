// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_list_reads;

void require(bool success, const char* message)
{
    if (!success)
        throw std::runtime_error(message);
}

template<class Value>
Value take(model::Result<Value> result)
{
    if (!result.is_success())
        throw std::runtime_error(result.error());
    return std::move(result).value();
}

class ProjectionHost final : public native::HostedService {
public:
    int calls = 0;
    std::function<void()> callback;

    std::string_view interfaceName() const noexcept override { return "ListReadObjects.ProjectionHost"; }

    model::Result<native::HostedServiceValue> invoke(std::string_view,
        std::span<const native::HostedServiceValue>) override
    {
        ++calls;
        if (callback)
            callback();
        return model::Result<native::HostedServiceValue>::Success(native::HostedValue{});
    }
};

int main()
{
    try {
        const auto host = std::make_shared<ProjectionHost>();
        const std::array<native::HostedConstructorArgument, 1> inputs{host};
        const auto instance = take(native::HostedInstance::bind(proof_list_reads, inputs));
        model::TaskScope tasks;
        const auto invoke = [&](std::string_view method, std::span<const native::HostedCallArgument> arguments = {}) {
            const auto task = instance.startMethod(tasks, method, arguments);
            require(task.completed(), "synchronous fixture did not settle");
            return task.result();
        };
        const auto number = [&](std::string_view method) { return take(invoke(method)).integer(); };
        const auto indexed = [&](std::string_view method, std::int64_t index) {
            const std::array<native::HostedCallArgument, 1> arguments{index};
            return take(invoke(method, arguments)).integer();
        };
        for (int repetition = 0; repetition < 256; ++repetition) {
            require(indexed("Read", 0) == 6 && indexed("Read", 1) == 9, "read from record list");
            require(number("TemporaryResult") == 9, "read from temporary Result");
            require(indexed("CellValue", 0) == 31 && indexed("CellValue", 1) == 47, "list lost a receiver");
        }
        require(indexed("LocalReads", 128) == 960 && indexed("ScalarReads", 128) == 1920 &&
                    indexed("RecordReads", 128) == 1152 && indexed("NestedRecordReads", 128) == 512,
                "list reads changed the sequence or values");
        require(number("NestedValues") == 17, "nested list lost an element");
        require(invoke("RollbackSelected").is_failure() && indexed("CellValue", 0) == 31, "selected receiver escaped rollback");
        require(number("RetainSelectedCell") == 48, "selected receiver reclaimed before last use");
        require(number("ReplaceIndex") == 20 && number("Size") == 1 && number("First") == 90 && number("Effects") == 1,
                "index expression changed the earlier list expression");
        take(invoke("Reset"));
        require(invoke("AppendIndex").is_failure() && number("Size") == 2 && number("First") == 10 &&
                    number("Effects") == 0, "failing list access committed changes");
        for (std::int64_t index : {-1, 2}) {
            const std::array<native::HostedCallArgument, 1> arguments{index};
            const auto outcome = invoke("Read", arguments);
            require(outcome.is_failure() && outcome.error() == "list index outside admitted range", "invalid index diagnostic");
        }
        require(indexed("ProjectScalars", 128) == 896 && indexed("ProjectNested", 128) == 1408 &&
                    number("ProjectListIsolation") == 1 && number("TemporaryProjection") == 11,
                "receiver record projection changed its source");
        host->callback = [&] { (void)invoke("ProjectImageValue"); };
        const auto reentered = invoke("ProjectAcrossHost");
        require(reentered.is_failure() && reentered.error() == "model reentry during a synchronous host callback" &&
                    number("ProjectImageValue") == 11, "host reentry changed the receiver");

        host->callback = [] { throw std::runtime_error("projection host fault"); };
        const auto faulted = invoke("ProjectAcrossHost");
        require(faulted.is_failure() && faulted.error() == "projection host fault" &&
                    number("ProjectImageValue") == 11, "failed callback committed a projection");

        host->callback = {};
        const auto succeeded = take(invoke("ProjectAcrossHost"));
        require(succeeded.integer() == 11 &&
                    number("ProjectImageValue") == 23 && host->calls == 3, "projection did not traverse and commit across host");
        std::cout << "Original ListReadObjects receiver lifetimes and transactions passed.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
