#include "Model.hpp"

#include <pipelang/json/ReaderAdapter.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace api = pipelang_json_tests::source::PipeLang::Json;
using Reader = pipelang::json::ReaderAdapter<api::JsonReader, api::JsonReadLimits, api::JsonNode, api::JsonKind>;

int main() {
    try {
        auto reader = std::make_shared<Reader>();
        api::WireTests::WireInputSuite suite(reader);
        const auto wide = pipelang::model::ByteBuffer({0xff, 0xfe, '4', 0, '2', 0});
        const auto result = suite.Run(wide, {});
        if (!result.is_success())
            throw std::runtime_error(result.error());

        if (reader->nativeReader().liveDocuments() != 0 || pipelang::json::Reader::aggregateRetainedBytes() != 0)
            throw std::runtime_error("wide Source input leaked a document");

        std::cout << "Generated Source opted into UTF-16 while preserving actual input bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
