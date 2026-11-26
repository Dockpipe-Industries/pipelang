// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "HostedJsonReader.hpp"
#include "FixtureInputs.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace native_json;
extern "C" const HostedModel proof_parser;

class Fixture final : public HostedService {
public:
    explicit Fixture(HostedJsonReader& reader) : reader_(reader), inputs_(json_contracts::fixtureInputs()) {}

    std::string_view interfaceName() const noexcept override { return "PipeLang.Json.Tests.FixtureBytes"; }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        const auto integer = [&](std::size_t index) { return std::get<std::int64_t>(arguments[index]); };
        if (signature == "CompleteCancelled(int)") {
            pipelang::model::CancellationSource cancellation;
            cancellation.cancel();
            const std::array<HostedServiceValue, 2> args{integer(0), cancellation.token()};
            return reader_.invoke("CompleteDocument(int,CancellationToken)", args);
        }
        const auto scenario = integer(0);
        if (scenario < 0 || static_cast<std::uint64_t>(scenario) >= inputs_.size())
            return Response::Failure("fixture.scenario");
        const auto& bytes = inputs_[scenario];
        if (signature == "Length(int)")
            return Response::Success(static_cast<std::int64_t>(bytes.size()));
        if (signature != "Slice(int,int,int)")
            throw std::runtime_error("unexpected fixture signature");
        const auto start = integer(1);
        const auto count = integer(2);
        const auto length = static_cast<std::int64_t>(bytes.size());
        if (start < 0 || count < 0 || count > 1048576 || start > length || count > length - start)
            return Response::Failure("fixture.slice");
        const auto* buffer = reinterpret_cast<const std::uint8_t*>(bytes.data());
        return Response::Success(HostedValue::fromBytes(std::span(buffer + start, static_cast<std::size_t>(count))));
    }

private:
    HostedJsonReader& reader_;
    std::array<std::string, 41> inputs_;
};

int main()
{
    try {
        const auto reader = std::make_shared<HostedJsonReader>(pipelang::json::ReaderOptions{2});
        const auto other = std::make_shared<HostedJsonReader>();
        const auto fixture = std::make_shared<Fixture>(*reader);
        const std::array<HostedConstructorArgument, 3> constructor{reader, other, fixture};
        const auto suite = HostedInstance::bind(proof_parser, constructor);
        if (!suite.is_success())
            throw std::runtime_error(suite.error());
        pipelang::model::TaskScope scope;
        const std::array<HostedArgument, 1> arguments{CancellationToken{}};
        const auto result = suite.value().startValue(scope, arguments).result();
        if (!result.is_success())
            throw std::runtime_error(result.error());
        if (result.value().size() != 42)
            throw std::runtime_error("source test registration count changed");
        for (std::size_t index = 0; index < result.value().size(); ++index) {
            const auto test = result.value().at(index);
            if (test.field("outcome").enumerator() == "Passed")
                continue;
            std::cerr << test.field("id").text();
            for (const auto* name : {"primaryFailure", "cleanupFailure"}) {
                const auto failure = test.field(name);
                if (failure.hasValue())
                    std::cerr << ": " << failure.value().field("message").text();
            }
            std::cerr << '\n';
            return 1;
        }
        if (reader->reader().liveDocuments() != 0 || other->reader().liveDocuments() != 0)
            throw std::runtime_error("source cleanup left a document open");
        std::cout << "PipeLang.Json generated ABI/source query contracts: 42 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
