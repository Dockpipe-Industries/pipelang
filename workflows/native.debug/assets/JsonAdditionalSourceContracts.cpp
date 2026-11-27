// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "HostedJsonReader.hpp"
#include <pipelang/json/DocumentWriter.hpp>

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace native_json;
using pipelang::json::DocumentWriter;
using pipelang::json::RenderLayout;
extern "C" const HostedModel proof_string_index;
extern "C" const HostedModel proof_render;
extern "C" const HostedModel proof_writer;
extern "C" const HostedModel proof_wire;

namespace {

bool cancelled(const HostedServiceValue& token)
{
    return std::get<CancellationToken>(token).is_cancelled();
}

auto outputTo(std::string& output)
{
    return [&](std::span<const std::uint8_t> bytes) {
        output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
}

class Indexer final : public HostedService {
public:
    explicit Indexer(pipelang::json::Reader& reader) : reader_(reader) {}
    std::string_view interfaceName() const noexcept override { return "PipeLang.Json.JsonStringIndexer"; }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        try {
            if (signature == "BeginStringIndex(int,PipeLang.Json.JsonStringIndexLimits)") {
                const auto document = std::get<std::int64_t>(arguments[0]);
                const auto& limits = std::get<HostedValue>(arguments[1]);
                const auto index = reader_.beginStringIndex(document,
                    {limits.field("entries").integer(), limits.field("retainedBytes").integer()});
                return Response::Success(HostedValue::fromRecord("PipeLang.Json.JsonStringIndex", {
                    {"document", HostedValue::fromInteger(index.document)},
                    {"generation", HostedValue::fromInteger(index.generation)}
                }));
            }
            const auto& indexed = std::get<HostedValue>(arguments[0]);
            const pipelang::json::StringIndexHandle index{indexed.field("document").integer(),
                indexed.field("generation").integer()};
            if (signature == "CloseStringIndex(PipeLang.Json.JsonStringIndex)") {
                reader_.closeStringIndex(index);
                return Response::Success(HostedValue{});
            }
            const auto source = node(arguments[1]);
            const auto stop = [&] { return cancelled(arguments[2]); };
            if (signature == "AddString(PipeLang.Json.JsonStringIndex,PipeLang.Json.JsonNode,CancellationToken)")
                return Response::Success(reader_.addString(index, source, stop));
            if (signature == "ContainsString(PipeLang.Json.JsonStringIndex,PipeLang.Json.JsonNode,CancellationToken)")
                return Response::Success(reader_.containsString(index, source, stop));
            return Response::Failure("unexpected string-index signature");
        } catch (const pipelang::json::ReadFailure& error) {
            return jsonFailure(error);
        }
    }

private:
    pipelang::json::Reader& reader_;
};

class Renderer final : public HostedService {
public:
    Renderer(pipelang::json::Reader& reader, std::string& output)
        : reader_(reader), output_(output) {}

    std::string_view interfaceName() const noexcept override { return "PipeLang.Json.JsonValueRenderer"; }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        if (signature != "WriteValue(PipeLang.Json.JsonNode,PipeLang.Json.JsonRenderOptions,CancellationToken)")
            return Response::Failure("unexpected value renderer signature");
        try {
            const auto& option = std::get<HostedValue>(arguments[1]);
            const auto layout = static_cast<RenderLayout>(option.field("layout").ordinal());
            const auto bytes = reader_.render(node(arguments[0]),
                {static_cast<std::size_t>(option.field("maximumBytes").integer()), layout, option.field("ascii").boolean(),
                    static_cast<std::size_t>(option.field("initialDepth").integer())},
                outputTo(output_), [&] { return cancelled(arguments[2]); });
            return Response::Success(static_cast<std::int64_t>(bytes));
        } catch (const pipelang::json::ReadFailure& error) {
            return jsonFailure(error);
        }
    }

private:
    pipelang::json::Reader& reader_;
    std::string& output_;
};

class Writer final : public HostedService {
public:
    explicit Writer(DocumentWriter& writer) : writer_(writer) {}

    std::string_view interfaceName() const noexcept override { return "PipeLang.Json.JsonDocumentOutput"; }

