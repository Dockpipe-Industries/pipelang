// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <thread>

using namespace pipelang::native_app_v19;
using pipelang::model::CancellationToken;
using pipelang::model::Result;
using pipelang::model::TaskScope;

extern "C" const HostedModel proof_dictionaries;

namespace {

HostedValue entry(std::string key, std::int64_t value)
{
    return HostedValue::fromRecord("KeyValuePair<string,int>", {
        {"key", HostedValue::fromText(std::move(key))},
        {"value", HostedValue::fromInteger(value)}
    });
}

class Port final : public HostedService {
public:
    std::string_view interfaceName() const noexcept override { return "NativeProof.Dictionaries.DictionaryPort"; }

    Result<HostedServiceValue> invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        using Outcome = Result<HostedServiceValue>;
        if (signature != "Send(Dictionary<string,int>,CancellationToken)" || arguments.size() != 2)
            return Outcome::Failure("unexpected dictionary service call");
        const auto* request = std::get_if<HostedValue>(&arguments.front());
        if (!request || request->kind() != HostedValue::Kind::Dictionary || request->size() != 2 ||
            request->at(0).field("key").text() != "étape" || request->at(0).field("value").integer() != 42 ||
            request->at(1).field("key").text() != "other" || request->at(1).field("value").integer() != 17)
            return Outcome::Failure("host request lost dictionary entries");
        std::vector<HostedValue> entries{entry("new", 43), entry("earlier", 16)};
        for (std::int64_t index = 0; index < 1024; ++index)
            entries.push_back(entry("entry-" + std::to_string(index), index));
        return Outcome::Success(HostedValue::fromDictionary("Dictionary<string,int>", std::move(entries)));
    }
};

}

int main()
{
    HostedValue detached;
    {
        const std::array<HostedConstructorArgument, 1> services{std::make_shared<Port>()};
        const auto hosted = HostedInstance::bind(proof_dictionaries, services);
        if (!hosted.is_success())
            return 1;
        TaskScope scope;
        const std::array<HostedArgument, 2> arguments{std::int64_t{0}, CancellationToken{}};
        const auto task = hosted.value().startValue(scope, arguments);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.completed()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return 2;
            scope.poll();
            if (!task.completed())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto result = task.result();
        if (!result.is_success()) {
            std::cerr << result.error() << '\n';
            return 3;
        }
        detached = result.value();
    }
    if (detached.kind() != HostedValue::Kind::Dictionary || detached.size() != 1026 ||
        detached.at(1).field("key").text() != "earlier" || detached.at(1).field("value").integer() != 16 ||
        detached.at(1025).field("key").text() != "entry-1023" || detached.at(1025).field("value").integer() != 1023)
        return 4;
    std::cout << "Hosted dictionary preserved entry types and order across suspension.\n";
}
