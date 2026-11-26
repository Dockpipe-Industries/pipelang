// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/json/Reader.hpp>
#include <pipelang/model/native_host.hpp>

#include <array>
#include <chrono>
#include <stdexcept>

using namespace pipelang::native_app_v19;
using pipelang::model::CancellationToken;

extern "C" const HostedModel proof_json_wire;

namespace {

class JsonReader final : public HostedService {
public:
    using Response = pipelang::model::Result<HostedServiceValue>;

    std::string_view interfaceName() const noexcept override { return "PipeLang.Json.JsonReader"; }
    std::size_t liveDocuments() const { return reader_.liveDocuments(); }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        try {
            if (signature == "BeginDocument(PipeLang.Json.JsonReadLimits)" && arguments.size() == 1) {
                const auto& source = std::get<HostedValue>(arguments[0]);
                const pipelang::json::ReadLimits limits{
                    source.field("inputBytes").integer(), source.field("nodes").integer(),
                    source.field("depth").integer(), source.field("decodedBytes").integer(),
                    source.field("nameBytes").integer(), source.field("retainedBytes").integer(),
                    source.field("detectUnicodeEncoding").boolean()
                };
                return Response::Success(reader_.begin(limits));
            }
            if (signature == "AppendDocument(int,ByteBuffer)" && arguments.size() == 2) {
                reader_.append(std::get<std::int64_t>(arguments[0]), std::get<HostedValue>(arguments[1]).bytes());
                return Response::Success(HostedValue{});
            }
            if (signature == "CompleteDocument(int,CancellationToken)" && arguments.size() == 2) {
                const auto& token = std::get<CancellationToken>(arguments[1]);
                const auto node = reader_.complete(std::get<std::int64_t>(arguments[0]), [&] {
                    return token.is_cancelled();
                });
                return Response::Success(HostedValue::fromRecord("PipeLang.Json.JsonNode", {
                    {"document", HostedValue::fromInteger(node.document)}, {"node", HostedValue::fromInteger(node.node)}
                }));
            }
            if (signature == "InputSlice(int,int,int)" && arguments.size() == 3) {
                const auto original = reader_.inputSlice(std::get<std::int64_t>(arguments[0]),
                    std::get<std::int64_t>(arguments[1]), std::get<std::int64_t>(arguments[2]));
                return Response::Success(HostedValue::fromBytes(original));
            }
            if (signature == "Int64Value(PipeLang.Json.JsonNode)" && arguments.size() == 1) {
                const auto& node = std::get<HostedValue>(arguments[0]);
                return Response::Success(reader_.integer({node.field("document").integer(), node.field("node").integer()}));
            }
            if (signature == "CloseDocument(int)" && arguments.size() == 1) {
                reader_.close(std::get<std::int64_t>(arguments[0]));
                return Response::Success(HostedValue{});
            }
            return Response::Failure("unexpected JSON proof service call");
        } catch (const pipelang::json::ReadFailure& error) {
            if (std::string_view(error.what()) == "json.cancelled")
                return Response::Cancelled(error.what());
            return Response::Failure(error.what());
        }
    }

private:
    pipelang::json::Reader reader_;
};

HostedResult run(const HostedInstance& instance, pipelang::model::TaskScope& scope, std::int64_t scenario,
                 CancellationToken token = {})
{
    const std::array<HostedArgument, 2> arguments{scenario, token};
    const auto task = instance.start(scope, arguments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task.completed()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("JSON wire proof timed out");
        scope.poll();
    }
    return task.result();
}

}

int main()
{
    const auto reader = std::make_shared<JsonReader>();
    const std::array<HostedConstructorArgument, 1> arguments{reader};
    const auto instance = HostedInstance::bind(proof_json_wire, arguments);
    if (!instance.is_success())
        throw std::runtime_error(instance.error());
    pipelang::model::TaskScope scope;
    const auto valid = run(instance.value(), scope, 0);
    if (!valid.is_success())
        throw std::runtime_error(valid.error());
    if (valid.value() != 42 || reader->liveDocuments() != 0)
        throw std::runtime_error("valid UTF-16 JSON did not return 42 and close its document");
    const auto invalid = run(instance.value(), scope, 1);
    if (!invalid.is_failure() || invalid.error() != "json.syntax" || reader->liveDocuments() != 0 ||
        pipelang::json::Reader::aggregateRetainedBytes() != 0)
        throw std::runtime_error("rejected UTF-16 JSON retained a document or returned the wrong fault");

    pipelang::model::CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled = run(instance.value(), scope, 2, cancellation.token());
    if (!cancelled.is_cancelled() || cancelled.error() != "json.cancelled" || reader->liveDocuments() != 0 ||
        pipelang::json::Reader::aggregateRetainedBytes() != 0)
        throw std::runtime_error("cancelled UTF-16 JSON retained a document or returned the wrong fault");
}