    Response invoke(std::string_view signature, std::span<const HostedServiceValue> arguments) override
    {
        try {
            if (signature == "Finish()")
                return Response::Success(static_cast<std::int64_t>(writer_.finish()));
            if (signature == "BeginObject()")
                writer_.beginObject();
            else if (signature == "BeginArray()")
                writer_.beginArray();
            else if (signature == "Name(string)")
                writer_.name(std::get<std::string>(arguments[0]));
            else if (signature == "EndObject()")
                writer_.endObject();
            else if (signature == "EndArray()")
                writer_.endArray();
            else if (signature == "WriteText(string)")
                writer_.text(std::get<std::string>(arguments[0]));
            else if (signature == "WriteInteger(int)")
                writer_.integer(std::get<std::int64_t>(arguments[0]));
            else if (signature == "WriteFinite(float)")
                writer_.finite(std::get<HostedValue>(arguments[0]).floating());
            else if (signature == "WriteBoolean(bool)")
                writer_.boolean(std::get<bool>(arguments[0]));
            else if (signature == "WriteNull()")
                writer_.nullValue();
            else if (signature == "WriteValue(PipeLang.Json.JsonNode,CancellationToken)")
                writer_.borrowed(node(arguments[0]), [&] { return cancelled(arguments[1]); });
            else
                return Response::Failure("unexpected document output signature");
            return Response::Success(HostedValue{});
        } catch (const pipelang::json::ReadFailure& error) {
            return jsonFailure(error);
        }
    }

private:
    DocumentWriter& writer_;
};

void require(bool condition, std::string_view error)
{
    if (!condition)
        throw std::runtime_error(std::string(error));
}

HostedInstance bind(const HostedModel& model, std::initializer_list<HostedConstructorArgument> arguments)
{
    auto bound = HostedInstance::bind(model, arguments);
    require(bound.is_success(), bound.is_success() ? "" : bound.error());
    return std::move(bound).value();
}

void requireClosed(HostedJsonReader& reader)
{
    require(reader.reader().liveDocuments() == 0 && pipelang::json::Reader::aggregateRetainedBytes() == 0,
            "source JSON suite leaked native reader storage");
}

HostedValue bytes(std::string_view input)
{
    const auto* data = reinterpret_cast<const std::uint8_t*>(input.data());
    return HostedValue::fromBytes(std::span(data, input.size()));
}

void stringIndex(const std::shared_ptr<HostedJsonReader>& reader)
{
    const auto indexer = std::make_shared<Indexer>(reader->reader());
    auto suite = bind(proof_string_index, {reader, indexer});
    pipelang::model::TaskScope scope;
    const auto checked = [](const HostedValueResult& result) {
        require(result.is_success(), result.is_success() ? "" : result.error());
    };
    const auto call = [&](std::string_view name, std::initializer_list<HostedCallArgument> args) {
        checked(suite.startMethod(scope, name, args).result());
    };
    std::string input = "[";
    for (std::int64_t element = 0; element < 4098; ++element) {
        if (element != 0)
            input += ',';
        input += "\"k" + std::to_string(element) + "\"";
    }
    input += ']';
    checked(suite.startWithValues(scope, std::array<HostedCallArgument, 3>{bytes(input), std::int64_t{4097}, CancellationToken{}}).result());
    pipelang::model::CancellationSource cancellation;
    cancellation.cancel();
    call("Cancelled", {std::int64_t{0}, cancellation.token()});
    for (std::int64_t element = 0; element < 4097; ++element)
        call("Insert", {element, HostedValue::fromBoolean(true), CancellationToken{}});
    call("Insert", {std::int64_t{4096}, HostedValue::fromBoolean(false), CancellationToken{}});
    call("AtCapacity", {std::int64_t{4097}, CancellationToken{}});
    call("Insert", {std::int64_t{0}, HostedValue::fromBoolean(false), CancellationToken{}});
    call("Close", {cancellation.token()});

    const auto encoded = bytes(R"(["a","\u0061","a\u0000b","a\u0000c","A","é","e\u0301"])");
    checked(suite.startWithValues(scope, std::array<HostedCallArgument, 3>{encoded, std::int64_t{6}, CancellationToken{}}).result());
    call("Insert", {std::int64_t{0}, HostedValue::fromBoolean(true), CancellationToken{}});
    call("Insert", {std::int64_t{1}, HostedValue::fromBoolean(false), CancellationToken{}});
    for (std::int64_t element = 2; element < 7; ++element)
        call("Insert", {element, HostedValue::fromBoolean(true), CancellationToken{}});
    call("Insert", {std::int64_t{1}, HostedValue::fromBoolean(false), CancellationToken{}});
    call("CrossDocumentLookup", {CancellationToken{}, cancellation.token()});
    call("Close", {CancellationToken{}});
    requireClosed(*reader);
    std::cout << "JSON generated optional string-index ABI/source contracts passed\n";
}

