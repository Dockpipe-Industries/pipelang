#include "Model.hpp"
#include <pipelang/json/DocumentWriterAdapter.hpp>
#include <pipelang/json/ReaderAdapter.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace api = pipelang_json_tests::source::PipeLang::Json;
using Reader = pipelang::json::ReaderAdapter<api::JsonReader, api::JsonReadLimits, api::JsonNode, api::JsonKind>;
using Output = pipelang::json::DocumentWriterAdapter<api::JsonDocumentOutput, api::JsonNode>;

int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("Select the isolated JSON writer fixture output");

        std::ofstream file(argv[1], std::ios::binary | std::ios::out);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        auto reader = std::make_shared<Reader>();
        auto output = std::make_shared<Output>(reader->nativeReader(),
            pipelang::json::RenderOptions{32768, pipelang::json::RenderLayout::Indented, true},
            [&](std::span<const std::uint8_t> bytes) {
                file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            });
        api::WriterTests::JsonWriterSuite suite(reader, output);
        const auto result = suite.Write({});
        if (!result.is_success())
            throw std::runtime_error(result.error());
        file.close();

        const std::string_view expected = R"({
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
        std::ifstream recorded(argv[1], std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(recorded), {}};
        if (content != expected || result.value() != static_cast<std::int64_t>(expected.size()) ||
            reader->nativeReader().liveDocuments() != 0)
            throw std::runtime_error("Actual Source writer bytes or document lifetime differ");

        using pipelang::json::DocumentWriter;
        using pipelang::json::RenderLayout;
        auto captured = DocumentWriter::capture(reader->nativeReader(), {32768, RenderLayout::Indented, true, 1},
            [&](DocumentWriter& writer) {
                auto bound = std::make_shared<Output>(writer);
                api::WriterTests::JsonWriterSuite nested(reader, bound);
                const auto projected = nested.Project({});
                if (!projected.is_success())
                    throw std::runtime_error(projected.error());
            });
        if (reader->nativeReader().liveDocuments() != 0)
            throw std::runtime_error("Capture retained the borrowed Source document");

        std::string composite;
        pipelang::json::Reader other;
        DocumentWriter envelope(other, {32768, RenderLayout::Indented, true},
            [&](std::span<const std::uint8_t> bytes) {
                composite.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            });
        envelope.beginArray();
        envelope.captured(captured);
        envelope.endArray();
        (void)envelope.finish();

        std::string nestedExpected = "[\n";
        std::size_t offset = 0;
        while (offset < expected.size()) {
            const auto lineEnd = expected.find('\n', offset);
            nestedExpected += "  ";
            nestedExpected.append(expected.substr(offset, lineEnd + 1 - offset));
            offset = lineEnd + 1;
        }
        nestedExpected += "]\n";
        if (composite != nestedExpected)
            throw std::runtime_error("Captured Source value did not preserve nesting and exact bytes");

        std::cout << "Structured JSON written by generated PipeLang Source to real output\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
