// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang/json/Reader.hpp>
#include <pipelang/model/native_host.hpp>

namespace native_json {

using namespace pipelang::native_app_v19;
using pipelang::model::CancellationToken;
using Response = pipelang::model::Result<HostedServiceValue>;

inline pipelang::json::NodeHandle node(const HostedServiceValue& packed)
{
    const auto& value = std::get<HostedValue>(packed);
    return {value.field("document").integer(), value.field("node").integer()};
}

inline HostedValue pack(pipelang::json::NodeHandle source)
{
    return HostedValue::fromRecord("PipeLang.Json.JsonNode", {
        {"document", HostedValue::fromInteger(source.document)},
        {"node", HostedValue::fromInteger(source.node)}
    });
}

inline Response jsonFailure(const pipelang::json::ReadFailure& error)
{
    if (std::string_view(error.what()) == "json.cancelled")
        return Response::Cancelled(error.what());
    return Response::Failure(error.what());
}

class HostedJsonReader final : public HostedService {
public:
    explicit HostedJsonReader(pipelang::json::ReaderOptions options = {}) : reader_(options) {}

    std::string_view interfaceName() const noexcept override { return "PipeLang.Json.JsonReader"; }
    pipelang::json::Reader& reader() noexcept { return reader_; }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        try {
            return dispatch(signature, arguments);
        } catch (const pipelang::json::ReadFailure& error) {
            return jsonFailure(error);
        }
    }

private:
    Response dispatch(std::string_view signature, std::span<const HostedServiceValue> arguments)
    {
        const auto integer = [&](std::size_t index) { return std::get<std::int64_t>(arguments[index]); };
        const auto value = [&](std::size_t index) -> const HostedValue& { return std::get<HostedValue>(arguments[index]); };
        const auto handle = [&] { return node(arguments[0]); };
        if (signature == "BeginDocument(PipeLang.Json.JsonReadLimits)") {
            const auto& source = value(0);
            const pipelang::json::ReadLimits limits{
                source.field("inputBytes").integer(), source.field("nodes").integer(),
                source.field("depth").integer(), source.field("decodedBytes").integer(),
                source.field("nameBytes").integer(), source.field("retainedBytes").integer(),
                source.field("detectUnicodeEncoding").boolean()
            };
            return Response::Success(reader_.begin(limits));
        }
        if (signature == "AppendDocument(int,ByteBuffer)") {
            reader_.append(integer(0), value(1).bytes());
            return Response::Success(HostedValue{});
        }
        if (signature == "CompleteDocument(int,CancellationToken)") {
            const auto& token = std::get<CancellationToken>(arguments[1]);
            return Response::Success(pack(reader_.complete(integer(0), [&] { return token.is_cancelled(); })));
        }
        if (signature == "CloseDocument(int)") {
            reader_.close(integer(0));
            return Response::Success(HostedValue{});
        }
        if (signature == "InputLength(int)")
            return Response::Success(reader_.inputLength(integer(0)));
        if (signature == "InputSlice(int,int,int)")
            return Response::Success(HostedValue::fromBytes(reader_.inputSlice(integer(0), integer(1), integer(2))));
        if (signature == "Kind(PipeLang.Json.JsonNode)") {
            constexpr std::array<std::string_view, 6> names{"Null", "Boolean", "Number", "String", "Object", "Array"};
            const auto kind = static_cast<std::int64_t>(reader_.kind(handle()));
            return Response::Success(HostedValue::fromEnum("PipeLang.Json.JsonKind", kind, std::string(names.at(kind))));
        }
        if (signature == "Count(PipeLang.Json.JsonNode)")
            return Response::Success(reader_.count(handle()));
        if (signature == "At(PipeLang.Json.JsonNode,int)")
            return Response::Success(pack(reader_.at(handle(), integer(1))));
        if (signature == "FindMember(PipeLang.Json.JsonNode,string)") {
            const auto found = reader_.find(handle(), std::string(std::get<std::string>(arguments[1])));
            if (found)
                return Response::Success(HostedValue::fromOptional("Optional<PipeLang.Json.JsonNode>", pack(*found)));
            return Response::Success(HostedValue::fromOptional("Optional<PipeLang.Json.JsonNode>"));
        }
        if (signature == "BoolValue(PipeLang.Json.JsonNode)")
            return Response::Success(reader_.boolean(handle()));
        if (signature == "Int64Value(PipeLang.Json.JsonNode)")
            return Response::Success(reader_.integer(handle()));
        if (signature == "FiniteFloatValue(PipeLang.Json.JsonNode)")
            return Response::Success(HostedValue::fromFloat(reader_.finiteFloat(handle())));
        if (signature == "ScalarLength(PipeLang.Json.JsonNode)")
            return Response::Success(reader_.scalarLength(handle()));
        if (signature == "ScalarSlice(PipeLang.Json.JsonNode,int,int)")
            return Response::Success(HostedValue::fromBytes(reader_.scalarSlice(handle(), integer(1), integer(2))));
        if (signature == "NameLength(PipeLang.Json.JsonNode)")
            return Response::Success(reader_.nameLength(handle()));
        if (signature == "NameSlice(PipeLang.Json.JsonNode,int,int)")
            return Response::Success(HostedValue::fromBytes(reader_.nameSlice(handle(), integer(1), integer(2))));
        return Response::Failure("unexpected native JSON reader signature: " + std::string(signature));
    }

    pipelang::json::Reader reader_;
};

}