void valueRender(const std::shared_ptr<HostedJsonReader>& reader)
{
    std::string output;
    auto suite = bind(proof_render, {reader, std::make_shared<Renderer>(reader->reader(), output)});
    pipelang::model::TaskScope scope;
    pipelang::model::CancellationSource cancellation;
    cancellation.cancel();
    const std::array<HostedArgument, 2> args{CancellationToken{}, cancellation.token()};
    const auto result = suite.startValue(scope, args).result();
    require(result.is_success(), result.is_success() ? "" : result.error());
    require(output == R"({"a": "\u20ac", "z": []})", "rendered JSON bytes differ");
    requireClosed(*reader);
    std::cout << "Generated JSON value renderer Source contract passed\n";
}

void documentWriter(const std::shared_ptr<HostedJsonReader>& reader, const char* path)
{
    constexpr std::string_view expected = R"({
  "case": "demo",
  "count": 8,
  "data": {
    "a": "\u2028",
    "z": [
      null,
      false
    ]
  },
  "statuses": [
    true,
    null,
    0.9
  ]
}
)";
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    DocumentWriter writer(reader->reader(), {32768, RenderLayout::Indented, true},
        [&](std::span<const std::uint8_t> fragment) {
            file.write(reinterpret_cast<const char*>(fragment.data()), static_cast<std::streamsize>(fragment.size()));
        });
    auto suite = bind(proof_writer, {reader, std::make_shared<Writer>(writer)});
    pipelang::model::TaskScope scope;
    const std::array<HostedArgument, 1> argument{CancellationToken{}};
    const auto result = suite.start(scope, argument).result();
    require(result.is_success(), result.is_success() ? "" : result.error());
    file.close();
    std::ifstream observed(path, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(observed), {}};
    require(contents == expected && result.value() == static_cast<std::int64_t>(expected.size()), "writer output differs");
    requireClosed(*reader);

    auto captured = DocumentWriter::capture(reader->reader(), {32768, RenderLayout::Indented, true, 1},
        [&](DocumentWriter& nested) {
            const auto model = bind(proof_writer, {reader, std::make_shared<Writer>(nested)});
            const std::array<HostedCallArgument, 1> projectArgs{CancellationToken{}};
            const auto projected = model.startMethod(scope, "Project", projectArgs).result();
            require(projected.is_success(), projected.is_success() ? "" : projected.error());
        });
    requireClosed(*reader);
    std::string output;
    pipelang::json::Reader other;
    DocumentWriter envelope(other, {32768, RenderLayout::Indented, true}, outputTo(output));
    envelope.beginArray();
    envelope.captured(captured);
    envelope.endArray();
    (void)envelope.finish();
    std::string nestedExpected = "[\n";
    std::size_t offset = 0;
    while (offset < expected.size()) {
        const auto end = expected.find('\n', offset);
        nestedExpected += "  ";
        nestedExpected.append(expected.substr(offset, end + 1 - offset));
        offset = end + 1;
    }
    nestedExpected += "]\n";
    require(output == nestedExpected, "captured output differs");
    std::cout << "Structured JSON written by generated PipeLang Source to real output\n";
}

void wireInput(const std::shared_ptr<HostedJsonReader>& reader)
{
    const auto suite = bind(proof_wire, {reader});
    const std::array<std::uint8_t, 6> wire{0xff, 0xfe, '4', 0, '2', 0};
    const std::array<HostedCallArgument, 2> args{HostedValue::fromBytes(wire), CancellationToken{}};
    pipelang::model::TaskScope scope;
    const auto invoked = suite.startWithValues(scope, args).result();
    require(invoked.is_success(), invoked.is_success() ? "" : invoked.error());
    requireClosed(*reader);
    std::cout << "Generated Source opted into UTF-16 while preserving actual input bytes\n";
}

}

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "select a JSON output file");
        const auto reader = std::make_shared<HostedJsonReader>();
        stringIndex(reader);
        valueRender(reader);
        documentWriter(reader, argv[1]);
        wireInput(reader);
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
